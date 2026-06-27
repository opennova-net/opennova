#include "npruntime/server_tick.h"

#include <vector>

#include <netsim/entity_wire_bridge.h> // snapshot_world / GameEntitySnapshot
#include <netsim/net_system.h>         // drain_connection_c2s / emit_connection_s2c
#include <world/world.h>               // World::run_logic_tick

namespace opennova::np {

void Server_TickUpdate(NapiNPServerCtx &ctx, const PlayerReplicationState &fallback_anchor) {
	// A joiner is a pure non-authority client (its frame is P5's Client_ProcessNetworkFrame); the
	// pre-World P2 unit-test path has no simulation to drive. Either way: no host frame. The host
	// tick runs under is_authority [orig: Game_ProcessMainFrame @0x5263f0 gates the call
	// `if (is_authority && !suspended) Server_TickUpdate(...)` @0x5266b4]. is_in_session (+0x58) is
	// NOT the gate here — it gates the per-frame replicate/broadcast at step (3) [D-NET-120]; the
	// C2S drain + sim tick run regardless (the orig recv/send pumps are not is_in_session-gated).
	if (ctx.world == nullptr || ctx.is_authority == 0) return;
	world::World &world = *ctx.world;

	// (1) net-before-logic: drain each in-match connection's queued C2S 0x0C and read-apply (SNAP).
	// burst.spawned marks an in-match connection — a mid-burst peer is still receiving its §5.2a
	// initial-state stream via tick_connections (which skips spawned peers, napi_np_protocol.cpp:509)
	// and has no per-frame C2S 0x0C uplink yet. [orig: PumpRecvQueues walks connection_list.]
	// [D-NET-124] This fan assumes the spawned remote-peer (type-1) nodes stay resident in
	// connection_list; a mid-match configure_session_runtime() erases them (napi_np_protocol.cpp),
	// which would silently drop those peers from drain+replicate — revisit when reconfigure lands.
	// drain_connection_c2s null-checks conn.link.transport internally + enforces the owner gate.
	// [D-NET-122] is_in_match(conn) is the single predicate shared with the emit fan below (was an
	// inline burst.spawned in each); the host's own loopback satisfies it once its §5.2a burst
	// completes, so it is drained + emitted like any peer (its C2S is empty — is_authority suppresses
	// the host's own 0x0C — but its 0x0A local view is no longer starved).
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!is_in_match(conn)) continue;
		netsim::drain_connection_c2s(world, conn.link);
	}

	// (2) one logic tick (the host is always authority here). WAC/BMS/AI advance the world.
	// [D-NET-123] Server_TickUpdate OWNS this logic tick — the inverse of the legacy seam, where the
	// C2S drain ran INSIDE run_logic_tick (NetSystem::tick as a World ISystem). A P7 binding that
	// migrates to Server_TickUpdate must DROP its own run_logic_tick()/NetSystem registration or the
	// sim advances twice per frame (and the C2S queue drains twice — the P7 guardrail in the header).
	world.run_logic_tick(/*is_authority=*/true);

	// (3) serialize-after — SESSION-ONLY [D-NET-120]: the original's per-frame replicate/broadcast
	// blocks are each gated on is_in_session (+0x58) inside Server_TickUpdate (@0x51d9ab..0x51e3f3),
	// while the C2S recv pump above is not — so a World kept alive past match-end (is_in_session 0,
	// world non-null) keeps ticking but stops fanning ghost 0x0A frames. Build the world snapshot
	// ONCE, then fan a per-connection-anchored 0x0A to every in-match connection
	// [orig: NapiNPServer_SendFiltered @0x4C87E0 once, SendToConn per node].
	// [D-NET-122 RESOLVED at P5] This fan uses the shared is_in_match(conn) predicate (was an inline
	// burst.spawned). The host's own type-2 loopback now satisfies it once its §5.2a burst completes,
	// so Server_TickUpdate (the SP/host driver) fans it a per-frame 0x0A — its local view is no longer
	// starved (the gap the legacy NetSystem::emit_s2c filled by emitting to every transport-bearing
	// connection). Its 0x0A anchors to its owned_entity (the host player, bound by
	// Server_BuildPlayerInfoAndAdd), NOT the D-NET-121 dvxi5 fallback_anchor.
	if (ctx.is_in_session) {
		const std::vector<GameEntitySnapshot> ents = netsim::snapshot_world(world);
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (!is_in_match(conn)) continue;
			netsim::emit_connection_s2c(world, conn.link, ents, fallback_anchor);
		}
	}

	// (4) flush is implicit: host_send staged each 0x0A on its transport. The loopback's local client
	// reads it via client_recv / NetClientView::pump; a remote peer's transport outbound_ is popped +
	// framed into a 0x83 SESSION by the owner (frame_in_match_s2c). No socket I/O in libs/.
}

} // namespace opennova::np
