#pragma once

// The ONE host bring-up / local request drain / per-tick session frame over a
// mission::MissionKernel (ADR 0042 d3) — the net half the group order keeps
// out of runtime/mission. Promoted from the retail-mission rig; every host
// embedder drives it: the ctest rig (tests/common/retail_mission_files), the
// shipping Godot Simulation (the shell TickTarget that owns a kernel), and
// the dedicated host (apps/nw_server).

#include <net/netsim/idatagram_socket.h>
#include <net/netsim/loopback_channel.h>
#include <net/npruntime/client_runtime.h>
#include <net/npruntime/host_session.h>

#include <cstdint>
#include <memory>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::inmatch {

// The SP listen session's net state over one kernel: the in-process loopback
// carrying the serve-and-play side's own dcb-2 client, the np host owner, and
// the local ClientRuntime folding that loopback into its ClientState.
struct ListenHostState {
	netsim::LoopbackChannel host_loop; // the local dcb-2 client; declared before the runtime
	np::HostOwner host_owner;
	std::unique_ptr<np::ClientRuntime> client_runtime;
	// Deterministic startup overrides for goldens and the tick digest (the
	// same three HostConfig fields): zero asks bringup's production helper to
	// mint the volatile retail values (the clock-mixed session seed also
	// seeds the world's CRT rand stream, D-NET-115).
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
};

namespace listen_host {

// The witnessed §5.0 listen-host bring-up: the npruntime ctx over the
// kernel's world, start_host_session (mode 3 -> create_session, which resets
// the connection table and installs the loopback -> auto-spawn of the local player
// at the start marker), and the local HostClient replica pipeline folding the
// loopback into its ClientState. Invoked by the kernel boot's
// bringup_net_session hook. [orig: SinglePlayer_StartMission @0x561af0]
void bringup(mission::MissionKernel &kernel, ListenHostState &state);

// The dedicated-host bring-up (HostOnly): the npruntime ctx over the kernel's
// world/mission and start_host_session with the embedder's consolidated
// HostConfig — no serve-and-play loopback, no local player, no local replica
// fold (serve_and_play is forced off; the caller authors everything else,
// socket mode included). The witnessed original makes serve-only a true
// host-only session [orig: HG_SERVEONLY -> CGameSession_SetConnectionMode(1),
// is_host=1/is_client=0; HostDialog read @0x555940, dispatch @0x556d00, mode
// switch @0x4c49f0]. Invoked by the kernel boot's bringup_net_session hook.
void bringup_dedicated(mission::MissionKernel &kernel, ListenHostState &state,
		const np::HostConfig &host_cfg);

// The local player's own C2S gameplay messages (the witnessed local reload
// producer) reach the SAME per-message server dispatcher a remote connection
// does; every other datagram stays queued for Server_TickUpdate's drain.
// frame() runs it first; the listen-host ctest drives it directly.
void drain_host_client_gameplay_requests(mission::MissionKernel &kernel,
		ListenHostState &state);

// The listen frame: the environment advance, input -> the local player's
// body, Server_TickUpdate (the C2S drain, ONE logic tick, the 0x0A fan)
// through the shared owner loop, the
// local view/weapon pumps, the local ClientState fold, then the new-soldier
// .adm ground. `viewport_height` feeds the S2C 0x68 wrap seam (0 for a
// headless embedder — npruntime then suppresses 0x68, D-NET-206); `perf` is
// optional phase attribution. [orig: Game_ProcessMainFrame @0x5263f0]
void frame(mission::MissionKernel &kernel, ListenHostState &state,
		netsim::IDatagramSocket &socket, int32_t viewport_height,
		np::HostSessionPerf *perf);

} // namespace listen_host

} // namespace opennova::inmatch
