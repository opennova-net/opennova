#pragma once

#include <cstdint>
#include <vector>

#include <base/io/tick_rate.h>

#include <net/npwire/session_hello.h> // DisconnectEvent

#include <runtime/inmatch/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::inmatch {

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
// transaction so the connection field and wire value cannot diverge. The arm
// stamps the server frame clock (world.out.rounds.frame_clock), which the
// death's 620-frame recent-spawn test reads.
// [orig: Server_SendRandomSeedToPlayer @0x5101A0, enable==1 arm: the clock
//  @0x5101A9, +0x178C8 @0x5101B3, +0x178CC = 0 @0x5101B9]
std::vector<uint8_t> Server_RerollPlayerTickSeed(
		NapiNPConnection &connection, uint32_t frame_clock);

// The disarm twin: zero the retained anchor, stamp the server frame clock as
// the fire grace's start and return the four-zero 0x61 body. Death and
// round-end call this arm; the zero seed freezes the client's network-role
// tick until the next re-arm.
// [orig: Server_SendRandomSeedToPlayer @0x5101A0, enable==0 arm: the clock
//  @0x51022A, +0x178CC @0x510237]
std::vector<uint8_t> Server_DisarmPlayerTickSeed(
		NapiNPConnection &connection, uint32_t frame_clock);

// The round end, synchronous at its latch: Match's outcome and frozen board,
// then (in an MP session) every in-match slot's 0x61 / 0x1D, game state 11 and
// slot state 7, the round tallies and the linger, before the caller's next
// statement, so a later sweep in the same second walks nobody. A no-op once
// the round is over. The periodic win check and the admin round ends call it;
// a WAC / BMS action latches inside the world's script pass and Server_TickUpdate
// announces it behind that pass's routed records.
// [orig: Server_ProcessRoundEnd @0x5164F0 - the round-over gate @0x5164F6, slot
//  state 7 @0x51685E; called inline by Server_CheckWinConditions @0x51AD77,
//  CAdminServer_HandleGotoCommand @0x404A95, CAdminServer_HandleMissionCommand
//  @0x40660B, CNapiGameSession_HandleServerCommand @0x4D31C0]
void Server_ProcessRoundEnd(NapiNPServerCtx &ctx, world::World &world,
		int32_t winning_team);

// The disarmed grace reads the server frame clock against the disarm's stamp.
// [orig: PlayerSlot_IsActive @0x4FC760 (`cmp g_ServerFrameClock` @0x4FC78E)]
bool Server_AcceptsPlayerFireTick(const NapiNPConnection &connection,
		uint32_t client_tick, uint32_t frame_clock, uint32_t send_holdoff_ticks);

// The previous entity update's records, routed onto the connections (server_tick.cpp): the
// placed devices, the deaths and their staged sends, the medic interactions, the powerup
// grants, the overlay updates, the gameplay events and the water crossings. Server_TickUpdate
// runs it at its head; a session owner runs it first at the head of its pump, so the update's
// sends lead what the tick's walk and receive pump queue (D-NET-NEW-L1-6); the receive pump
// runs it behind each message's handler, for what that handler raised (D-NET-NEW-V3-1), and
// once more at its end, for what the connection producers raised (D-NET-686).
// [orig: Game_ProcessMainFrame - the Entity_UpdateAllEntities call @0x52674B after
//  Server_TickUpdate's send pump @0x51E487]
void route_entity_pass_records(NapiNPServerCtx &ctx, world::World &world);

// The authoritative per-frame host loop [orig: Server_TickUpdate @0x51d7e0]. One call = one engine
// tick (the original 62 Hz cadence). Walks the SINGLE-OWNER connection table
// NapiNPProtocol.connection_list (each node's embedded replication::Connection `link`) and, for every
// in-match connection (`burst.spawned`) that has a transport:
//   (1) drain its queued C2S 0x0C player uplinks and read-apply each (the SNAP)
//       [orig: PumpRecvQueues -> DispatchOpcode -> NetPacket_DispatchEntityPacketCallback @0x4D6A80].
//   (2) run the world's script pass (World::run_script_pass: the WAC tick, the every-32
//       legs, the BMS quarter pass), then this tick's own maintenance legs.
//   (3) fan ONE per-connection-anchored S2C 0x0A frame to each connection
//       [orig: NapiNPServer_SendFiltered @0x4C87E0 builds once -> SendToConn @0x4c4f20 per node].
//   (4) run the world's entity pass (World::run_entity_pass: the gated entity update, the
//       weapon pump, the tail), whose records lead the next call's queue [orig:
//       Game_ProcessMainFrame's Entity_UpdateAllEntities call @0x52674B follows its
//       Server_TickUpdate call @0x5266B6].
//   (5) flush — implicit: host_send staged the body on each transport (loopback s2c_ / remote
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
// offline play runs the local role's tick. The drain/emit primitives are invoked only here over
// connection_list; the parallel NetSystem-as-ISystem owner was removed at P8.
void Server_TickUpdate(NapiNPServerCtx &ctx);

// Recompute every in-match player's kit weight from its live rows: the listen
// host's own player from its local inventory, a remote player from its
// granted combos + live clips over the authority pool table. The last
// statement of the periodic-second block and the tail of every accepted
// LOADOUT_SUBMIT. [orig: Server_RecalculateAllPlayerScores @0x5014E0; callers
// Server_TickUpdate @0x51e1ab, NapiNPServerMsg_HandlePlayerLoadout @0x515f9d]
void Server_RecalculateAllPlayerKitWeights(
		std::vector<NapiNPConnection> &roster, world::World &world);

// The join-phase validation watchdog (NetPlayer states 3/4): the NovaWorld
// ClientPlayerEnterRequest announcement on an armed host, the 120 s
// NONWTOVALU / NWJTICKTMOUT reaps. Runs from the periodic block; exposed for
// the shell-driven ticket flow and its tests.
// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0, caller @0x51DBF3]
void Server_CheckPlayerTimeouts(NapiNPServerCtx &ctx);
// The service's ServerPlayerEnterResult for a held joiner (see
// NapiNPServerCtx::on_player_enter_request). Success admits it into the spawn
// pump; a failure punts it with "NWU:NWPENTERFAIL" carrying MsgCode.
// [orig: CNapiGameSession_HandlePlayEnterResponse @0x4D1940]
bool Server_ApplyPlayerEnterResult(NapiNPServerCtx &ctx, uint32_t connection_id,
		bool success, int32_t msg_code);

// Re-arm every connection's ONE-SHOT minimap initial scan (the pool-2
// non-spawn-point sweep emit_minimap_overlay_for_slot runs once per client
// epoch, then leaves to the SpawnPoint refresh + the 14-tick pool-1 phase
// walk). A mission restart resets each client view to empty retained map
// banks, so the host calls this alongside the baseline restore — the next
// producer invocation then re-sends the full persistent building/zone
// marker set to every in-match connection, loopback and remote alike.
void Server_RearmMinimapInitialScan(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
