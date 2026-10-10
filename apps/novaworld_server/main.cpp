// Standalone novaworld server entrypoint.
//
// Wires the gate UDP, NW UDP, HTTP listeners onto a single SQLite-backed
// process. Architecture and design rationale: the README.md beside this file.
// This is our NovaWorld server, not an emulator (CONTEXT.md "NovaWorld").

#include "gate_listener.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#ifdef OPENNOVA_HTTP_ENABLED
#include "auth.h"
#include "http_listener.h"
#include "session_store.h"
#include "web_session.h"
#endif

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/unknown_tracker.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <cstdlib>
#include <base/io/log.h>

namespace {

std::atomic<bool> g_shutdown{false};

// The first signal starts the orderly shutdown; the handler then puts the
// default action back, so the same signal again ends the process even when
// that shutdown is stuck. Both calls are signal-safe: a lock-free atomic store,
// and std::signal for the signal being handled. (Windows' CRT resets a handler
// to SIG_DFL before calling it anyway, so a second Ctrl+C there already ended
// the process; the reset brings POSIX to the same behavior.) Not as a PID
// namespace's init, though: the kernel drops a default-action SIGINT or SIGTERM
// sent to a container's PID 1 (pid_namespaces(7)), so the second one would do
// nothing there. deploy/compose runs the server under Docker's init
// (`init: true`), which is PID 1 and forwards the signals.
void on_signal(int sig) {
	g_shutdown.store(true);
	std::signal(sig, SIG_DFL);
}

// main() alone owns SIGINT and SIGTERM (Ctrl+C; `docker stop` sends SIGTERM):
// the handler raises g_shutdown (and restores the default action, above), and
// the tick loop then runs the orderly shutdown. The HTTP listener keeps Crow's
// own signal handling off both.
void install_signal_handlers() {
	std::signal(SIGINT,  on_signal);
	std::signal(SIGTERM, on_signal);
}

void apply_seed(opennova::db::Database &db,
                const std::filesystem::path &seed_dir,
                bool include_dev_users) {
	if (!std::filesystem::exists(seed_dir)) {
		std::printf("[boot] seed dir %s missing; skipping seed\n",
		            seed_dir.string().c_str());
		return;
	}
	std::vector<std::filesystem::path> files;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() != ".sql") continue;
		// Dev-only seed (the `test`/`foo` accounts with public passwords) is
		// skipped unless SEED_DEV_USERS is set — production must not create them.
		if (!include_dev_users &&
		    e.path().filename().string().find("dev_users") != std::string::npos) {
			std::printf("[boot] seed skipped (dev-only): %s\n",
			            e.path().filename().string().c_str());
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
			std::printf("[boot] seed applied: %s\n",
			            p.filename().string().c_str());
		} catch (const opennova::db::SqliteError &e) {
			std::fprintf(stderr,
			             "[boot] seed FAILED: %s — %s\n",
			             p.filename().string().c_str(), e.what());
		}
	}
}

} // namespace


namespace {
// The engine/ diagnostic channel (io/log.h): libraries are silent until the host
// installs a sink. Reproduce the historical stream split — lifecycle to stdout,
// warnings and errors to stderr; per-tick kDebug tracing opts in via NW_LOG_DEBUG.
bool g_log_debug_enabled = false;
void app_log_sink(opennova::io::LogLevel level, const char *msg) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug_enabled) return;
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n", msg);
}
} // namespace

int main() {
	g_log_debug_enabled = std::getenv("NW_LOG_DEBUG") != nullptr;
	opennova::io::set_log_sink(&app_log_sink);
	using namespace opennova;
	using namespace opennova::novaworld_server;

	// Force unbuffered stdout/stderr so log lines actually flush when
	// running under a non-tty parent (background pipe, file redirect,
	// etc). _IOLBF crashed MSVC's CRT with STATUS_STACK_BUFFER_OVERRUN —
	// _IONBF is the safe form on Windows.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	const ServerConfig config = ServerConfig::from_env();

	std::printf("==== opennova-novaworld-server booting ====\n");
	std::printf("  public_host=%s\n", config.public_host.c_str());
	std::printf("  gate_udp=:%u  nw_udp=:%u  http=:%u\n",
	            config.gate_udp_port, config.nw_udp_port, config.http_port);
	std::printf("  database=%s\n", config.database_path.string().c_str());

	// --- DB: ensure dir exists, open, migrate, seed ----------------------
	if (auto parent = config.database_path.parent_path(); !parent.empty()) {
		std::filesystem::create_directories(parent);
	}
	// Every thread that touches the database owns its connection (Database is
	// single-threaded): this thread, the gate's and the NW UDP listener's
	// receive threads each hold one for their lifetime (the listeners lease
	// theirs in start(), which fails the boot when the open does); each HTTP
	// request and each erase_lobby_state call leases one for the call. The pool
	// outlives every listener (declared first, destroyed last).
	std::unique_ptr<db::ConnectionPool> db_pool;
	std::optional<db::ConnectionPool::Lease> main_lease;
	try {
		db_pool = std::make_unique<db::ConnectionPool>(config.database_path);
		main_lease.emplace(db_pool->acquire());
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] FATAL: open db failed: %s\n", e.what());
		return 1;
	}
	db::Database &dbh = **main_lease;

	try {
		auto r = db::run_migrations(dbh, config.migrations_dir);
		std::printf("[boot] migrations: %zu applied, %zu skipped\n",
		            r.applied.size(), r.skipped.size());
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] FATAL: migrations failed: %s\n", e.what());
		return 1;
	}

	apply_seed(dbh, config.seed_dir, config.seed_dev_users);

#ifdef OPENNOVA_HTTP_ENABLED
	// The first website admin: ONNET_BOOTSTRAP_ADMIN names an existing account.
	if (!config.bootstrap_admin.empty()) {
		try {
			if (promote_to_admin(dbh, config.bootstrap_admin)) {
				std::printf("[boot] bootstrap admin: '%s' has the admin role\n",
				            config.bootstrap_admin.c_str());
			} else {
				std::fprintf(stderr,
				             "[boot] WARN bootstrap admin: no account named '%s'; nobody promoted\n",
				             config.bootstrap_admin.c_str());
			}
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[boot] WARN bootstrap admin: %s\n", e.what());
		}
	}
#endif

	// --- Connection manager (shared across listeners) ---------------------
	ConnectionManager manager(config.heartbeat_timeout_ms);
	manager.on_added([](const Connection &c) {
		std::printf("[conn] added id=0x%08x addr=%s pn=%s\n",
		            c.id, peer_addr_to_string(c.addr).c_str(), c.pn.c_str());
	});

	// --- Unknown-message tracker (shared across listeners) ----------------
	// Listener threads record() sightings of anything we don't handle; the
	// main tick loop flushes them into unknown_messages (never per-packet).
	UnknownTracker unknown_tracker;

	// --- Previous-run cleanup, before any listener serves -----------------
	// Phase I.2: drop stale active_hosts rows from the previous server
	// run before accepting new connections (the UDP HELLO from any
	// surviving retail process will fail anyway, so its prior row is
	// dead state).
	try {
		hostdb::clear_all(dbh);
		std::printf("[boot] active_hosts + host_players cleared (previous-run cleanup)\n");
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] WARN active_hosts clear: %s\n", e.what());
	}
#ifdef OPENNOVA_HTTP_ENABLED
	try {
		clear_all_active_user_sessions(dbh);
		std::printf("[boot] active_user_sessions cleared (previous-run cleanup)\n");
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] WARN active_user_sessions clear: %s\n", e.what());
	}
#endif

	// --- Listeners --------------------------------------------------------
	GateListener gate;
	gate.set_unknown_tracker(&unknown_tracker);
	gate.set_db_pool(db_pool.get());

	NwUdpListener nwudp(manager);
	nwudp.set_db_pool(db_pool.get());
	// The endpoint advertised to joiners uses the client's NOVAWORLD session port
	// (client_reflect_novaworld_port, 32768), NOT the gate port
	// (client_reflect_gate_port, 49152 — that's only the gate's
	// ReflectedPortNumber). See server_config.h.
	nwudp.set_reflect_endpoint(config.client_reflect_ip, config.client_reflect_novaworld_port);
	nwudp.set_unknown_tracker(&unknown_tracker);

	// Wired once NwUdpListener exists, before any listener starts, so the
	// lost-handler can clean up the per-connection lobby_state entry too.
	// Without this, a hosting retail process that exits without GOODBYE
	// (heartbeat timeout) would stay in /api/hosts forever (G.6). Pass the
	// dropped Connection's PeerAddr — lobby_states_ is keyed by addr, not by
	// ci (two retail processes both send ci=0x00000001) (G.7).
	manager.on_lost([&nwudp](const Connection &c, DropReason r) {
		std::printf("[conn] lost  id=0x%08x identity='%s' reason=%s\n",
		            c.id, c.identity.c_str(), drop_reason_name(r));
		nwudp.erase_lobby_state(c.addr, drop_reason_name(r));
	});

	// Each listener advertises its siblings' ports as the config it starts
	// with names them: the NW UDP listener the HTTP port (the SessionInit's
	// web domain), the gate both (UDPNOVAWORLD, STARTUPURL). So they start in
	// the order those ports become known, HTTP, then NW UDP, then the gate,
	// and once one binds, `listening` carries the port it serves: the
	// configured port, or the OS's pick for port 0. With nonzero ports every
	// advertised byte stays the configured port's; and the gate, the client's
	// first contact, answers only once what it names serves.
	ServerConfig listening = config;
#ifdef OPENNOVA_HTTP_ENABLED
	SessionStore sessions;
	HttpListener http(manager, *db_pool, sessions);
	http.set_unknown_tracker(&unknown_tracker);
	if (!http.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: HTTP listener start failed\n");
		return 1;
	}
	listening.http_port = http.bound_port();
#endif
	if (!nwudp.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: NW UDP listener start failed\n");
#ifdef OPENNOVA_HTTP_ENABLED
		http.stop();
#endif
		return 1;
	}
	listening.nw_udp_port = nwudp.bound_port();
	if (!gate.start(listening)) {
		std::fprintf(stderr, "[boot] FATAL: gate listener start failed\n");
#ifdef OPENNOVA_HTTP_ENABLED
		http.stop();
#endif
		nwudp.stop();
		return 1;
	}

	install_signal_handlers();
	std::printf("[boot] ready. Ctrl+C to stop.\n");

	// --- Main loop: tick the connection manager ---------------------------
	uint64_t last_host_sweep_ms = 0;
	while (!g_shutdown.load()) {
		std::this_thread::sleep_for(
			std::chrono::milliseconds(config.tick_interval_ms));
		using namespace std::chrono;
		const auto now = static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		if (auto dropped = manager.tick(now); dropped > 0) {
			std::printf("[tick] expired %zu connection(s)\n", dropped);
		}
		// Flush any unknown-message sightings the listener threads recorded
		// since the last tick into unknown_messages (deduped upsert).
		try {
			unknown_tracker.flush(dbh);
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}

		// policy: throttled backstop sweep of crash-orphaned host rows the
		// normal GOODBYE/StopHosting/timeout teardown missed.
		if (now - last_host_sweep_ms >= config.host_sweep_interval_ms) {
			last_host_sweep_ms = now;
			try {
				const int pruned = hostdb::prune_stale_hosts(
					dbh, static_cast<int64_t>(config.host_stale_window_ms / 1000));
				if (pruned > 0) {
					std::printf("[sweep] pruned %d stale host row(s)\n", pruned);
				}
			} catch (const db::SqliteError &e) {
				std::fprintf(stderr, "[sweep] WARN prune_stale_hosts: %s\n", e.what());
			}
#ifdef OPENNOVA_HTTP_ENABLED
			// Website sessions past their (sliding) expiry.
			try {
				if (const auto pruned = prune_expired_web_sessions(dbh); pruned > 0) {
					std::printf("[sweep] pruned %zu expired web session(s)\n", pruned);
				}
			} catch (const db::SqliteError &e) {
				std::fprintf(stderr, "[sweep] WARN prune_expired_web_sessions: %s\n", e.what());
			}
#endif
		}
	}

	// The HTTP listener first (Crow's threads joined: no request still holds a
	// lease), then the gate and NW UDP receive threads (each returns its lease
	// as it exits), then the connections. As main() returns, its locals are
	// destroyed in reverse declaration order, so main_lease and then the pool
	// (declared first) go last; the last close checkpoints the WAL.
	std::printf("[shutdown] stopping listeners\n");
#ifdef OPENNOVA_HTTP_ENABLED
	http.stop();
#endif
	gate.stop();
	nwudp.stop();
	manager.shutdown();
	std::printf("==== opennova-novaworld-server stopped ====\n");
	return 0;
}
