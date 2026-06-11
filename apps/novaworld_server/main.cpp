// Standalone novaworld server entrypoint.
//
// Wires the gate UDP, NW UDP, HTTP listeners onto a single SQLite-backed
// process. Architecture diagram + design rationale: notes/architecture.md.
// Per memory `feedback_novaworld_server_naming.md` this is "our" server,
// not an emulator — refer to it as the novaworld server, not the emulator.

#include "gate_listener.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#ifdef OPENNOVA_HTTP_ENABLED
#include "auth.h"
#include "http_listener.h"
#include "session_store.h"
#endif

#include <novaworld/connection/manager.h>
#include <novaworld/db/sqlite.h>
#include <novaworld/host_repository.h>
#include <novaworld/unknown_tracker.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace {

std::atomic<bool> g_shutdown{false};

void on_signal(int) {
	g_shutdown.store(true);
}

void install_signal_handlers() {
	std::signal(SIGINT,  on_signal);
	std::signal(SIGTERM, on_signal);
}

void apply_seed(opennova::db::Database &db,
                const std::filesystem::path &seed_dir) {
	if (!std::filesystem::exists(seed_dir)) {
		std::printf("[boot] seed dir %s missing; skipping seed\n",
		            seed_dir.string().c_str());
		return;
	}
	std::vector<std::filesystem::path> files;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql") files.push_back(e.path());
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

int main() {
	using namespace opennova;
	using namespace opennova::server;

	// Force unbuffered stdout/stderr so log lines actually flush when
	// running under a non-tty parent (background pipe, file redirect,
	// etc). _IOLBF crashed MSVC's CRT with STATUS_STACK_BUFFER_OVERRUN —
	// _IONBF is the safe form on Windows.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	const ServerConfig config = ServerConfig::from_env();

	std::printf("==== opennova-novaworld booting ====\n");
	std::printf("  public_host=%s\n", config.public_host.c_str());
	std::printf("  gate_udp=:%u  nw_udp=:%u  http=:%u\n",
	            config.gate_udp_port, config.nw_udp_port, config.http_port);
	std::printf("  database=%s\n", config.database_path.string().c_str());

	// --- DB: ensure dir exists, open, migrate, seed ----------------------
	if (auto parent = config.database_path.parent_path(); !parent.empty()) {
		std::filesystem::create_directories(parent);
	}
	std::unique_ptr<db::Database> dbh;
	try {
		dbh = std::make_unique<db::Database>(config.database_path);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] FATAL: open db failed: %s\n", e.what());
		return 1;
	}

	try {
		auto r = db::run_migrations(*dbh, config.migrations_dir);
		std::printf("[boot] migrations: %zu applied, %zu skipped\n",
		            r.applied.size(), r.skipped.size());
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] FATAL: migrations failed: %s\n", e.what());
		return 1;
	}

	apply_seed(*dbh, config.seed_dir);

	// --- Connection manager (shared across listeners) ---------------------
	ConnectionManager manager(config.heartbeat_timeout_ms);
	manager.on_added([](const Connection &c) {
		std::printf("[conn] added id=0x%08x addr=%u.%u.%u.%u:%u pn=%s\n",
		            c.id,
		            (c.addr.ip >> 24) & 0xff, (c.addr.ip >> 16) & 0xff,
		            (c.addr.ip >>  8) & 0xff,  c.addr.ip        & 0xff,
		            c.addr.port, c.pn.c_str());
	});

	// --- Unknown-message tracker (shared across listeners) ----------------
	// Listener threads record() sightings of anything we don't handle; the
	// main tick loop flushes them into unknown_messages (never per-packet).
	UnknownTracker unknown_tracker;

	// --- Listeners --------------------------------------------------------
	GateListener gate;
	gate.set_unknown_tracker(&unknown_tracker);
	if (!gate.start(config)) {
		std::fprintf(stderr, "[boot] FATAL: gate listener start failed\n");
		return 1;
	}

	NwUdpListener nwudp(manager);
	nwudp.set_database(dbh.get());
	nwudp.set_unknown_tracker(&unknown_tracker);
	// Phase I.2: drop stale active_hosts rows from the previous server
	// run before accepting new connections (the UDP HELLO from any
	// surviving retail process will fail anyway, so its prior row is
	// dead state).
	try {
		hostdb::clear_all(*dbh);
		std::printf("[boot] active_hosts + host_players cleared (previous-run cleanup)\n");
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] WARN active_hosts clear: %s\n", e.what());
	}
#ifdef OPENNOVA_HTTP_ENABLED
	try {
		clear_all_active_user_sessions(*dbh);
		std::printf("[boot] active_user_sessions cleared (previous-run cleanup)\n");
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[boot] WARN active_user_sessions clear: %s\n", e.what());
	}
#endif
	if (!nwudp.start(config)) {
		std::fprintf(stderr, "[boot] FATAL: NW UDP listener start failed\n");
		gate.stop();
		return 1;
	}

	// Wired after NwUdpListener exists so the lost-handler can clean up the
	// per-connection lobby_state entry too. Without this, a hosting retail
	// process that exits without GOODBYE (heartbeat timeout) would stay in
	// /api/hosts forever (G.6). Pass the dropped Connection's PeerAddr —
	// lobby_states_ is keyed by addr, not by ci (two retail processes both
	// send ci=0x00000001) (G.7).
	manager.on_lost([&nwudp](const Connection &c, DropReason r) {
		std::printf("[conn] lost  id=0x%08x identity='%s' reason=%s\n",
		            c.id, c.identity.c_str(), drop_reason_name(r));
		nwudp.erase_lobby_state(c.addr, drop_reason_name(r));
	});

#ifdef OPENNOVA_HTTP_ENABLED
	SessionStore sessions;
	HttpListener http(manager, *dbh, nwudp, sessions);
	http.set_unknown_tracker(&unknown_tracker);
	if (!http.start(config)) {
		std::fprintf(stderr, "[boot] FATAL: HTTP listener start failed\n");
		gate.stop();
		nwudp.stop();
		return 1;
	}
#endif

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
			unknown_tracker.flush(*dbh);
		} catch (const db::SqliteError &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}

		// policy: throttled backstop sweep of crash-orphaned host rows the
		// normal GOODBYE/StopHosting/timeout teardown missed.
		if (now - last_host_sweep_ms >= config.host_sweep_interval_ms) {
			last_host_sweep_ms = now;
			try {
				const int pruned = hostdb::prune_stale_hosts(
					*dbh, static_cast<int64_t>(config.host_stale_window_ms / 1000));
				if (pruned > 0) {
					std::printf("[sweep] pruned %d stale host row(s)\n", pruned);
				}
			} catch (const db::SqliteError &e) {
				std::fprintf(stderr, "[sweep] WARN prune_stale_hosts: %s\n", e.what());
			}
		}
	}

	std::printf("[shutdown] stopping listeners\n");
#ifdef OPENNOVA_HTTP_ENABLED
	http.stop();
#endif
	gate.stop();
	nwudp.stop();
	manager.shutdown();
	std::printf("==== opennova-novaworld stopped ====\n");
	return 0;
}
