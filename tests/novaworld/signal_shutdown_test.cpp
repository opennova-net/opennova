// The NovaWorld service's programs stop in order on SIGINT (Ctrl+C) and on
// SIGTERM (what `docker stop` and an ECS task's stop send). The real binaries
// run as child processes (program_process.h) on an empty database of its own
// (common/test_database.h),
// which its shared boot migrates from backend/migrations, with every port 0:
// the OS picks each one as the listener binds it, and the test reads the port
// back from the listener's "listening" line. Once it is ready it gets the
// signal, and it must exit 0 within the deadline, having logged its shutdown
// legs in order and closed every database connection: none is left on its
// database once it has exited.
//
// SIGINT stops a lobby (its NW UDP listener's receive thread). SIGTERM stops a
// web process: the test first waits for Crow to answer, then holds a half-sent
// request open across the signal, so neither Crow's own signal handling nor a
// lingering connection can keep the process up.
//
// On Linux a third run checks the second signal: the first one's handler puts
// the default action back (read from /proc's SigCgt mask) while the shutdown is
// still pending, held off by a long tick, and the same signal again then ends
// the process at once.
//
// Web keeps its state in Redis (the test's server, common/test_redis.h), and
// one more run points it at a Redis that does not answer: the boot stops
// there, exit 1, before any listener serves, and the log names the URL with
// its password masked.
//
// What each program binds and advertises, and its own checks, are
// programs_test's.

#include "program_process.h"

#include "common/test_expect.h"
#include "common/test_redis.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace program_process;

namespace {

// A program's environment for these runs: every port 0, the lobby's and the
// web endpoint named (never served here), and web's Redis.
std::vector<std::string> program_env_of(const test_temp::TempDir &temp, const std::string &db_url) {
	return program_env(temp, db_url,
			{
					"ONNET_PUBLIC_HOST=127.0.0.1",
					"ONNET_WEB_HOST=127.0.0.1",
					"ONNET_LOBBY_ENDPOINT=127.0.0.1:64206",
					"ONNET_GATE_UDP_PORT=0",
					"ONNET_NW_UDP_PORT=0",
					"ONNET_HTTP_PORT=0",
					"ONNET_REDIS_URL=" + test_redis::server_url(),
			});
}

int exercise_shutdown(ProgramProcess &server, const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path, int signal_number) {
	const char *program = signal_number == SIGINT ? "lobby" : "web";
	const std::string &binary = signal_number == SIGINT ? bin.lobby : bin.web;
	if (!boot(server, binary, program_env_of(temp, database.conninfo()), temp.root(), log_path))
		return 1;
	const std::string booted = read_text(log_path);
	TEST_EXPECT(booted.find("{\"boot\":{\"program\":\"" + std::string(program) + "\"}}") !=
			std::string::npos);
	TEST_EXPECT(booted.find("[boot] migrations: " + std::to_string(migration_count()) +
						" applied, 0 skipped") != std::string::npos);

	net::ScopedSocket lingering;
	if (signal_number == SIGTERM) {
		const uint16_t http_port = logged_port(booted, kHttpLine);
		TEST_EXPECT(http_port != 0);
		TEST_EXPECT(wait_for_http(server, http_port));
		// A request that never finishes: the headers' blank line is never sent.
		net::Endpoint to;
		TEST_EXPECT(net::resolve_ipv4("127.0.0.1", to, false));
		to.port = http_port;
		lingering = net::ScopedSocket(net::tcp_connect(to, 2000));
		TEST_EXPECT(lingering.is_valid());
		const std::string partial = "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n";
		TEST_EXPECT(net::tcp_send_all(lingering.get(),
				reinterpret_cast<const uint8_t *>(partial.data()), partial.size()));
	}

	TEST_EXPECT(::kill(server.pid, signal_number) == 0);
	const auto signalled = Clock::now();
	if (!server.wait_exit(kExitDeadline)) {
		std::fprintf(stderr, "  still running %lld s after the signal\n",
				static_cast<long long>(kExitDeadline.count()));
		return 1;
	}
	if (WIFSIGNALED(server.status))
		std::fprintf(stderr, "  killed by signal %d\n", WTERMSIG(server.status));
	TEST_EXPECT(server.exit_code() == 0);
	const auto exit_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - signalled).count();

	const std::string log = read_text(log_path);
	if (signal_number == SIGINT) {
		TEST_EXPECT(logged_in_order(log,
				{
						"[boot] ready.",
						"[shutdown] stopping listeners",
						"[nwudp] loop exiting",
						"==== opennova-novaworld-lobby stopped ====",
				}));
	} else {
		TEST_EXPECT(logged_in_order(log,
				{
						"[boot] ready.",
						"[shutdown] stopping listeners",
						"[http] loop exiting",
						"==== opennova-novaworld-web stopped ====",
				}));
	}
	// Every connection closed.
	TEST_EXPECT(connections_left(database) == 0);
	std::printf("  %s: exit 0 %lld ms after the signal, shutdown in order, connections closed\n",
			program, static_cast<long long>(exit_ms));
	return 0;
}

#ifdef __linux__
// Whether `pid` has a handler for `signal_number`: its SigCgt mask in /proc.
bool catches(pid_t pid, int signal_number) {
	const std::string status = test_io::read_file_text("/proc/" + std::to_string(pid) + "/status");
	const size_t at = status.find("SigCgt:");
	if (at == std::string::npos)
		return false;
	const unsigned long long mask = std::strtoull(status.c_str() + at + 7, nullptr, 16);
	return ((mask >> (signal_number - 1)) & 1ULL) != 0;
}

// The first signal raises the shutdown flag and puts the default action back.
// The tick loop, sleeping a minute between looks at the flag, leaves that
// shutdown pending (as a stuck one would be), and the second signal ends the
// process at once.
int exercise_second_signal(ProgramProcess &server, const Binaries &bin,
		const test_temp::TempDir &temp, const test_db::TestDatabase &database,
		const std::string &log_path, int signal_number) {
	std::vector<std::string> env = program_env_of(temp, database.conninfo());
	env.push_back("TICK_INTERVAL_MS=60000");
	if (!boot(server, bin.lobby, env, temp.root(), log_path))
		return 1;
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

// A Redis web cannot reach (a closed port) stops its boot: exit 1 with the
// FATAL line, no listener started, every database connection closed, and the
// URL's password masked in the log.
int exercise_redis_unreachable(ProgramProcess &server, const Binaries &bin,
		const test_temp::TempDir &temp, const test_db::TestDatabase &database,
		const std::string &log_path, int) {
	std::vector<std::string> env = program_env_of(temp, database.conninfo());
	for (std::string &e : env) {
		if (e.rfind("ONNET_REDIS_URL=", 0) == 0)
			e = "ONNET_REDIS_URL=redis://probe:pw-s3cret@127.0.0.1:1";
	}
	TEST_EXPECT(server.spawn(bin.web, env, temp.root(), log_path));
	if (!server.wait_exit(kBootDeadline)) {
		std::fprintf(stderr, "  still booting %lld s after the start\n",
				static_cast<long long>(kBootDeadline.count()));
		return 1;
	}
	TEST_EXPECT(server.exit_code() == 1);
	const std::string log = read_text(log_path);
	TEST_EXPECT(log.find("[boot] FATAL: redis unreachable") != std::string::npos);
	TEST_EXPECT(log.find("redis://probe:***@127.0.0.1:1") != std::string::npos);
	TEST_EXPECT(log.find("pw-s3cret") == std::string::npos);
	TEST_EXPECT(log.find(kHttpLine) == std::string::npos);
	TEST_EXPECT(log.find("[boot] ready.") == std::string::npos);
	TEST_EXPECT(connections_left(database) == 0);
	std::printf("  exit 1 at boot, before any listener, the URL's password masked\n");
	return 0;
}

using Exercise = int (*)(ProgramProcess &, const Binaries &, const test_temp::TempDir &,
		const test_db::TestDatabase &, const std::string &, int);

int run_case(const Binaries &bin, Exercise exercise, int signal_number, const char *name) {
	// The TempDir and the database outlive the child: ProgramProcess is
	// destroyed (and the child killed, on failure) before they go. The
	// database is empty: the server migrates and seeds it at boot.
	test_temp::TempDir temp((std::string("nw_signal_") + name).c_str());
	const test_db::TestDatabase database({ /*migrate=*/false });
	const std::string log_path = temp.file("program.log");
	std::printf("%s:\n", name);
	int rc = 0;
	{
		ProgramProcess server;
		rc = exercise(server, bin, temp, database, log_path, signal_number);
	}
	if (rc != 0)
		std::fprintf(stderr, "FAIL: %s\n---- program log ----\n%s---- end ----\n", name,
				read_text(log_path).c_str());
	return rc;
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
	int rc = run_case(bin, exercise_shutdown, SIGINT, "sigint");
	if (rc == 0)
		rc = run_case(bin, exercise_shutdown, SIGTERM, "sigterm");
#ifdef __linux__
	if (rc == 0)
		rc = run_case(bin, exercise_second_signal, SIGINT, "second_sigint");
#endif
	if (rc == 0)
		rc = run_case(bin, exercise_redis_unreachable, 0, "redis_unreachable");
	net::shutdown();
	if (rc == 0)
		std::printf("OK: novaworld programs' signal shutdown\n");
	return rc;
}
