#pragma once

#include <cstdint>
#include <vector>

#include <base/io/tick_rate.h>

#include <net/npwire/session_hello.h> // DisconnectEvent

#include <net/npruntime/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::np {

// One explicit global countdown: reset 0 emits at the next boundary, reload
// 0x136 emits again after exactly 310 further Server_TickUpdate calls.
inline constexpr uint32_t NETWORK_QUALITY_BROADCAST_PERIOD_TICKS = 0x136u;
inline constexpr uint32_t CONTROL_REQUEST_LIVE_GATE_TICKS =
		30u * uint32_t(io::kTicksPerSecondInt);
inline constexpr uint32_t CONTROL_REQUEST_PERIOD_TICKS =
		12u * uint32_t(io::kTicksPerSecondInt);

// Stage retail's high-table H:0x03 LogPuntEvent record for one remote. The
// first event wins and immediately closes that connection's gameplay gate;
// HostOwner still flushes the reliable description on its next open boundary.
bool Server_StageHostDisconnect(
		NapiNPConnection &connection, const DisconnectEvent &event);
bool Server_StageHostPunt(
		NapiNPConnection &connection, uint32_t mismatch_type);

// Re-roll one player's retained fire-freshness/tick anchor and return its
// exact 4-byte S2C 0x61 body. Join, revive, and deployment call this one
// transaction so the connection field and wire value cannot diverge.
// [orig: Server_SendRandomSeedToPlayer @0x5101A0, enable==1 arm]
std::vector<uint8_t> Server_RerollPlayerTickSeed(
		NapiNPConnection &connection);

// The disarm twin: zero the retained anchor and return the four-zero 0x61
// body. Death and round-end call this arm; the zero seed freezes the
// client's network-role tick until the next re-arm.
// [orig: Server_SendRandomSeedToPlayer @0x5101A0, enable==0 arm @0x510237]
std::vector<uint8_t> Server_DisarmPlayerTickSeed(
		NapiNPConnection &connection);

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
//       Socket-free here (no UDP in engine/; the owner pumps bytes through the transport).
//
// A connection without its live owned player emits no 0x0A. Retail's writer is
// state==6 gated and reads the recipient entity before it increments the frame
// phase [orig: Server_SendEntityStateToPlayer @0x517BA0,
// @0x517BF5..0x517C13, @0x517BE8]. A joiner (is_authority == 0) and the pre-World P2 path
// (ctx.world == nullptr) no-op [orig: the host tick runs under is_authority @0x5266b4; is_in_session
// @+0x58 gates the replicate/broadcast at step (3), not the C2S drain or the whole tick].
//
// OWNERSHIP INVARIANT: this IS the C2S drain AND it owns the logic tick. Production role routing is
// mutually exclusive: listen/dedicated hosts call this path, joiners call the client path, and
// offline play calls tick_no_net. The drain/emit primitives are invoked only here over
// connection_list; the parallel NetSystem-as-ISystem owner was removed at P8.
void Server_TickUpdate(NapiNPServerCtx &ctx);

// Re-arm every connection's ONE-SHOT minimap initial scan (the pool-2
// non-spawn-point sweep emit_minimap_overlay_state runs once per client
// epoch, then leaves to the SpawnPoint refresh + the 14-tick pool-1 phase
// walk). A mission restart resets each client view to empty retained map
// banks, so the host calls this alongside the baseline restore — the next
// producer invocation then re-sends the full persistent building/zone
// marker set to every in-match connection, loopback and remote alike.
void Server_RearmMinimapInitialScan(NapiNPServerCtx &ctx);

} // namespace opennova::np
