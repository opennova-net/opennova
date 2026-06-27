#pragma once

#include <novaworld/replication_min.h> // PlayerReplicationState (opennova::)

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
// loopback). A joiner (is_authority == 0) and the pre-World P2 path (ctx.world == nullptr) no-op
// [orig: Server_TickUpdate gates on is_authority / is_in_session @ +0x58].
//
// IMPORTANT (P7 guardrail): this IS the C2S drain. Do NOT also register a netsim::NetSystem as a
// World ISystem when driving the runtime through Server_TickUpdate, or the C2S queue drains twice.
void Server_TickUpdate(NapiNPServerCtx &ctx, const PlayerReplicationState &fallback_anchor = {});

} // namespace opennova::np
