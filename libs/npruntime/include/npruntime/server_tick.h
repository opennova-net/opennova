#pragma once

#include <npwire/replication_model.h> // PlayerReplicationState (opennova::)

#include "npruntime/napi_np_server_ctx.h" // NapiNPServerCtx

namespace opennova::np {

// The authoritative per-frame host loop [orig: Server_TickUpdate @0x51d7e0]. One call = one engine
// tick (the original 62 Hz cadence). Walks the SINGLE-OWNER connection table
// NapiNPProtocol.connection_list (each node's embedded netsim::Connection `link`) and, for every
// in-match connection (`burst.spawned`) that has a transport:
//   (1) drain its queued C2S 0x0C player uplinks and read-apply each (the SNAP)
//       [orig: PumpRecvQueues -> DispatchOpcode -> dispatch_entity_packet_callback @0x4D6A80].
//   (2) advance the simulation one logic tick (World::run_logic_tick: WAC/BMS/AI).
//   (3) fan ONE per-connection-anchored S2C 0x0A frame to each connection
//       [orig: NapiNPServer_SendFiltered @0x4C87E0 builds once -> SendToConn @0x4c4f20 per node].
//   (4) flush — implicit: host_send staged the body on each transport (loopback s2c_ / remote
//       outbound_, which the owner pops + frames into a 0x83 SESSION via frame_in_match_s2c).
//       Socket-free here (no UDP in libs/; the owner pumps bytes through the transport).
//
// `fallback_anchor` is the 0x0A subject for any connection with no owned entity yet (the host's own
// loopback) [D-NET-121: PlayerReplicationState default-constructs to the dvxi5 map-center coords, NOT
// origin — so the {} default is a NON-zero anchor; a spawned connection with no resolvable owned
// entity anchors its 0x0A there]. A joiner (is_authority == 0) and the pre-World P2 path
// (ctx.world == nullptr) no-op [orig: the host tick runs under is_authority @0x5266b4; is_in_session
// @+0x58 gates the replicate/broadcast at step (3), not the C2S drain or the whole tick].
//
// IMPORTANT (P7 guardrail) [D-NET-125]: this IS the C2S drain AND it owns the logic tick. Do NOT also
// keep a separate run_logic_tick (nor a parallel connection-table driver) when driving the runtime
// through Server_TickUpdate, or the C2S queue drains — and the sim advances — twice. The drain/emit
// primitives (netsim::drain_connection_c2s / emit_connection_s2c, connection_fan.h) are invoked ONLY
// from here over connection_list; the legacy NetSystem-as-ISystem was retired at P8.
void Server_TickUpdate(NapiNPServerCtx &ctx, const PlayerReplicationState &fallback_anchor = {});

} // namespace opennova::np
