#pragma once

// The service's remote-admin link to its hosted servers (ADR 0053 d5,
// proposed: both admin paths, through the service only). A running hosted
// server's opennova-serve opens retail's remote-admin console (CAdminServer)
// on the TCP port its game.cfg names, admitting the service alone (admin.cfg's
// ip_restrict; on a cloud machine its security group too). The service is that
// console's only client: owners never get the port or the login, and reach the
// console only through the site's routes, which run their commands on the link.
//
// An AdminLink keeps one logged-in session open to one server and speaks the
// protocol as the retail admin client RAT.exe does, through the engine's client
// codec (engine/net/admin/admin_protocol.h; docs/net/novaworld-net-re.md §6.9):
// the server's challenge, the login encrypted under it, then one NUL-ended
// command packet at a time (the server reads one packet per read and drops what
// follows it, so nothing is pipelined). The server marks no reply's end, so the
// link reads each command's replies by the per-verb count the dispatcher
// witnesses (admin_reply_plan): an exact count, or one that the first reply
// decides (PLAYER BAN <#>, GOTO MENUSTATE), and a 200 ms quiet gap only for the
// verbs whose count the line cannot tell (PLAYER BAN ALL, a PLAYER sub-verb the
// server does not know). Bytes that arrive between commands (a reply a
// non-retail server miscounted, the server's close) are read off before the
// next command goes, and the link watches its idle socket, so a server that
// went away is noticed at once and its side of the connection reset.
//
// Each link runs a thread of its own: at most ONNET_HOSTED_CAP links live
// (default 5), the apps' socket layer is blocking with per-socket timeouts
// (apps/common/net_sockets.h), and
// a thread per link keeps one dead or slow machine (a connect or a reply
// timing out) from holding up another server's commands, which one shared
// thread over blocking sockets would queue behind it.
//
// The thread logs in, reconnects with a backoff (1 s, doubling to 4 s) after a
// refused login or a lost session, polls the server every 15 s with GET
// GAMESTATE (its status: the session answers, and the server's scene), and runs
// the queued commands. A route waits at most its own budget (the routes' is
// 3 s) for its command; a command still queued when the wait ends is dropped,
// one already sent finishes and its replies still reach the console. The link
// ends with QUIT on a logged-in session, so the server closes the connection
// and frees its slot (a retail server keeps a FIN'd slot, D-NET-360), and
// with a reset otherwise.
//
// The console: every command a caller asks to record, its replies, and the
// link's status lines (logged in, the session lost or refused, the server's
// scene when it changes) go to an in-memory reply ring and from there to the
// server's hosted_server_console ring (hosted_repository.h, migration 0012).
// The admin password is a log secret (loggable.h's add_log_secret) for the
// link's lifetime, and never reaches a log line, a console line or a reply.
//
// AdminLinks holds the live links. The supervisor (supervisor.h) syncs it every
// pass with its running servers: a link comes up when a server reaches
// running, is replaced when a restart rendered a new login, and goes down as
// the server leaves running (at stopping, or a crash). Crow-free.

#include "hosted_repository.h"

#include "net_sockets.h"

#include <net/novaworld/db/sqlite.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::hosted {

// Where and as whom a link logs in.
struct AdminLinkTarget {
	std::string host;     // the machine's IPv4 address, as the service reaches it
	uint16_t port = 0;    // its game.cfg's remote_admin_port
	std::string user;     // admin.cfg's kAdminUser
	std::string password; // admin.cfg's password: never logged, never in a reply
	bool operator==(const AdminLinkTarget &o) const {
		return host == o.host && port == o.port && user == o.user && password == o.password;
	}
	bool operator!=(const AdminLinkTarget &o) const { return !(*this == o); }
};

struct AdminLinkConfig {
	int io_timeout_ms = 5000;        // the connect, and each reply a count says is coming
	int quiet_gap_ms = 200;          // the QuietGap framing's silence
	int status_interval_ms = 15000;  // GET GAMESTATE
	int idle_check_ms = 250;         // how often an idle session's socket is looked at
	int reconnect_min_ms = 1000;
	// The backoff stays short: a link lives only while its server is listed and running,
	// and the health check counts a link that is not up (three misses, 20 s apart).
	int reconnect_max_ms = 4000;
	int quit_timeout_ms = 2000;      // how long QUIT waits for the server's close
	size_t queue_max = 16;
	size_t ring_max = 256;           // the in-memory reply ring (and its unwritten tail)
	// Where the link's own log lines go, each masked of the log secrets first;
	// unset, stdout.
	std::function<void(const std::string &line)> log;
};

enum class AdminLinkState : uint8_t {
	Connecting, // logging in (the first time, or again after the backoff)
	Up,         // logged in: commands run
	Down,       // the last login failed or the session was lost; the backoff runs
	Stopped,    // ended
};
const char *admin_link_state_name(AdminLinkState state);

struct AdminLinkStatus {
	AdminLinkState state = AdminLinkState::Connecting;
	std::string game_state;   // the last poll's scene: "Game", "Menus" or "Unknown"; "" before one
	std::string last_poll_at; // UTC 'YYYY-MM-DD HH:MM:SS' of the last answered poll, "" before one
	std::string up_since;     // UTC, the session's login; "" while not up
	std::string error;        // why the last login or session failed; "" while up
};

enum class AdminCommandOutcome : uint8_t {
	Answered, // sent, and every reply its count names read (an ERROR or USAGE reply is an answer)
	Refused,  // no command a retail server takes in one packet (empty, a control byte, too long)
	LinkDown, // no logged-in session to send it on, or the session was lost on it
	Busy,     // the queue is full
	TimedOut, // no answer within the caller's wait (`sent` says whether it went out)
	Stopped,  // the link is ending
};
const char *admin_command_outcome_name(AdminCommandOutcome outcome);

struct AdminCommandResult {
	AdminCommandOutcome outcome = AdminCommandOutcome::LinkDown;
	bool sent = false;
	// Each reply packet's text, in the server's code page (cp1252), in order.
	std::vector<std::string> replies;
};

struct AdminCommandOptions {
	std::string actor = "service"; // the console's sender: "owner", "admin" or "service"
	bool record = true;            // the command and its replies go to the console
};

class AdminLink {
public:
	// The longest command line: the server buffers 1024 bytes of a packet, its
	// 8-byte header and the line's NUL among them; a longer packet wedges the
	// slot. [orig: CAdminServer_AcceptConnection @0x405751..0x40577B]
	static constexpr size_t kCommandMaxBytes = 1024 - 8 - 1;

	// `pool` (may be null) is where the console goes.
	AdminLink(int64_t server_id, AdminLinkTarget target, db::ConnectionPool *pool, AdminLinkConfig config = {});
	~AdminLink();
	AdminLink(const AdminLink &) = delete;
	AdminLink &operator=(const AdminLink &) = delete;

	void start();
	// Asks the thread to end (QUIT on a logged-in session); returns at once.
	void request_stop();
	bool finished() const { return finished_.load(); }
	// Waits for the thread; safe from any thread, any number of times.
	void join();

	// Queues one command line, as RAT.exe sends it (the server's cp1252 bytes),
	// and waits at most `wait` for its replies. A link that is not up answers
	// LinkDown at once.
	AdminCommandResult run(const std::string &command, const AdminCommandOptions &options,
			std::chrono::milliseconds wait);

	AdminLinkStatus status() const;
	// The in-memory reply ring, oldest first.
	std::vector<ConsoleLine> recent() const;
	int64_t server_id() const { return server_id_; }
	const AdminLinkTarget &target() const { return target_; }

private:
	struct Pending {
		std::string command;
		AdminCommandOptions options;
		std::mutex mu;
		std::condition_variable cv;
		bool sent = false;
		bool cancelled = false;
		bool done = false;
		AdminCommandResult result;
	};

	void thread_main();
	bool log_in(std::string &error);
	// The session's end: QUIT (and the server's close) when logged in, else a reset.
	void close_session(bool logged_in);
	// One command and its replies, framed by admin_reply_plan; false (with
	// `error`) when the session is gone.
	bool exchange(const std::string &command, std::vector<std::string> &replies, std::string &error);
	// Reads off whatever arrived between commands; false when the session is gone.
	bool drain_unsolicited(std::string &error);
	bool read_packet(std::string &text, std::string &error);
	bool poll_status(std::string &error);
	void execute(Pending &pending, bool &session_ok, std::string &error);
	void complete(Pending &pending, AdminCommandOutcome outcome, std::vector<std::string> replies);
	void fail_queued(AdminCommandOutcome outcome);
	void set_state(AdminLinkState state, const std::string &error);
	void record(const char *kind, const std::string &actor, const std::string &text);
	void flush_console();
	void log(const std::string &line) const;
	// Waits up to `ms`; true once the link is asked to stop.
	bool wait_stop(int ms);

	const int64_t server_id_;
	const AdminLinkTarget target_;
	db::ConnectionPool *const pool_;
	const AdminLinkConfig config_;

	net::Socket socket_; // the thread's alone

	mutable std::mutex mu_;
	std::condition_variable cv_;
	bool stop_ = false;
	std::deque<std::shared_ptr<Pending>> queue_;
	AdminLinkStatus status_;
	std::deque<ConsoleLine> ring_;
	std::vector<ConsoleLine> unwritten_;
	std::string last_recorded_error_;
	// Whether the last command run was a recorded one (the thread's alone): a stray
	// packet after it goes to the console only then.
	bool last_command_recorded_ = false;

	std::atomic<bool> finished_{ false };
	std::mutex join_mu_;
	std::thread thread_;
};

// The live links, one per running hosted server.
class AdminLinks {
public:
	explicit AdminLinks(db::ConnectionPool *pool, AdminLinkConfig config = {});
	~AdminLinks();
	AdminLinks(const AdminLinks &) = delete;
	AdminLinks &operator=(const AdminLinks &) = delete;

	// The links the supervisor wants, by server id: each running server's
	// target. A new id gets a link; an id whose target changed (a restart's new
	// login or port) gets a new one; a link no longer wanted ends. The ending
	// links finish off the caller's thread and are joined by a later sync.
	void sync(const std::map<int64_t, AdminLinkTarget> &wanted);
	// The server's link; null when it has none.
	std::shared_ptr<AdminLink> find(int64_t server_id) const;
	// Whether the server's link has its session up (its last status poll or
	// command answered): the supervisor's health check.
	bool answering(int64_t server_id) const;
	// Every link ended and joined (the service's shutdown).
	void stop_all();

private:
	db::ConnectionPool *const pool_;
	const AdminLinkConfig config_;
	mutable std::mutex mu_;
	std::map<int64_t, std::shared_ptr<AdminLink>> links_;
	std::vector<std::shared_ptr<AdminLink>> ending_;
};

} // namespace opennova::novaworld_server::hosted
