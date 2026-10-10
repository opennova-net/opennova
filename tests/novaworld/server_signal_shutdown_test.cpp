// opennova-novaworld-server stops in order on SIGINT (Ctrl+C) and on SIGTERM
// (what `docker stop` sends). The real binary runs as a child process on a
// SQLite file in this run's temp directory under backend/migrations, with every
// port 0: the OS picks each one as the listener binds it, and the test reads the
// HTTP port back from the "[http] listening on :<port>" line. Once it is ready it
// gets the signal, and it must exit 0 within the deadline, having logged its
// shutdown legs in order (the HTTP listener, then the gate and NW UDP receive
// threads) and closed every database connection: the WAL file goes with the
// last close, and a process killed instead leaves it behind.
//
// With the HTTP layer (the server built with BUILD_NOVAWORLD_HTTP, as the Linux
// image is) the test first waits for Crow to answer, then holds a half-sent
// request open across the signal, so neither Crow's own signal handling nor a
// lingering connection can keep the process up.
//
// On Linux a third run checks the second signal: the first one's handler puts
// the default action back (read from /proc's SigCgt mask) while the shutdown is
// still pending, held off by a long tick, and the same signal again then ends
// the process at once.
//
// A last run checks what a server on every port 0 advertises before a SIGTERM
// stops it: the gate's reply names the NW UDP listener's bound port
// (UDPNOVAWORLD) and, with the HTTP layer, the HTTP listener's (STARTUPURL), as
// does the NW UDP session's 0x82 SessionInit (its web domain). main() starts
// HTTP, then NW UDP, then the gate, each with the ports bound before it.
//
// POSIX only (fork, execve, kill). The child dies with the test (SIGKILL on
// every failure path, and PR_SET_PDEATHSIG on Linux should ctest kill the test).

#include "net_http.h"
#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kBootDeadline = std::chrono::seconds(20);
constexpr auto kHttpDeadline = std::chrono::seconds(10);
// The main loop notices the signal within one tick (500 ms by default).
constexpr auto kExitDeadline = std::chrono::seconds(5);

// The server as a child process, its stdout and stderr in one log file. The
// destructor kills (SIGKILL) and reaps a child that has not exited, so no
// failure path leaves it running.
struct ServerProcess {
	pid_t pid = -1;
	int status = 0;
	bool exited = false;

	ServerProcess() = default;
	ServerProcess(const ServerProcess &) = delete;
	ServerProcess &operator=(const ServerProcess &) = delete;
	~ServerProcess() {
		if (pid > 0 && !exited) {
			::kill(pid, SIGKILL);
			::waitpid(pid, &status, 0);
		}
	}

	// `binary` with exactly `env` as its environment (nothing inherited, so a
	// shell's ONNET_* or DATABASE_PATH cannot leak in), run in `cwd`.
	bool spawn(const std::string &binary, const std::vector<std::string> &env,
	           const std::string &cwd, const std::string &log_path) {
		// Everything the child touches is built before the fork: between fork
		// and execve it only makes system calls.
		std::vector<char *> argv{const_cast<char *>(binary.c_str()), nullptr};
		std::vector<char *> envp;
		for (const std::string &kv : env) envp.push_back(const_cast<char *>(kv.c_str()));
		envp.push_back(nullptr);
		const pid_t parent = ::getpid();

		std::fflush(stdout);
		std::fflush(stderr);
		pid = ::fork();
		if (pid < 0) return false;
		if (pid == 0) {
#ifdef __linux__
			::prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (::getppid() != parent) ::_exit(127);
#else
			(void)parent;
#endif
			// A signal the test's runner blocks must still reach the server.
			sigset_t none;
			sigemptyset(&none);
			::sigprocmask(SIG_SETMASK, &none, nullptr);
			const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0 || ::chdir(cwd.c_str()) != 0) ::_exit(126);
			::dup2(fd, STDOUT_FILENO);
			::dup2(fd, STDERR_FILENO);
			::close(fd);
			::execve(binary.c_str(), argv.data(), envp.data());
			::_exit(127);
		}
		return true;
	}

	// Reap without blocking: true once the child has exited.
	bool poll() {
		if (!exited && ::waitpid(pid, &status, WNOHANG) == pid) exited = true;
		return exited;
	}

	bool wait_exit(Clock::duration limit) {
		const auto deadline = Clock::now() + limit;
		while (!poll()) {
			if (Clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return true;
	}
};

std::string read_text(const std::string &path) {
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	return std::string(bytes.begin(), bytes.end());
}

// Polls the log for `line` while the child runs; false on the deadline or when
// the child exits first.
bool wait_for_log(ServerProcess &server, const std::string &log_path,
                  const char *line, Clock::duration limit) {
	const auto deadline = Clock::now() + limit;
	for (;;) {
		if (read_text(log_path).find(line) != std::string::npos) return true;
		if (server.poll() || Clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

// GET /api/health until Crow answers 200.
bool wait_for_http(ServerProcess &server, uint16_t port) {
	const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/health";
	const auto deadline = Clock::now() + kHttpDeadline;
	for (;;) {
		const net::HttpReply reply = net::http_exchange(
				"GET", url, {}, {}, 2000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
		if (reply.transport_ok && reply.code == 200) return true;
		if (server.poll() || Clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

// True when the `lines` appear in `log` in this order.
bool logged_in_order(const std::string &log, const std::vector<const char *> &lines) {
	size_t at = 0;
	for (const char *line : lines) {
		const size_t found = log.find(line, at);
		if (found == std::string::npos) {
			std::fprintf(stderr, "  missing (in order): %s\n", line);
			return false;
		}
		at = found + std::strlen(line);
	}
	return true;
}

// The server's environment: every port 0 (the OS picks each one), the database
// in the temp directory, the repo's migrations and seed, and web roots that do
// not exist (the HTTP layer serves its API and 404s the rest).
std::vector<std::string> server_env(const test_temp::TempDir &temp, const std::string &db_path) {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	return {
		"ONNET_PUBLIC_HOST=127.0.0.1",
		"ONNET_GATE_UDP_PORT=0",
		"ONNET_NW_UDP_PORT=0",
		"ONNET_HTTP_PORT=0",
		"DATABASE_PATH=" + db_path,
		"MIGRATIONS_DIR=" + (source_dir / "backend" / "migrations").string(),
		"SEED_DIR=" + (source_dir / "backend" / "seed").string(),
		"WEB_DIST_DIR=" + temp.file("web_dist"),
		"TEMPLATES_DIR=" + temp.file("templates"),
		"STATIC_DIR=" + temp.file("static"),
	};
}

// Spawns the server and waits for "[boot] ready."; false, having said why,
// when it exits first or misses the boot deadline.
bool boot(ServerProcess &server, const std::string &binary, const std::vector<std::string> &env,
          const test_temp::TempDir &temp, const std::string &log_path) {
	if (!server.spawn(binary, env, temp.root(), log_path)) {
		std::fprintf(stderr, "  fork failed\n");
		return false;
	}
	if (wait_for_log(server, log_path, "[boot] ready.", kBootDeadline)) return true;
	if (server.exited) {
		std::fprintf(stderr, "  exited before ready (wait status 0x%x)\n",
		             static_cast<unsigned>(server.status));
	} else {
		std::fprintf(stderr, "  not ready within the boot deadline\n");
	}
	return false;
}

// The port a listener logged as the one it bound (the line `prefix` starts,
// the port after it), 0 when no line names one. Each listener's start() logs
// it before it returns, so it precedes "[boot] ready.".
constexpr const char *kGateLine = "[gate] listening on UDP :";
constexpr const char *kNwUdpLine = "[nwudp] listening on UDP :";
constexpr const char *kHttpLine = "[http] listening on :";
uint16_t logged_port(const std::string &log, const std::string &prefix) {
	const size_t at = log.find(prefix);
	if (at == std::string::npos) return 0;
	const unsigned long port = std::strtoul(log.c_str() + at + prefix.size(), nullptr, 10);
	return port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 0;
}

int exercise_shutdown(ServerProcess &server, const std::string &binary,
                      const test_temp::TempDir &temp, const std::string &log_path,
                      int signal_number) {
	const std::string db_path = temp.file("novaworld.db");
	if (!boot(server, binary, server_env(temp, db_path), temp, log_path)) return 1;

#ifdef OPENNOVA_HTTP_ENABLED
	const uint16_t http_port = logged_port(read_text(log_path), kHttpLine);
	TEST_EXPECT(http_port != 0);
	TEST_EXPECT(wait_for_http(server, http_port));
	// A request that never finishes: the headers' blank line is never sent.
	net::Endpoint to;
	TEST_EXPECT(net::resolve_ipv4("127.0.0.1", to, false));
	to.port = http_port;
	net::ScopedSocket lingering(net::tcp_connect(to, 2000));
	TEST_EXPECT(lingering.is_valid());
	const std::string partial = "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n";
	TEST_EXPECT(net::tcp_send_all(lingering.get(),
	                              reinterpret_cast<const uint8_t *>(partial.data()),
	                              partial.size()));
#endif

	TEST_EXPECT(::kill(server.pid, signal_number) == 0);
	const auto signalled = Clock::now();
	if (!server.wait_exit(kExitDeadline)) {
		std::fprintf(stderr, "  still running %lld s after the signal\n",
		             static_cast<long long>(kExitDeadline.count()));
		return 1;
	}
	if (WIFSIGNALED(server.status)) {
		std::fprintf(stderr, "  killed by signal %d\n", WTERMSIG(server.status));
	}
	TEST_EXPECT(WIFEXITED(server.status));
	TEST_EXPECT(WEXITSTATUS(server.status) == 0);
	const auto exit_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - signalled).count();

	const std::string log = read_text(log_path);
	TEST_EXPECT(logged_in_order(log, {
		"[boot] ready.",
		"[shutdown] stopping listeners",
#ifdef OPENNOVA_HTTP_ENABLED
		"[http] loop exiting",
#endif
		"[gate] loop exiting",
		"[nwudp] loop exiting",
		"==== opennova-novaworld-server stopped ====",
	}));
	// Every connection closed: the last close checkpoints the WAL and removes it.
	TEST_EXPECT(std::filesystem::exists(db_path));
	TEST_EXPECT(!std::filesystem::exists(db_path + "-wal"));
	std::printf("  exit 0 %lld ms after the signal, shutdown in order, WAL closed\n",
	            static_cast<long long>(exit_ms));
	return 0;
}

#ifdef __linux__
// Whether `pid` has a handler for `signal_number`: its SigCgt mask in /proc.
bool catches(pid_t pid, int signal_number) {
	const std::string status =
			test_io::read_file_text("/proc/" + std::to_string(pid) + "/status");
	const size_t at = status.find("SigCgt:");
	if (at == std::string::npos) return false;
	const unsigned long long mask = std::strtoull(status.c_str() + at + 7, nullptr, 16);
	return ((mask >> (signal_number - 1)) & 1ULL) != 0;
}

// The first signal raises the shutdown flag and puts the default action back.
// The tick loop, sleeping a minute between looks at the flag, leaves that
// shutdown pending (as a stuck one would be), and the second signal ends the
// process at once.
int exercise_second_signal(ServerProcess &server, const std::string &binary,
                           const test_temp::TempDir &temp, const std::string &log_path,
                           int signal_number) {
	std::vector<std::string> env = server_env(temp, temp.file("novaworld.db"));
	env.push_back("TICK_INTERVAL_MS=60000");
	if (!boot(server, binary, env, temp, log_path)) return 1;
	TEST_EXPECT(catches(server.pid, signal_number));

	TEST_EXPECT(::kill(server.pid, signal_number) == 0);
	const auto deadline = Clock::now() + kExitDeadline;
	while (catches(server.pid, signal_number)) {
		if (server.poll() || Clock::now() >= deadline) {
			std::fprintf(stderr, "  the handler still installed %lld s after the first signal\n",
			             static_cast<long long>(kExitDeadline.count()));
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	TEST_EXPECT(!server.poll()); // the shutdown is still pending

	TEST_EXPECT(::kill(server.pid, signal_number) == 0);
	if (!server.wait_exit(kExitDeadline)) {
		std::fprintf(stderr, "  still running %lld s after the second signal\n",
		             static_cast<long long>(kExitDeadline.count()));
		return 1;
	}
	TEST_EXPECT(WIFSIGNALED(server.status));
	TEST_EXPECT(WTERMSIG(server.status) == signal_number);
	std::printf("  the first signal left the shutdown pending, the second ended the process\n");
	return 0;
}
#endif

// One gate probe to 127.0.0.1:`port` and its reply, decoded into `out`.
int probe_gate(uint16_t port, opennova::GateResponse &out) {
	net::ScopedSocket client(net::udp_bind(0));
	TEST_EXPECT(client.is_valid());
	const auto inner = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t probe_size = 0;
	TEST_EXPECT(opennova::napi_envelope_encode(inner.data(), inner.size(), probe.data(),
	                                           probe.size(), &probe_size) == 0);
	probe.resize(probe_size);
	const net::Endpoint to{{127, 0, 0, 1}, port};
	TEST_EXPECT(net::udp_send_to(client.get(), to, probe.data(), probe.size()) > 0);
	uint8_t rx[2048];
	net::Endpoint from{};
	const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, 3000);
	TEST_EXPECT(n > 0);
	std::vector<uint8_t> reply(static_cast<size_t>(n));
	size_t reply_len = 0;
	TEST_EXPECT(opennova::napi_envelope_decode(rx, static_cast<size_t>(n), reply.data(),
	                                           reply.size(), &reply_len) == 0);
	TEST_EXPECT(opennova::gate_response_decrypt_and_parse(reply.data(), reply_len, out));
	return 0;
}

#ifdef OPENNOVA_HTTP_ENABLED
// A NovaWorld session to 127.0.0.1:`port` up to its 0x82 SessionInit; the web
// domain that carried goes to `web_domain`.
int session_web_domain(uint16_t port, std::string &web_domain) {
	net::ScopedSocket client(net::udp_bind(0));
	TEST_EXPECT(client.is_valid());
	const net::Endpoint to{{127, 0, 0, 1}, port};
	opennova::ClientSession session;
	const auto send = [&](const std::vector<uint8_t> &datagram) {
		if (!datagram.empty()) net::udp_send_to(client.get(), to, datagram.data(), datagram.size());
	};
	send(session.start());
	for (int i = 0; i < 25 && session.server_web_domain().empty(); ++i) {
		uint8_t rx[4096];
		net::Endpoint from{};
		const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, 200);
		if (n <= 0) continue;
		std::vector<std::vector<uint8_t>> out;
		TEST_EXPECT(session.handle_datagram(rx, static_cast<size_t>(n), out));
		session.finish_receive_batch(out);
		session.pump(out);
		for (const auto &datagram : out) send(datagram);
	}
	web_domain = session.server_web_domain();
	return 0;
}
#endif

// Every port 0: what the server advertises names the ports its listeners
// bound, read back from their "listening" lines. Then the signal stops it.
int exercise_sibling_ports(ServerProcess &server, const std::string &binary,
                           const test_temp::TempDir &temp, const std::string &log_path,
                           int signal_number) {
	if (!boot(server, binary, server_env(temp, temp.file("novaworld.db")), temp, log_path)) return 1;
	const std::string log = read_text(log_path);
	const uint16_t gate_port = logged_port(log, kGateLine);
	const uint16_t nw_port = logged_port(log, kNwUdpLine);
	TEST_EXPECT(gate_port != 0 && nw_port != 0);

	opennova::GateResponse reply;
	if (probe_gate(gate_port, reply) != 0) return 1;
	if (reply.udp_novaworld != "127.0.0.1:" + std::to_string(nw_port)) {
		std::fprintf(stderr, "  UDPNOVAWORLD %s, the NW UDP listener on :%u\n",
		             reply.udp_novaworld.c_str(), static_cast<unsigned>(nw_port));
		return 1;
	}
#ifdef OPENNOVA_HTTP_ENABLED
	const uint16_t http_port = logged_port(log, kHttpLine);
	TEST_EXPECT(http_port != 0);
	const std::string http_host = "127.0.0.1:" + std::to_string(http_port);
	const std::string startup = "http://" + http_host + "/nwprepare.dll?";
	if (reply.startup_url.compare(0, startup.size(), startup) != 0) {
		std::fprintf(stderr, "  STARTUPURL %s, the HTTP listener on :%u\n",
		             reply.startup_url.c_str(), static_cast<unsigned>(http_port));
		return 1;
	}
	std::string web_domain;
	if (session_web_domain(nw_port, web_domain) != 0) return 1;
	if (web_domain != http_host) {
		std::fprintf(stderr, "  SessionInit web domain '%s', the HTTP listener on :%u\n",
		             web_domain.c_str(), static_cast<unsigned>(http_port));
		return 1;
	}
#endif

	TEST_EXPECT(::kill(server.pid, signal_number) == 0);
	TEST_EXPECT(server.wait_exit(kExitDeadline));
	TEST_EXPECT(WIFEXITED(server.status) && WEXITSTATUS(server.status) == 0);
	std::printf("  the gate names the NW UDP listener's bound port :%u\n",
	            static_cast<unsigned>(nw_port));
#ifdef OPENNOVA_HTTP_ENABLED
	std::printf("  the gate and the SessionInit name the HTTP listener's bound port :%u\n",
	            static_cast<unsigned>(http_port));
#endif
	return 0;
}

using Exercise = int (*)(ServerProcess &, const std::string &, const test_temp::TempDir &,
                         const std::string &, int);

int run_case(const std::string &binary, Exercise exercise, int signal_number, const char *name) {
	// The TempDir outlives the child: ServerProcess is destroyed (and the child
	// killed, on failure) before the directory goes.
	test_temp::TempDir temp((std::string("nw_signal_") + name).c_str());
	const std::string log_path = temp.file("server.log");
	std::printf("%s:\n", name);
	int rc = 0;
	{
		ServerProcess server;
		rc = exercise(server, binary, temp, log_path, signal_number);
	}
	if (rc != 0) {
		std::fprintf(stderr, "FAIL: %s\n---- server log ----\n%s---- end ----\n", name,
		             read_text(log_path).c_str());
	}
	return rc;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <opennova-novaworld-server>\n", argv[0]);
		return 2;
	}
	// Absolute: the child runs in its temp directory.
	const std::string binary = std::filesystem::absolute(argv[1]).string();
	TEST_EXPECT(net::startup() == 0);
	int rc = run_case(binary, exercise_shutdown, SIGINT, "sigint");
	if (rc == 0) rc = run_case(binary, exercise_shutdown, SIGTERM, "sigterm");
#ifdef __linux__
	if (rc == 0) rc = run_case(binary, exercise_second_signal, SIGINT, "second_sigint");
#endif
	if (rc == 0) rc = run_case(binary, exercise_sibling_ports, SIGTERM, "sibling_ports");
	net::shutdown();
	if (rc == 0) std::printf("OK: novaworld server signal shutdown\n");
	return rc;
}
