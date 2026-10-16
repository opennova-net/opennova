#pragma once

// The console routes' commands, through the database (migration 0008,
// hosted_console_commands and hosted_console_replies): the web routes never
// hold an admin link (admin_link.h); the controller does. A route writes its
// command with the deadline its budget allows and waits for its row to be
// done; the controller's ConsoleDrain claims the queued commands (FOR UPDATE
// SKIP LOCKED) under its controller's generation, runs each on its server's
// link off the drain's thread (a thread a command, so one slow server holds up
// no other), and writes what came back: the outcome, whether the command was
// sent, each reply packet in the server's code page. A command an earlier
// controller claimed and never answered (it went away) the next one's drain
// answers link down; a route that has no done row past its wait says the
// command was not sent (only a done row says it was). The console ring
// (hosted_server_console) is still the link's to write. Service policy, not a
// port.

#include "admin_link.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <thread>

namespace opennova::novaworld_server::hosted {

// How long past its deadline a route waits for the controller's own answer to
// a command it handed the link (its TimedOut says whether it was sent).
inline constexpr int64_t kConsoleAnswerGraceMs = 500;
// How often a route reads its command's row while it waits.
inline constexpr int64_t kConsolePollMs = 15;

// The route's run: `command` on server `server_id`'s link (as AdminLink::run),
// through the controller, waiting at most `wait` (and the grace) for its
// answer; a command still queued when the wait ends is withdrawn, never sent,
// and one claimed but not answered by then is TimedOut, not sent (whether its
// controller sent it is unknown). The calling thread holds no lease. Throws
// db::Error on a database failure.
AdminCommandResult run_console_command(db::ConnectionPool &pool, int64_t server_id,
		const std::string &command, const AdminCommandOptions &options,
		std::chrono::milliseconds wait);

// The controller's side: claims the queued commands every poll_ms under its
// controller's generation (controller_leader's when start() runs: the drain
// starts once its controller leads; 0 when driven by hand) and runs each on its
// link (none: LinkDown), writing its answer to its row; the rows whose routes
// went away (past their deadline a minute) are deleted, and those an earlier
// generation claimed and left running are answered LinkDown, not sent.
class ConsoleDrain {
public:
	// The most commands running at once; more wait queued.
	static constexpr size_t kMaxRunning = 64;

	// `pool` and `links` must outlive the drain.
	ConsoleDrain(db::ConnectionPool &pool, AdminLinks &links, int64_t poll_ms = 20);
	~ConsoleDrain();
	ConsoleDrain(const ConsoleDrain &) = delete;
	ConsoleDrain &operator=(const ConsoleDrain &) = delete;

	void start();
	// Ends the drain once the commands running have answered. Idempotent.
	void stop();
	// One claim of the queued commands, each started on a thread of its own
	// (the drain's turn; the tests call it by hand): how many it started.
	size_t drain_once();

private:
	struct Running {
		std::thread thread;
		std::atomic<bool> done{ false };
	};

	void run();
	void execute(int64_t id, int64_t server_id, std::string line, AdminCommandOptions options,
			std::chrono::milliseconds left);
	// Joins the commands that answered; how many still run.
	size_t reap();

	db::ConnectionPool &pool_;
	AdminLinks &links_;
	const int64_t poll_ms_;
	std::list<Running> running_; // the drain's thread alone
	int64_t last_cleanup_ = 0;
	int64_t generation_ = 0; // set by start(), before the drain's thread

	std::mutex mu_;
	std::condition_variable cv_;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace opennova::novaworld_server::hosted
