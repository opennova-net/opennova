#include "npruntime/server_tick.h"

#include <vector>

#include <netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <netsim/net_system.h>         // drain_connection_c2s / emit_connection_s2c
#include <world/world.h>               // World::run_logic_tick

namespace opennova::np {

void Server_TickUpdate(NapiNPServerCtx &ctx, const PlayerReplicationState &fallback_anchor) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame.
	// [orig: Server_TickUpdate gates on is_authority / is_in_session @ +0x58.]
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;

	// (1) net-before-logic: drain each in-match connection's queued C2S 0x0C and read-apply (SNAP).
	// burst.spawned marks an in-match connection — a mid-burst peer is still receiving its §5.2a
	// initial-state stream via tick_connections (which skips spawned peers, napi_np_protocol.cpp:509)
	// and has no per-frame C2S 0x0C uplink yet. [orig: PumpRecvQueues walks connection_list.]
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!conn.burst.spawned || conn.link.transport == nullptr) continue;
		netsim::drain_connection_c2s(world, *conn.link.transport);
	}

	// (2) one logic tick (the host is always authority here). WAC/BMS/AI advance the world.
	world.run_logic_tick(/*is_authority=*/true);

	// (3) serialize-after: build the world snapshot ONCE, then fan a per-connection-anchored 0x0A to
	// every in-match connection [orig: NapiNPServer_SendFiltered @0x4C87E0 once, SendToConn per node].
	const std::vector<GameEntitySnapshot> ents = netsim::snapshot_world(world);
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!conn.burst.spawned || conn.link.transport == nullptr) continue;
		netsim::emit_connection_s2c(world, conn.link, ents, fallback_anchor);
	}

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / NetClientView::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in libs/.
}

} // namespace opennova::np
