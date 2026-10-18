// The six programs end to end (apps/novaworld_common/program.h): gate, lobby,
// web, fleet, worker and data as six processes of their built binaries
// (program_process.h), on one database of the test's own and one Redis
// namespace, sharing nothing but those (ctest passes the six binaries). Every
// address a program announces comes from the config, every port fixed up
// front. Hosted servers across them (the fleet's supervisor, the
// worker's machine driver, a machine's agent and its servers) are the local
// stack's smoke test's (scripts/dev_stack.sh smoke, its machines containers of
// their own); hosted_local_e2e runs them in process over the real agent.
//
//   1. a retail-protocol host (a ClientSession, no account) lists through the
//      gate (its reply hands out the lobby's instance) and the lobby, and a
//      second client sits in its lobby;
//   2. an admin's push through web (POST /api/admin/hosts/<rid>/command)
//      reaches the host, as a lobby command the lobby takes;
//   3. web, the worker, the data program and the gate are stopped (SIGTERM, as
//      a redeploy stops them) and the fleet killed (SIGKILL), then all five
//      started again:
//      the lobby never restarts, the host's listing and both lobby sessions
//      survive (the same session rows, still verified), the new gate hands out
//      the same lobby, and a push through the new web still reaches the host;
//   4. the fleet's database connection freezes (a TCP proxy that stops
//      forwarding, as a dead network does): its watchdog finds its loops
//      stalled and exits it alone (70), while the others run on, and a push
//      still reaches the host;
//   5. the lobby is restarted: it alone drops its own sessions and listings
//      (the host's session row and its listing gone), and its new run, an
//      instance of its own at the same address, takes clients again.
//
// Built only with BUILD_NOVAWORLD_HTTP; POSIX only. Serial.

#include "lobby_client.h"
#include "program_process.h"

#include "health.h"

#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include "common/test_expect.h"
#include "common/test_redis.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

using namespace program_process;
namespace fs = std::filesystem;
namespace nws = opennova::novaworld_server;

namespace {

using lobby_client::LobbyClient;
using Notice = lobby_client::Notice;

constexpr const char *kAdminToken = "e2e-admin-token";
constexpr const char *kHostName = "Programs E2E Host";

// The lobby on loopback at `port`.
net::Endpoint loopback(uint16_t port) {
	return net::Endpoint{ { 127, 0, 0, 1 }, port };
}

// A client's lobby_sessions row: the one at its address.
std::string address_of(const LobbyClient &c) {
	return "127.0.0.1:" + std::to_string(c.port());
}

// --- a TCP proxy to PostgreSQL that can freeze ------------------------------------------

// Forwards each connection to the database both ways until frozen: then it
// forwards nothing and reads nothing, every statement on it waiting forever, as
// a dead network leaves a connection. close() ends every connection.
class FreezableProxy {
public:
	~FreezableProxy() { close(); }

	// Listens on 127.0.0.1:<a free port> for `host`:`port` (resolved now).
	bool start(const std::string &host, uint16_t port) {
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *found = nullptr;
		if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 ||
				found == nullptr)
			return false;
		std::memcpy(&target_, found->ai_addr, sizeof(target_));
		::freeaddrinfo(found);
		listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t len = sizeof(local);
		if (listener_ < 0 ||
				::bind(listener_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 ||
				::listen(listener_, 32) != 0 ||
				::getsockname(listener_, reinterpret_cast<sockaddr *>(&local), &len) != 0)
			return false;
		port_ = ntohs(local.sin_port);
		accepter_ = std::thread([this] { accept_loop(); });
		return true;
	}

	uint16_t port() const { return port_; }
	void freeze() { frozen_.store(true); }

	void close() {
		if (stopping_.exchange(true))
			return;
		if (listener_ >= 0)
			::shutdown(listener_, SHUT_RDWR);
		if (accepter_.joinable())
			accepter_.join();
		if (listener_ >= 0)
			::close(listener_);
		std::vector<std::thread> pumps;
		{
			std::lock_guard<std::mutex> lock(mu_);
			for (const int fd : fds_)
				::shutdown(fd, SHUT_RDWR);
			pumps.swap(pumps_);
		}
		for (std::thread &t : pumps)
			t.join();
		std::lock_guard<std::mutex> lock(mu_);
		for (const int fd : fds_)
			::close(fd);
		fds_.clear();
	}

private:
	void accept_loop() {
		while (!stopping_.load()) {
			pollfd p{ listener_, POLLIN, 0 };
			if (::poll(&p, 1, 100) <= 0)
				continue;
			const int client = ::accept(listener_, nullptr, nullptr);
			if (client < 0)
				continue;
			const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
			if (upstream < 0 ||
					::connect(upstream, reinterpret_cast<sockaddr *>(&target_), sizeof(target_)) !=
							0) {
				::close(client);
				if (upstream >= 0)
					::close(upstream);
				continue;
			}
			std::lock_guard<std::mutex> lock(mu_);
			fds_.push_back(client);
			fds_.push_back(upstream);
			pumps_.emplace_back([this, client, upstream] { pump(client, upstream); });
		}
	}

	// Until either side ends (then both end: a client gone, as a killed process's
	// connection goes, ends its database session and the locks it held).
	void pump(int a, int b) {
		forward(a, b);
		::shutdown(a, SHUT_RDWR);
		::shutdown(b, SHUT_RDWR);
	}

	void forward(int a, int b) {
		char buffer[16384];
		while (!stopping_.load()) {
			if (frozen_.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			pollfd p[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
			if (::poll(p, 2, 100) <= 0)
				continue;
			for (int i = 0; i < 2; ++i) {
				if ((p[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
					continue;
				const ssize_t n = ::recv(p[i].fd, buffer, sizeof buffer, 0);
				if (n <= 0)
					return;
				const int to = i == 0 ? b : a;
				for (ssize_t sent = 0; sent < n;) {
					const ssize_t w =
							::send(to, buffer + sent, static_cast<size_t>(n - sent), MSG_NOSIGNAL);
					if (w <= 0)
						return;
					sent += w;
				}
			}
		}
	}

	int listener_ = -1;
	uint16_t port_ = 0;
	sockaddr_in target_{};
	std::atomic<bool> frozen_{ false };
	std::atomic<bool> stopping_{ false };
	std::thread accepter_;
	std::mutex mu_;
	std::vector<int> fds_;
	std::vector<std::thread> pumps_;
};

// A postgresql:// URL's host and port (5432 when it names none); false when it names no host.
bool url_host_port(
		const std::string &url, std::string &host, uint16_t &port, size_t &start, size_t &end) {
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return false;
	const size_t authority = scheme + 3;
	end = url.find_first_of("/?", authority);
	if (end == std::string::npos)
		end = url.size();
	const size_t at = url.rfind('@', end);
	start = at == std::string::npos || at < authority ? authority : at + 1;
	const std::string hostport = url.substr(start, end - start);
	const size_t colon = hostport.rfind(':');
	host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
	port = colon == std::string::npos
			? 5432
			: static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
	return !host.empty();
}

// --- web's routes ----------------------------------------------------------------

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

struct Web {
	uint16_t port = 0;

	net::HttpReply send(const char *method, const std::string &path,
			const std::vector<std::string> &headers, const std::string &body = {}) const {
		return net::http_exchange(method, "http://127.0.0.1:" + std::to_string(port) + path,
				headers, body, 5000, loopback_only());
	}

	// The admin's push to listed RID `rid`: 202 once the lobby has it queued.
	int push(uint32_t rid, const std::string &name) const {
		const auto reply = send("POST", "/api/admin/hosts/" + std::to_string(rid) + "/command",
				{ std::string("Authorization: Bearer ") + kAdminToken,
						"Content-Type: application/json" },
				"{\"verb\":\"SetServerName\",\"args\":[\"" + name + "\"]}");
		if (reply.code != 202)
			std::fprintf(stderr, "  push: %d %s\n", reply.code, body_text(reply).c_str());
		return reply.code;
	}
};

template <typename Done> bool wait_for(Done done, int seconds) {
	const auto deadline = Clock::now() + std::chrono::seconds(seconds);
	while (!done() && Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return done();
}

// TEST_EXPECT, with every program's log dumped on a failure (`fail` in scope).
#define E2E_EXPECT(expr)                                                                           \
	do {                                                                                           \
		if (!(expr)) {                                                                             \
			std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, #expr);    \
			return fail("an expectation");                                                         \
		}                                                                                          \
	} while (0)

// One program's process and its log.
struct Program {
	const char *name;
	std::string binary;
	std::string log_path;
	ProgramProcess process;
	int starts = 0;
};

void dump_logs(std::vector<Program *> programs) {
	for (Program *p : programs) {
		std::string log = read_text(p->log_path);
		if (log.size() > 6000)
			log = log.substr(log.size() - 6000);
		std::fprintf(stderr, "---- %s (last 6000 bytes) ----\n%s\n", p->name, log.c_str());
	}
}

// The lobby endpoint the gate at 127.0.0.1:`port` hands out, "" when no reply came.
std::string gate_hands_out(uint16_t port) {
	return lobby_client::gate_hands_out(loopback(port));
}

int run(const Binaries &bin) {
	test_temp::TempDir temp("nw_programs_e2e");
	const test_db::TestDatabase database({ /*migrate=*/false });

	// Every port the programs' config names, fixed up front: the programs are
	// six processes, none of which learns another's bound port.
	const uint16_t gate_port = free_udp_port();
	const uint16_t nw_port = free_udp_port();
	const uint16_t http_port = free_tcp_port();
	const uint16_t internal_port = free_tcp_port();
	TEST_EXPECT(gate_port && nw_port && http_port && internal_port && gate_port != nw_port &&
			http_port != internal_port);
	const std::string lobby_endpoint = "127.0.0.1:" + std::to_string(nw_port);

	// The fleet reaches the database through a proxy that can freeze (5).
	std::string pg_host;
	uint16_t pg_port = 0;
	size_t host_start = 0, host_end = 0;
	TEST_EXPECT(url_host_port(database.conninfo(), pg_host, pg_port, host_start, host_end));
	FreezableProxy proxy;
	TEST_EXPECT(proxy.start(pg_host, pg_port));
	const std::string proxied_url = database.conninfo().substr(0, host_start) +
			"127.0.0.1:" + std::to_string(proxy.port()) + database.conninfo().substr(host_end);

	// What every program is given; each reads its own and ignores the rest. The
	// gate has no fallback lobby: it hands out the lobby instances alone.
	const std::vector<std::string> common = {
		"ONNET_PUBLIC_HOST=127.0.0.1",
		"ONNET_WEB_HOST=127.0.0.1",
		"ONNET_GATE_UDP_PORT=" + std::to_string(gate_port),
		"ONNET_NW_UDP_PORT=" + std::to_string(nw_port),
		"ONNET_HTTP_PORT=" + std::to_string(http_port),
		"ONNET_INTERNAL_URL=http://127.0.0.1:" + std::to_string(internal_port),
		"ONNET_INTERNAL_HTTP_BIND=127.0.0.1:" + std::to_string(internal_port),
		std::string("ADMIN_API_TOKEN=") + kAdminToken,
		"ONNET_REDIS_URL=" + test_redis::server_url(),
		// The retail pages web serves (its main page's @HOST_URL@).
		"TEMPLATES_DIR=" +
				(fs::path(OPENNOVA_SOURCE_DIR) / "apps" / "novaworld_web" / "templates").string(),
		"STATIC_DIR=" +
				(fs::path(OPENNOVA_SOURCE_DIR) / "apps" / "novaworld_web" / "static").string(),
	};
	Program lobby{ "lobby", bin.lobby };
	Program gate{ "gate", bin.gate };
	Program web{ "web", bin.web };
	Program fleet{ "fleet", bin.fleet };
	Program worker{ "worker", bin.worker };
	Program data{ "data", bin.data };
	const std::vector<Program *> all = { &lobby, &gate, &web, &fleet, &worker, &data };
	const auto start_program = [&](Program &p) {
		const std::string url = &p == &fleet ? proxied_url : database.conninfo();
		std::vector<std::string> env = common;
		// Only the lobby announces itself.
		if (&p == &lobby)
			env.push_back("ONNET_LOBBY_ENDPOINT=" + lobby_endpoint);
		// Each start's log apart, so a restart's is its own.
		p.log_path = temp.file(
				(std::string(p.name) + "-" + std::to_string(++p.starts) + ".log").c_str());
		return boot(p.process, p.binary, program_env(temp, url, env), temp.root(), p.log_path);
	};
	const auto stop_program = [](Program &p, int signal_number) {
		return ::kill(p.process.pid, signal_number) == 0 &&
				p.process.wait_exit(std::chrono::seconds(20)) &&
				(signal_number == SIGKILL || p.process.exit_code() == 0);
	};
	const auto fail = [&](const char *what) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		dump_logs(all);
		return 1;
	};
	const auto leads = [&fleet] {
		return read_text(fleet.log_path).find(" leads (generation ") != std::string::npos;
	};

	// The lobby first (its boot migrates the empty database), then the others.
	for (Program *p : all) {
		if (!start_program(*p))
			return fail((std::string("the ") + p->name + " boots").c_str());
	}
	if (!wait_for(leads, 15))
		return fail("the fleet leads");
	const Web site{ http_port };
	E2E_EXPECT(wait_for_http(web.process, http_port, "/healthz"));
	const pid_t lobby_pid = lobby.process.pid;

	// --- 1. a retail-protocol host lists through the gate and the lobby; a client
	// sits in its lobby.
	E2E_EXPECT(gate_hands_out(gate_port) == lobby_endpoint);
	LobbyClient host("programs:host", 0x50524f47u, 0x484f5354u, loopback(nw_port));
	LobbyClient sitter("programs:lobby", 0x50524f47u, 0x4c4f4259u, loopback(nw_port));
	host.start();
	sitter.start();
	if (!wait_for([&] { return host.verified() && sitter.verified(); }, 10))
		return fail("both sessions verify against the lobby");
	host.host(kHostName);
	const auto hosted_result = host.wait_notice(Notice::Kind::HostResult, 0, 5000);
	E2E_EXPECT(hosted_result && hosted_result->fields.success != 0);
	opennova::db::Database db = database.open();
	const auto session_of =
			[&db](const LobbyClient &c) -> std::optional<opennova::lobbydb::SessionRow> {
		for (auto &row : opennova::lobbydb::list_sessions(db))
			if (opennova::lobbydb::session_addr(row) == address_of(c))
				return row;
		return std::nullopt;
	};
	std::optional<opennova::lobbydb::SessionRow> host_row;
	wait_for(
			[&] {
				host_row = session_of(host);
				return host_row && host_row->listed && host_row->rid != 0;
			},
			5);
	if (!host_row || !host_row->listed)
		return fail("the host's listing is in the lobby's session row");
	const uint32_t rid = host_row->rid;
	const auto lobby_row = session_of(sitter);
	E2E_EXPECT(lobby_row && !lobby_row->hosting);
	E2E_EXPECT(opennova::hostdb::find_host_by_rid(db, rid).has_value());

	// --- 2. an admin's push through web reaches the host.
	size_t seen = host.notice_count();
	E2E_EXPECT(site.push(rid, "Pushed Before") == 202);
	const auto before = host.wait_notice(Notice::Kind::Command, seen, 5000);
	E2E_EXPECT(before && before->command.args == std::vector<std::string>{ "Pushed Before" });

	// --- 3. web, the worker, the data program and the gate stopped as a
	// redeploy stops them, the fleet killed; all five started again. The lobby
	// never restarts.
	E2E_EXPECT(stop_program(web, SIGTERM));
	E2E_EXPECT(stop_program(fleet, SIGKILL));
	E2E_EXPECT(stop_program(worker, SIGTERM));
	E2E_EXPECT(stop_program(data, SIGTERM));
	E2E_EXPECT(stop_program(gate, SIGTERM));
	for (Program *p : { &gate, &web, &fleet, &worker, &data }) {
		if (!start_program(*p))
			return fail((std::string("the ") + p->name + " boots again").c_str());
	}
	if (!wait_for(leads, 20))
		return fail("the new fleet process leads");
	E2E_EXPECT(wait_for_http(web.process, http_port, "/healthz"));
	E2E_EXPECT(!lobby.process.poll() && lobby.process.pid == lobby_pid);
	// The listing and both sessions survived: the same rows, still verified; the
	// new gate hands out the same lobby.
	const auto host_after = session_of(host);
	const auto lobby_after = session_of(sitter);
	E2E_EXPECT(host_after && host_after->id == host_row->id && host_after->listed &&
			host_after->rid == rid);
	E2E_EXPECT(lobby_after && lobby_after->id == lobby_row->id);
	E2E_EXPECT(host.verified() && sitter.verified());
	E2E_EXPECT(opennova::hostdb::find_host_by_rid(db, rid).has_value());
	E2E_EXPECT(gate_hands_out(gate_port) == lobby_endpoint);
	seen = host.notice_count();
	E2E_EXPECT(site.push(rid, "Pushed After") == 202);
	const auto after = host.wait_notice(Notice::Kind::Command, seen, 5000);
	E2E_EXPECT(after && after->command.args == std::vector<std::string>{ "Pushed After" });
	std::printf("  web, fleet, worker, data and gate restarted; the listing and the lobby "
				"sessions survived; a push lands\n");

	// --- 4. the fleet's database freezes: its watchdog ends it alone.
	proxy.freeze();
	const auto frozen_at = Clock::now();
	if (!fleet.process.wait_exit(std::chrono::seconds(60)))
		return fail("the stalled fleet process exits");
	const auto stall_s =
			std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - frozen_at).count();
	E2E_EXPECT(fleet.process.exit_code() == nws::Watchdog::kStallExitCode);
	E2E_EXPECT(read_text(fleet.log_path).find("[watchdog] FATAL the ") != std::string::npos);
	E2E_EXPECT(!lobby.process.poll() && !gate.process.poll() && !web.process.poll() &&
			!worker.process.poll());
	E2E_EXPECT(host.verified() && sitter.verified());
	seen = host.notice_count();
	E2E_EXPECT(site.push(rid, "Pushed Alone") == 202);
	const auto alone = host.wait_notice(Notice::Kind::Command, seen, 5000);
	E2E_EXPECT(alone && alone->command.args == std::vector<std::string>{ "Pushed Alone" });
	proxy.close();
	std::printf("  the stalled fleet process exited 70 after %lld s, alone; a push still lands\n",
			static_cast<long long>(stall_s));

	// --- 5. the lobby restarted: it alone drops its own sessions and listings.
	// The clients fall silent first (no goodbye: what ends their sessions is the
	// lobby's restart), then the lobby, alone of its instances, stops at once and
	// starts again.
	E2E_EXPECT(session_of(host).has_value() && session_of(sitter).has_value());
	const auto instance_before = opennova::lobbydb::list_instances(db);
	E2E_EXPECT(instance_before.size() == 1);
	host.abandon();
	sitter.abandon();
	E2E_EXPECT(stop_program(lobby, SIGTERM));
	if (!start_program(lobby))
		return fail("the lobby boots again");
	E2E_EXPECT(!session_of(host) && !session_of(sitter));
	E2E_EXPECT(!opennova::hostdb::find_host_by_rid(db, rid).has_value());
	const auto instance_after = opennova::lobbydb::list_instances(db);
	E2E_EXPECT(instance_after.size() == 1 && !instance_before.empty() &&
			instance_after[0].id != instance_before[0].id &&
			instance_after[0].endpoint == lobby_endpoint);
	E2E_EXPECT(gate_hands_out(gate_port) == lobby_endpoint);
	std::printf("  the lobby restarted: its sessions and its listing went with it, and the gate "
				"hands it out again\n");

	for (Program *p : { &web, &worker, &gate, &lobby })
		E2E_EXPECT(stop_program(*p, SIGTERM));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	Binaries bin;
	if (!bin.read(argc, argv, 1)) {
		std::fprintf(stderr, "usage: %s <gate> <lobby> <web> <fleet> <worker> <data>\n", argv[0]);
		return 2;
	}
	TEST_EXPECT(net::startup() == 0);
	const int rc = run(bin);
	net::shutdown();
	if (rc == 0)
		std::printf(
				"OK: six programs, six processes: a redeploy and a stalled fleet process leave "
				"the lobby serving, and only the lobby's restart drops its sessions\n");
	return rc;
}
