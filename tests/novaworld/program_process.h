#pragma once

// A NovaWorld service program's binary as a child process, for the tests that
// run them whole (programs_test, signal_shutdown_test, nw_programs_e2e_test):
// spawned with exactly the environment the test gives it, its stdout and stderr
// in one log file the test reads back, signalled, reaped.
// The destructor kills (SIGKILL) and reaps a child that has not exited, so no
// failure path leaves one running, and on Linux PR_SET_PDEATHSIG kills it
// should ctest kill the test. POSIX only (fork, execve, kill).

#include "net_http.h"
#include "net_sockets.h"

#include <net/novaworld/db/database.h>

#include "common/file_io.h"
#include "common/test_database.h"
#include "common/temp_dir.h"

#include <chrono>
#include <cstdint>
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
#error "program_process.h needs OPENNOVA_SOURCE_DIR (the repo root)"
#endif

namespace program_process {

namespace net = opennova::net;
using Clock = std::chrono::steady_clock;

// A program's boot (migrations included) and its ready line.
inline constexpr auto kBootDeadline = std::chrono::seconds(20);
inline constexpr auto kHttpDeadline = std::chrono::seconds(10);
// The main loop notices the signal within one tick (500 ms by default).
inline constexpr auto kExitDeadline = std::chrono::seconds(5);

// The lines each listener logs once it serves, the port it bound after them.
inline constexpr const char *kGateLine = "[gate] listening on UDP :";
inline constexpr const char *kNwUdpLine = "[nwudp] listening on UDP :";
inline constexpr const char *kHttpLine = "[http] listening on :";
inline constexpr const char *kHealthLine = "[health] listening on TCP :";

// The five programs' binaries, as ctest passes them (gate, lobby, web,
// controller, worker; absolute, since a child runs in its temp directory).
struct Binaries {
	std::string gate, lobby, web, controller, worker;

	// argv[first..first+4]; false when there are not five.
	bool read(int argc, char **argv, int first) {
		if (argc < first + 5)
			return false;
		std::string *each[] = { &gate, &lobby, &web, &controller, &worker };
		for (int i = 0; i < 5; ++i)
			*each[i] = std::filesystem::absolute(argv[first + i]).string();
		return true;
	}
};

struct ProgramProcess {
	pid_t pid = -1;
	int status = 0;
	bool exited = false;

	ProgramProcess() = default;
	ProgramProcess(const ProgramProcess &) = delete;
	ProgramProcess &operator=(const ProgramProcess &) = delete;
	~ProgramProcess() { kill_now(); }

	// SIGKILL and reap, unless it has exited.
	void kill_now() {
		if (pid > 0 && !exited) {
			::kill(pid, SIGKILL);
			::waitpid(pid, &status, 0);
			exited = true;
		}
	}

	// `binary` with exactly `env` as its environment (nothing inherited, so a
	// shell's ONNET_* or DATABASE_* cannot leak in), run in `cwd`.
	bool spawn(const std::string &binary, const std::vector<std::string> &env,
			const std::string &cwd, const std::string &log_path) {
		// Everything the child touches is built before the fork: between fork
		// and execve it only makes system calls.
		std::vector<char *> argv{ const_cast<char *>(binary.c_str()), nullptr };
		std::vector<char *> envp;
		for (const std::string &kv : env)
			envp.push_back(const_cast<char *>(kv.c_str()));
		envp.push_back(nullptr);
		const pid_t parent = ::getpid();
		// An earlier run's log at the same path goes first, so no line of it
		// reads as this run's before the child has opened the file.
		std::remove(log_path.c_str());

		std::fflush(stdout);
		std::fflush(stderr);
		status = 0;
		exited = false;
		pid = ::fork();
		if (pid < 0)
			return false;
		if (pid == 0) {
#ifdef __linux__
			::prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (::getppid() != parent)
				::_exit(127);
#else
			(void)parent;
#endif
			// A signal the test's runner blocks must still reach the program.
			sigset_t none;
			sigemptyset(&none);
			::sigprocmask(SIG_SETMASK, &none, nullptr);
			const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0 || ::chdir(cwd.c_str()) != 0)
				::_exit(126);
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
		if (!exited && pid > 0 && ::waitpid(pid, &status, WNOHANG) == pid)
			exited = true;
		return exited;
	}

	bool wait_exit(Clock::duration limit) {
		const auto deadline = Clock::now() + limit;
		while (!poll()) {
			if (Clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return true;
	}

	// The exit code of a child that exited by itself; -1 otherwise.
	int exit_code() const { return exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
};

inline std::string read_text(const std::string &path) {
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	return std::string(bytes.begin(), bytes.end());
}

// Polls the log for `line` while the child runs; false on the deadline or when
// the child exits first.
inline bool wait_for_log(ProgramProcess &process, const std::string &log_path, const char *line,
		Clock::duration limit) {
	const auto deadline = Clock::now() + limit;
	for (;;) {
		if (read_text(log_path).find(line) != std::string::npos)
			return true;
		if (process.poll() || Clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

inline net::HttpResolver loopback_only() {
	return [](const std::string &host, net::Endpoint &out) {
		return net::resolve_ipv4(host, out, false);
	};
}

// GET `path` on 127.0.0.1:`port` until it answers 200.
inline bool wait_for_http(
		ProgramProcess &process, uint16_t port, const char *path = "/api/health") {
	const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
	const auto deadline = Clock::now() + kHttpDeadline;
	for (;;) {
		const net::HttpReply reply = net::http_exchange("GET", url, {}, {}, 2000, loopback_only());
		if (reply.transport_ok && reply.code == 200)
			return true;
		if (process.poll() || Clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

// True when the `lines` appear in `log` in this order.
inline bool logged_in_order(const std::string &log, const std::vector<const char *> &lines) {
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

// The port a listener logged as the one it bound (the line `prefix` starts,
// the port after it), 0 when no line names one.
inline uint16_t logged_port(const std::string &log, const std::string &prefix) {
	const size_t at = log.find(prefix);
	if (at == std::string::npos)
		return 0;
	const unsigned long port = std::strtoul(log.c_str() + at + prefix.size(), nullptr, 10);
	return port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 0;
}

// The connections left on `database`, waited out for a moment (a backend leaves
// pg_stat_activity just after its client closes).
inline int64_t connections_left(const test_db::TestDatabase &database) {
	opennova::db::Database admin(test_db::options_for(test_db::server_url()));
	int64_t left = -1;
	for (int i = 0; i < 50; ++i) {
		left = admin.query("SELECT COUNT(*) FROM pg_stat_activity WHERE datname = $1;",
							{ opennova::db::BindValue(database.name()) })[0]
					   .as_int(0)
					   .value_or(-1);
		if (left == 0)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}
	return left;
}

// backend/migrations' files: what a fresh database's boot applies.
inline size_t migration_count() {
	size_t count = 0;
	for (const auto &e : std::filesystem::directory_iterator(
				 std::filesystem::path(OPENNOVA_SOURCE_DIR) / "backend" / "migrations"))
		if (e.path().extension() == ".sql")
			++count;
	return count;
}

// A port free a moment ago on loopback, UDP or TCP (bound to 0 and let go): for
// the ports a program's config must name before another program binds them.
inline uint16_t free_udp_port() {
	uint16_t port = 0;
	net::Socket s = net::udp_bind(0, &port);
	net::close_socket(s);
	return port;
}
inline uint16_t free_tcp_port() {
	uint16_t port = 0;
	net::Socket s = net::tcp_listen(0, 4, &port, true);
	net::close_socket(s);
	return port;
}

// What every program's process is given: the test's database, the repo's
// migrations and seed, web roots that do not exist (the HTTP layer serves its
// API and 404s the rest), a health port the OS picks, each but the database
// unless `more` names it, and `more` (each name once in the result).
inline std::vector<std::string> program_env(const test_temp::TempDir &temp,
		const std::string &db_url, const std::vector<std::string> &more) {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	std::vector<std::string> env = {
		"DATABASE_URL=" + db_url,
	};
	const std::vector<std::string> defaults = {
		"ONNET_HEALTH_PORT=0",
		"MIGRATIONS_DIR=" + (source_dir / "backend" / "migrations").string(),
		"SEED_DIR=" + (source_dir / "backend" / "seed").string(),
		"WEB_DIST_DIR=" + temp.file("web_dist"),
		"TEMPLATES_DIR=" + temp.file("templates"),
		"STATIC_DIR=" + temp.file("static"),
	};
	for (const std::string &d : defaults) {
		const std::string name = d.substr(0, d.find('=') + 1);
		bool named = false;
		for (const std::string &m : more)
			named = named || m.rfind(name, 0) == 0;
		if (!named)
			env.push_back(d);
	}
	env.insert(env.end(), more.begin(), more.end());
	return env;
}

// Spawns `process` and waits for "[boot] ready."; false, having said why, when
// it exits first or misses the boot deadline.
inline bool boot(ProgramProcess &process, const std::string &binary,
		const std::vector<std::string> &env, const std::string &cwd, const std::string &log_path) {
	if (!process.spawn(binary, env, cwd, log_path)) {
		std::fprintf(stderr, "  fork failed\n");
		return false;
	}
	if (wait_for_log(process, log_path, "[boot] ready.", kBootDeadline))
		return true;
	if (process.exited)
		std::fprintf(stderr, "  exited before ready (wait status 0x%x)\n",
				static_cast<unsigned>(process.status));
	else
		std::fprintf(stderr, "  not ready within the boot deadline\n");
	return false;
}

} // namespace program_process
