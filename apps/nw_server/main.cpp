// nw-server — the headless in-match game HOST (libs/npruntime P6). A pure C++ Listen server: it loads a
// mission, stands up the npruntime runtime as a NovaWorld listen host (ConnectionMode::HostClient, the
// witnessed SP/co-op shape, §5.0), opens a real UDP socket, and drives the in-match host loop at the
// original 62 Hz cadence so retail-wire-compatible clients (opennova or, as a follow-up, stock retail)
// can join -> spawn -> play. All protocol/crypto/framing live in the libs; this binary only owns the
// socket + the cadence (host_owner_loop.h is the shared owner loop, also used by the two-endpoint test).
//
// It NEVER links godot-cpp (godot-cpp is a separate SCons build, not in this CMake graph). Separate from
// the matchmaking apps/novaworld_server (gate/lobby/HTTP) — this is the authoritative game server.

#include "host_owner_loop.h"

#include <npruntime/server_session.h> // set_connection_mode / set_transport_mode / create_session / start
#include <npruntime/server_spawn.h>   // Server_InitNewRoundState

#include <netsim/loopback_channel.h> // the host's own dcb-2 client (Listen host)

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

int main() {
	using namespace opennova;
	namespace nw = opennova::nw_server;

	// Mission source: NW_MISSION env var (no committed fixture — ADR 0003 forbids inventing one).
	const char *mission_path = std::getenv("NW_MISSION");
	if (mission_path == nullptr || *mission_path == '\0') {
		std::fprintf(stderr, "nw-server: set NW_MISSION=<path-to .bms> (the mission to host)\n");
		return 2;
	}
	const uint16_t port = env_port("NW_LAN_PORT", 32768); // ONNET_CLIENT_REFLECT_NOVAWORLD_PORT default

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

	// --- Stand up the npruntime runtime as a LISTEN host (HostClient + a loopback host player at
	//     dcb 2; joiners get dcb 3+, reproducing the retail LAN host/join scheme, §5.0 / §5.2a). ---
	nw::HostOwner owner;
	netsim::LoopbackChannel host_loop; // the host's own dcb-2 client (its 0x0A drained in-process)
	owner.host_loopback = &host_loop;
	owner.ctx.world = &world;
	owner.ctx.mission = &doc.bms_file();

	np::NapiGameSettings settings;
	settings.server_name = "OpenNova nw-server";
	settings.max_players = 16;
	np::SessionStartup startup; // deterministic host-start values (a real host mints host_key randomly)
	startup.host_key = 0;

	np::set_connection_mode(owner.ctx, np::ConnectionMode::HostClient); // §5.0 mode 3 (host + client)
	np::set_transport_mode(owner.ctx, np::SocketMode::Lan);             // a real LAN socket (was 1 for SP)
	np::create_session(owner.ctx, settings, startup, &host_loop);       // registers the dcb-2 loopback
	np::configure_session_runtime(owner.ctx, {});                       // ONCE — never mid-match (D-NET-124)
	np::Server_InitNewRoundState(owner.ctx); // §5.2a step 1 (the host player then spawns via the loop)

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::fprintf(stderr, "nw-server: hosting on UDP %u at 62 Hz (Ctrl+C to stop)\n", bound);

	// --- The fixed 62 Hz host loop. Absolute-deadline sleep_until so the cadence does not drift. ---
	using clock = std::chrono::steady_clock;
	const auto baseline = clock::now();
	constexpr int64_t kPeriodNs = 1000000000LL / 62; // ~16.129 ms per engine tick (the original cadence)
	for (uint64_t frame = 0; !g_shutdown.load(); ++frame) {
		nw::host_owner_pump(owner, sock.get());
		std::this_thread::sleep_until(
				baseline + std::chrono::nanoseconds(static_cast<int64_t>(frame + 1) * kPeriodNs));
	}

	std::fprintf(stderr, "nw-server: shutting down\n");
	net::shutdown();
	return 0;
}
