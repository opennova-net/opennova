#pragma once

// The ONE SP listen bring-up / local request drain / per-tick session frame
// over a mission::MissionKernel (ADR 0042 d3) — the net half the group order
// keeps out of runtime/mission. Promoted from the retail-mission rig; the
// ctest rig (tests/common/retail_mission_files) drives it today.
// (STAGED, NOT WIRED into a shipping embedder yet: godot/src Simulation — the
// shell TickTarget that owns a kernel — and apps/nw_server fold onto it in
// the next ADR 0042 campaign slices.)

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
};

namespace listen_host {

// The witnessed §5.0 listen-host bring-up: the npruntime ctx over the
// kernel's world, start_host_session (mode 3 -> create_session over the
// loopback -> configure_session_runtime -> the auto-spawn of the local player
// at the start marker), and the local HostClient replica pipeline folding the
// loopback into its ClientState. Invoked by the kernel boot's
// bringup_net_session hook. [orig: SinglePlayer_StartMission @0x561af0]
void bringup(mission::MissionKernel &kernel, ListenHostState &state);

// The local player's own C2S gameplay messages (the witnessed local reload
// producer) reach the SAME per-message server dispatcher a remote connection
// does; every other datagram stays queued for Server_TickUpdate's drain.
void drain_host_client_gameplay_requests(mission::MissionKernel &kernel,
		ListenHostState &state);

// The listen frame: input -> the local player's body, Server_TickUpdate (the
// C2S drain, ONE logic tick, the 0x0A fan) through the shared owner loop, the
// local view/weapon pumps, the local ClientState fold, then the new-soldier
// .adm ground. `viewport_height` feeds the S2C 0x68 wrap seam (0 for a
// headless embedder — npruntime then suppresses 0x68, D-NET-206); `perf` is
// optional phase attribution. [orig: Game_ProcessMainFrame @0x5263f0]
void frame(mission::MissionKernel &kernel, ListenHostState &state,
		netsim::IDatagramSocket &socket, int32_t viewport_height,
		np::HostSessionPerf *perf);

} // namespace listen_host

} // namespace opennova::inmatch
