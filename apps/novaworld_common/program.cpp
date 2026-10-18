#include "program.h"

#include "health_listener.h"
#include "loggable.h"
#include "program_log.h"
#include "program_usage.h"

#include <base/io/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::program {

namespace {

std::atomic<bool> g_shutdown{ false };

// The first signal starts the orderly shutdown; the handler then puts the
// default action back, so the same signal again ends the process even when
// that shutdown is stuck. Both calls are signal-safe: a lock-free atomic store,
// and std::signal for the signal being handled. Not as a PID namespace's init,
// though: the kernel drops a default-action SIGINT or SIGTERM sent to a
// container's PID 1 (pid_namespaces(7)), so the second one would do nothing
// there. The ECS task definitions and the compose files run each program
// under an init (initProcessEnabled; `init: true`), which is PID 1 and
// forwards the signals.
void on_signal(int sig) {
	g_shutdown.store(true);
	std::signal(sig, SIG_DFL);
}

// SIGINT and SIGTERM (Ctrl+C; `docker stop` and an ECS task's stop send
// SIGTERM) raise the shutdown flag and put the signal's default action back.
void install_signal_handlers() {
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
}

// Every seed file, each a script the server runs as one transaction. The
// caller holds the migration lock, so two processes booting on one database
// seed it one after the other.
void apply_seed(db::Database &db, const std::filesystem::path &seed_dir, bool include_dev_users) {
	if (!std::filesystem::exists(seed_dir)) {
		std::printf("[boot] seed dir %s missing; skipping seed\n", seed_dir.string().c_str());
		return;
	}
	std::vector<std::filesystem::path> files;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() != ".sql")
			continue;
		// Dev-only seed (the `test`/`foo` accounts with public passwords) is
		// skipped unless SEED_DEV_USERS is set — production must not create them.
		if (!include_dev_users &&
				e.path().filename().string().find("dev_users") != std::string::npos) {
			std::printf(
					"[boot] seed skipped (dev-only): %s\n", e.path().filename().string().c_str());
			continue;
		}
		files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	for (const auto &p : files) {
		std::ifstream in(p, std::ios::binary);
		std::ostringstream os;
		os << in.rdbuf();
		try {
			db.exec_script(os.str());
			std::printf("[boot] seed applied: %s\n", p.filename().string().c_str());
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[boot] seed FAILED: %s — %s\n", p.filename().string().c_str(),
					e.what());
		}
	}
}

// The engine/ diagnostic channel (io/log.h): libraries are silent until the host
// installs a sink. Reproduce the historical stream split — lifecycle to stdout,
// warnings and errors to stderr; per-tick kDebug tracing opts in via NW_LOG_DEBUG.
// A message is one line: its control bytes go out escaped (loggable), so a
// name a NovaWorld session carried into an engine log line cannot forge another,
// and every registered log secret masked (a hosted server's admin password).
bool g_log_debug_enabled = false;
void app_log_sink(io::LogLevel level, const char *msg) {
	if (level == io::LogLevel::kDebug && !g_log_debug_enabled)
		return;
	const std::string text = mask_log_secrets(msg);
	std::fprintf(level >= io::LogLevel::kWarn ? stderr : stdout, "%s\n",
			loggable(text, text.size()).c_str());
}

} // namespace

int fatal(const std::string &what) {
	std::fprintf(stderr, "[boot] FATAL: %s\n", what.c_str());
	return 1;
}

bool is_endpoint(const std::string &text) {
	const size_t colon = text.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size() ||
			colon + 6 < text.size())
		return false;
	unsigned port = 0;
	for (size_t i = colon + 1; i < text.size(); ++i) {
		if (text[i] < '0' || text[i] > '9')
			return false;
		port = port * 10 + static_cast<unsigned>(text[i] - '0');
	}
	return port >= 1 && port <= 65535;
}

int run_program(const Program &program) {
	g_log_debug_enabled = std::getenv("NW_LOG_DEBUG") != nullptr;
	io::set_log_sink(&app_log_sink);

	// Force unbuffered stdout/stderr so log lines actually flush when
	// running under a non-tty parent (background pipe, file redirect,
	// etc). _IOLBF crashed MSVC's CRT with STATUS_STACK_BUFFER_OVERRUN —
	// _IONBF is the safe form on Windows.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef SIGPIPE
	// A write to a connection its peer closed (the Redis client's: hiredis and
	// OpenSSL write to the socket with no MSG_NOSIGNAL; the health listener's
	// reply to a probe that left) fails with EPIPE, which the caller handles,
	// instead of ending the process.
	std::signal(SIGPIPE, SIG_IGN);
#endif

	// A directory log store (the local stack's) gets a copy of every line from
	// the first (program_log.h); a cloud one is filled by ECS's log driver. Put
	// back, every line copied, as the program returns (the tee outlives
	// everything below).
	std::string log_store_error;
	const LogStoreSetting log_store = log_store_from_env(log_store_error);
	LogTee tee;
	std::string tee_error;
	if (log_store.kind == LogStoreSetting::Kind::Directory &&
			tee.start(log_store.location, program.name, local_task_id(), tee_error))
		set_log_tee(&tee);
	const struct TeeUnset {
		~TeeUnset() { set_log_tee(nullptr); }
	} tee_unset;

	// One boot line per process start, before anything can fail, which the log
	// pipeline counts per program (the crash-loop alarms).
	std::printf("{\"boot\":{\"program\":\"%s\"}}\n", program.name);
	std::printf("==== opennova-novaworld-%s booting ====\n", program.name);
	if (!log_store_error.empty())
		std::fprintf(stderr, "[boot] WARN %s: no log store\n", log_store_error.c_str());
	if (!tee_error.empty())
		std::fprintf(stderr, "[boot] WARN the log's copy into %s: %s\n", log_store.location.c_str(),
				tee_error.c_str());

	ServerConfig config = ServerConfig::from_env();
	// The program's own settings, before the database is touched: a program
	// missing one does nothing else.
	if (program.configure) {
		if (const std::string why = program.configure(config); !why.empty())
			return fatal(why);
	}
	std::printf("  public_host=%s  web_host=%s\n", config.public_host.c_str(),
			loggable(config.web_host).c_str());
	std::printf("  gate_udp=:%u  nw_udp=:%u  http=:%u\n", config.gate_udp_port, config.nw_udp_port,
			config.http_port);
	std::printf("  database=%s (pool of %zu)\n", db::masked_url(config.database_url).c_str(),
			config.database_pool_max);

	// --- DB: connect, migrate, seed --------------------------------------
	// The password never reaches a log line.
	add_log_secret(config.database_password);
	db::ConnectOptions db_options;
	db_options.url = config.database_url;
	db_options.password = config.database_password;
	db_options.application_name = std::string("opennova-novaworld-") + program.name;
	// Every thread that touches the database owns its connection (Database is
	// single-threaded), and a thread holds one lease at a time (database.h).
	// The pool outlives every part of the program (declared first, destroyed
	// last); the boot's lease goes back before the program starts its parts.
	std::unique_ptr<db::ConnectionPool> db_pool;
	try {
		db_pool = std::make_unique<db::ConnectionPool>(db_options, config.database_pool_max);
		auto boot_lease = db_pool->acquire();
		// The migrations and the seeds under one hold of the migration lock,
		// whichever program boots first: the others wait for it, then find
		// nothing to apply.
		try {
			const db::SessionLock migrating(*boot_lease, db::kMigrationLock);
			auto r = db::run_migrations(*boot_lease, config.migrations_dir);
			std::printf("[boot] migrations: %zu applied, %zu skipped\n", r.applied.size(),
					r.skipped.size());
			apply_seed(*boot_lease, config.seed_dir, config.seed_dev_users);
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[boot] FATAL: migrations failed: %s\n", e.what());
			return 1;
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: open db failed: %s\n", e.what());
		return 1;
	}

	const Boot boot{ config, db_options, *db_pool };
	const int exit_code = program.run(boot);
	if (exit_code == 0)
		std::printf("==== opennova-novaworld-%s stopped ====\n", program.name);
	return exit_code;
}

int run_main_loop(
		const char *program, const Boot &boot, Watchdog &watchdog, const LoopHooks &hooks) {
	// The loop's connection: the heartbeat's counts and the program's tick (the
	// lobby's cleanup for a connection the manager's tick drops runs on this
	// thread's lease). Back in the pool before the program's shutdown, which
	// stops the threads (some leasing on this one).
	std::optional<db::ConnectionPool::Lease> lease;
	try {
		lease.emplace(boot.pool.acquire());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: the main loop's connection: %s\n", e.what());
		return 1;
	}

	// GET /livez on ONNET_HEALTH_PORT from this watchdog: the container's
	// health check and the load balancer's (the gate's target group).
	std::optional<HealthListener> health;
	if (hooks.health_listener) {
		health.emplace(watchdog);
		if (!health->start(boot.config.health_port))
			return fatal("the health listener could not bind TCP :" +
					std::to_string(boot.config.health_port) + " (ONNET_HEALTH_PORT)");
	}

	// The watchdog's own thread samples the program's loops every second, this
	// loop among them (a tick or a heartbeat stuck on the database stalls it
	// too). A loop stuck past the stall time ends this process (exit 70), and
	// its container runtime starts it again: nothing here can unstick a
	// thread, and no other program's process is touched.
	std::atomic<uint64_t> main_turns{ 0 };
	watchdog.watch(
			"main loop", [&main_turns] { return main_turns.load(std::memory_order_relaxed); });
	std::atomic<bool> loop_done{ false };
	std::thread watcher([&watchdog, &loop_done] {
		while (!loop_done.load()) {
			for (int i = 0; i < 10 && !loop_done.load(); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (const std::string stalled = watchdog.check(); !stalled.empty()) {
				std::fprintf(stderr,
						"[watchdog] FATAL the %s made no progress for %llu s; exiting %d\n",
						stalled.c_str(),
						static_cast<unsigned long long>(watchdog.stall_ms() / 1000),
						Watchdog::kStallExitCode);
				std::fflush(nullptr);
				// The line reaches the log's copy, if any, before the exit.
				drain_log_tee(500);
				std::_Exit(Watchdog::kStallExitCode);
			}
		}
	});

	// The program's own use of its task (program_usage.h): a sample now, then one
	// with every heartbeat, on the loop's connection.
	UsageSampler usage(program, find_usage_task());
	const auto sample_usage = [&usage](db::Database &db) {
		using namespace std::chrono;
		const auto wall_s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
		const auto steady_us =
				duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
		try {
			insert_usage_sample(
					db, usage.take(static_cast<int64_t>(wall_s), static_cast<uint64_t>(steady_us)));
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[usage] WARN the sample: %s\n", e.what());
		}
	};
	sample_usage(**lease);

	install_signal_handlers();
	std::printf("[boot] ready. Ctrl+C to stop.\n");

	uint64_t last_heartbeat_ms = Watchdog::steady_now_ms();
	db::LockFailures lock_failures_reported = db::lock_failures();
	while (!g_shutdown.load()) {
		db::Database &db = **lease;
		std::this_thread::sleep_for(std::chrono::milliseconds(boot.config.tick_interval_ms));
		main_turns.fetch_add(1, std::memory_order_relaxed);
		const uint64_t now = Watchdog::steady_now_ms();
		if (hooks.tick)
			hooks.tick(db, now);
		// The heartbeat line (health.h), every kHeartbeatMs: the program's
		// counts now, and what happened since the last line.
		if (now - last_heartbeat_ms >= kHeartbeatMs) {
			const auto window_s = static_cast<int64_t>((now - last_heartbeat_ms + 999) / 1000);
			last_heartbeat_ms = now;
			Heartbeat beat;
			beat.program = program;
			if (hooks.heartbeat) {
				try {
					hooks.heartbeat(db, beat, window_s);
				} catch (const db::Error &e) {
					std::fprintf(stderr, "[heartbeat] WARN counts: %s\n", e.what());
				}
			}
			const db::LockFailures failures = db::lock_failures();
			beat.db_lock_timeouts = static_cast<int64_t>(
					failures.lock_timeouts - lock_failures_reported.lock_timeouts);
			beat.db_deadlocks =
					static_cast<int64_t>(failures.deadlocks - lock_failures_reported.deadlocks);
			lock_failures_reported = failures;
			std::printf("%s\n", heartbeat_line(beat).c_str());
			sample_usage(db);
		}
	}
	// The shutdown is the program's: no watchdog over it (a second signal ends
	// a stuck one), and no health answer once it has begun.
	loop_done.store(true);
	watcher.join();
	if (health)
		health->stop();
	return 0;
}

} // namespace opennova::novaworld_server::program
