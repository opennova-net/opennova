// opennova-novaworld-server stops in order on SIGINT (Ctrl+C) and on SIGTERM
// (what `docker stop` sends). The real binary runs as a child process on a
// SQLite file in this run's temp directory under backend/migrations, a free
// HTTP port and ephemeral UDP ports. Once it is ready it gets the signal, and it
// must exit 0 within the deadline, having logged its shutdown legs in order
// (the HTTP listener, then the gate and NW UDP receive threads) and closed every
// database connection: the WAL file goes with the last close, and a process
// killed instead leaves it behind.
//
// With the HTTP layer (the server built with BUILD_NOVAWORLD_HTTP, as the Linux
// image is) the test first waits for Crow to answer, then holds a half-sent
// request open across the signal, so neither Crow's own signal handling nor a
// lingering connection can keep the process up.
//
// POSIX only (fork, execve, kill). The child dies with the test (SIGKILL on
// every failure path, and PR_SET_PDEATHSIG on Linux should ctest kill the test).

#include "net_http.h"
#include "net_sockets.h"

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdio>
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

int exercise(ServerProcess &server, const std::string &binary, const test_temp::TempDir &temp,
             const std::string &log_path, int signal_number) {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const std::string db_path = temp.file("novaworld.db");

	// A free port for Crow: bind an ephemeral one on loopback, release it.
	uint16_t http_port = 0;
	{
		net::ScopedSocket probe(net::tcp_listen(0, 1, &http_port, true));
		TEST_EXPECT(probe.is_valid() && http_port != 0);
	}

	const std::vector<std::string> env = {
		"ONNET_PUBLIC_HOST=127.0.0.1",
		"ONNET_GATE_UDP_PORT=0",
		"ONNET_NW_UDP_PORT=0",
		"ONNET_HTTP_PORT=" + std::to_string(http_port),
		"DATABASE_PATH=" + db_path,
		"MIGRATIONS_DIR=" + (source_dir / "backend" / "migrations").string(),
		"SEED_DIR=" + (source_dir / "backend" / "seed").string(),
		// Roots that do not exist: the HTTP layer serves its API and 404s the rest.
		"WEB_DIST_DIR=" + temp.file("web_dist"),
		"TEMPLATES_DIR=" + temp.file("templates"),
		"STATIC_DIR=" + temp.file("static"),
	};
	TEST_EXPECT(server.spawn(binary, env, temp.root(), log_path));
	if (!wait_for_log(server, log_path, "[boot] ready.", kBootDeadline)) {
		if (server.exited) {
			std::fprintf(stderr, "  exited before ready (wait status 0x%x)\n",
			             static_cast<unsigned>(server.status));
		} else {
			std::fprintf(stderr, "  not ready within the boot deadline\n");
		}
		return 1;
	}

#ifdef OPENNOVA_HTTP_ENABLED
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

int run_case(const std::string &binary, int signal_number, const char *name) {
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
	int rc = run_case(binary, SIGINT, "sigint");
	if (rc == 0) rc = run_case(binary, SIGTERM, "sigterm");
	net::shutdown();
	if (rc == 0) std::printf("OK: novaworld server signal shutdown\n");
	return rc;
}
