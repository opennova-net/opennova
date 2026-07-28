// nw-server — the headless in-match game HOST (libs/npruntime P6). A pure C++ dedicated server: it
// loads a mission, stands up the npruntime runtime as a NovaWorld HostOnly session, opens a real UDP
// socket, and drives the in-match host loop at the
// original 62 Hz cadence so retail-wire-compatible clients (opennova or, as a follow-up, stock retail)
// can join -> spawn -> play. All protocol/crypto/framing live in the libs; this binary only owns the
// socket + the cadence (npruntime/host_session.h is the shared owner loop, also used by the P6 test).
//
// It NEVER links godot-cpp (godot-cpp is a separate SCons build, not in this CMake graph). Separate from
// the matchmaking apps/novaworld_server (gate/lobby/HTTP) — this is the authoritative game server.

#include <npwire/net_ports.h>
#include <npruntime/host_session.h> // the host owner loop, promoted to libs/npruntime (P7/A3)

#include "net_datagram_socket.h" // net::Socket-backed netsim::IDatagramSocket adapter
#include "net_sockets.h"         // net::startup / udp_bind / ScopedSocket

#include <mission/mission.h> // MissionDocument
#include <mission/promote.h> // promote_mission

#include <world/ai.h>
#include <world/world.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <io/log.h>

namespace {

std::atomic<bool> g_shutdown{false};
void on_signal(int) { g_shutdown.store(true); }

uint16_t env_port(const char *name, uint16_t fallback) {
	if (const char *v = std::getenv(name)) {
		const int p = std::atoi(v);
		if (p > 0 && p < 65536) return static_cast<uint16_t>(p);
	}
	return fallback;
}

} // namespace


namespace {
// The libs/ diagnostic channel (io/log.h): libraries are silent until the host
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

	// Mission source: NW_MISSION env var (no committed fixture — ADR 0003 forbids inventing one).
	const char *mission_path = std::getenv("NW_MISSION");
	if (mission_path == nullptr || *mission_path == '\0') {
		std::fprintf(stderr, "nw-server: set NW_MISSION=<path-to .bms> (the mission to host)\n");
		return 2;
	}
	const uint16_t port = env_port("NW_LAN_PORT", opennova::kRetailLanPortMin); // the retail mpnovaworldport default

	// --- Load the mission + build the authoritative World (pools + nav + AI brains). ---
	mission::MissionDocument doc;
	if (!doc.load_bms_file(mission_path)) {
		std::fprintf(stderr, "nw-server: failed to load mission '%s'\n", mission_path);
		return 1;
	}
	world::World world;
	world::AiSystem ai;
	world.ai = &ai; // Server_BuildPlayerInfoAndAdd requires an AiSystem to spawn a player
	const mission::PromoteResult pr = mission::promote_mission(doc.bms_file(), world, ai);
	std::fprintf(stderr, "nw-server: promoted '%s' (%d entities, %d brains, %d nav nodes)\n",
	             mission_path, pr.spawned, pr.brains, pr.nav_nodes);

	// --- Open the UDP socket. ---
	if (net::startup() != 0) {
		std::fprintf(stderr, "nw-server: winsock init failed\n");
		return 1;
	}
	uint16_t bound = 0;
	net::ScopedSocket sock(net::udp_bind(port, &bound));
	if (!sock.is_valid()) {
		std::fprintf(stderr, "nw-server: bind on UDP %u failed\n", port);
		net::shutdown();
		return 1;
	}

	// --- Stand up the npruntime runtime as a dedicated HostOnly server. There is no synthetic
	//     loopback player; every roster row belongs to an admitted remote peer. ---
	np::HostOwner owner;
	owner.ctx.world = &world;
	owner.ctx.mission = &doc.bms_file();

	np::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova nw-server";
	host_cfg.config.max_players = 16;
	host_cfg.socket_mode = np::SocketMode::Lan; // a real LAN socket (Socketless=1 would be in-process SP)
	host_cfg.serve_and_play = false;            // headless dedicated host: no local-player registration
	np::start_host_session(owner, host_cfg);

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::fprintf(stderr, "nw-server: hosting on UDP %u at 62 Hz (Ctrl+C to stop)\n", bound);

	// --- The fixed 62 Hz host loop. Absolute-deadline sleep_until so the cadence does not drift. ---
	using clock = std::chrono::steady_clock;
	const auto baseline = clock::now();
	constexpr int64_t kPeriodNs = 1000000000LL / 62; // ~16.129 ms per engine tick (the original cadence)
	net::NetDatagramSocket dgram(sock.get()); // recv_timeout_ms = 0 (non-blocking; the loop self-paces)
	for (uint64_t frame = 0; !g_shutdown.load(); ++frame) {
		np::host_session_pump(owner, dgram);
		std::this_thread::sleep_until(
				baseline + std::chrono::nanoseconds(static_cast<int64_t>(frame + 1) * kPeriodNs));
	}

	std::fprintf(stderr, "nw-server: shutting down\n");
	net::shutdown();
	return 0;
}
