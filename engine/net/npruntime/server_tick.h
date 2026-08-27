#pragma once

#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h> // JO_ENGINE_TICK_RATE

#include <net/npwire/session_hello.h> // DisconnectEvent

#include <net/npruntime/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::np {

// Retail's maintenance clocks, expressed in original 62 Hz server ticks and
// exposed for protocol regression tests/capture tooling. Integrity shares one
// explicit global scoreboard counter: `++timer > 0x136`, reset zero => 311
// ticks. Its independently persistent family toggle starts with 0x31.
// [orig: Server_TickUpdate @0x51D7E0 -> @0x508540]
inline constexpr uint32_t INTEGRITY_REQUEST_PERIOD_TICKS = 0x136u + 1u;
// One explicit global countdown: reset 0 emits at the next boundary, reload
// 0x136 emits again after exactly 310 further Server_TickUpdate calls.
inline constexpr uint32_t NETWORK_QUALITY_BROADCAST_PERIOD_TICKS = 0x136u;
inline constexpr uint32_t CONTROL_REQUEST_LIVE_GATE_TICKS =
		30u * uint32_t(JO_ENGINE_TICK_RATE);
inline constexpr uint32_t CONTROL_REQUEST_PERIOD_TICKS =
		12u * uint32_t(JO_ENGINE_TICK_RATE);

// Optional attribution for one authoritative Server_TickUpdate. Callers pass
// nullptr outside an active diagnostics capture; the zero-initialized value is
// flattened so adapters can forward it without depending on World internals.
struct ServerTickPerf {
	uint64_t input_us = 0;
	uint64_t world_us = 0;
	uint64_t world_setup_us = 0;
	uint64_t world_scripts_us = 0;
	uint64_t world_ai_us = 0;
	uint64_t world_ai_reactions_us = 0;
	uint64_t world_ai_collision_tables_us = 0;
	uint64_t world_ai_entities_us = 0;
	uint64_t world_ai_infantry_entities_us = 0;
	uint64_t world_ai_infantry_remote_us = 0;
	uint64_t world_ai_infantry_combat_us = 0;
	uint64_t world_ai_infantry_animation_us = 0;
	uint64_t world_ai_infantry_collision_us = 0;
	uint64_t world_ai_infantry_collision_contacts_us = 0;
	uint64_t world_ai_infantry_collision_repulsion_us = 0;
	uint64_t world_ai_infantry_collision_ground_us = 0;
	uint64_t world_ai_other_entities_us = 0;
	uint64_t world_ai_authority_vehicles_us = 0;
	uint64_t world_ai_vehicle_scan_us = 0;
	uint64_t world_ai_vehicle_motors_us = 0;
	uint64_t world_ai_vehicle_riders_us = 0;
	uint64_t world_ai_client_vehicles_us = 0;
	uint64_t world_ai_events_us = 0;
	uint64_t world_attachments_us = 0;
	uint64_t world_attachment_orphans_us = 0;
	uint64_t world_attachment_child_pose_us = 0;
	uint64_t world_attachment_riders_us = 0;
	uint64_t world_throwables_us = 0;
	uint64_t world_weapons_us = 0;
	uint64_t world_projectiles_us = 0;
	uint64_t world_destruction_us = 0;
	uint64_t world_housekeeping_us = 0;
	uint64_t match_us = 0;
	uint64_t rules_us = 0;
	uint64_t replication_us = 0;
	uint64_t replication_query_prep_us = 0;
	uint64_t replication_query_collect_us = 0;
	uint64_t replication_query_grid_us = 0;
	uint64_t replication_query_grid_span_us = 0;
	uint64_t replication_query_grid_bucket_us = 0;
	uint64_t replication_query_grid_workspace_us = 0;
	uint64_t replication_snapshot_us = 0;
	uint64_t replication_fan_us = 0;
	uint64_t replication_fan_setup_us = 0;
	uint64_t replication_round_selection_us = 0;
	uint64_t replication_entity_selection_us = 0;
	uint64_t replication_entity_setup_us = 0;
	uint64_t replication_entity_scoring_us = 0;
	uint64_t replication_entity_los_us = 0;
	uint64_t replication_entity_los_terrain_us = 0;
	uint64_t replication_entity_los_sector_us = 0;
	uint64_t replication_entity_sort_us = 0;
	uint64_t replication_entity_budget_us = 0;
	uint64_t replication_encode_us = 0;
	uint64_t replication_enqueue_us = 0;

	// Sum another record into this one (a render frame consumes 0..N ticks;
	// the shell keeps one summed record per frame).
	ServerTickPerf &operator+=(const ServerTickPerf &o) {
		input_us += o.input_us;
		world_us += o.world_us;
		world_setup_us += o.world_setup_us;
		world_scripts_us += o.world_scripts_us;
		world_ai_us += o.world_ai_us;
		world_ai_reactions_us += o.world_ai_reactions_us;
		world_ai_collision_tables_us += o.world_ai_collision_tables_us;
		world_ai_entities_us += o.world_ai_entities_us;
		world_ai_infantry_entities_us += o.world_ai_infantry_entities_us;
		world_ai_infantry_remote_us += o.world_ai_infantry_remote_us;
		world_ai_infantry_combat_us += o.world_ai_infantry_combat_us;
		world_ai_infantry_animation_us += o.world_ai_infantry_animation_us;
		world_ai_infantry_collision_us += o.world_ai_infantry_collision_us;
		world_ai_infantry_collision_contacts_us += o.world_ai_infantry_collision_contacts_us;
		world_ai_infantry_collision_repulsion_us += o.world_ai_infantry_collision_repulsion_us;
		world_ai_infantry_collision_ground_us += o.world_ai_infantry_collision_ground_us;
		world_ai_other_entities_us += o.world_ai_other_entities_us;
		world_ai_authority_vehicles_us += o.world_ai_authority_vehicles_us;
		world_ai_vehicle_scan_us += o.world_ai_vehicle_scan_us;
		world_ai_vehicle_motors_us += o.world_ai_vehicle_motors_us;
		world_ai_vehicle_riders_us += o.world_ai_vehicle_riders_us;
		world_ai_client_vehicles_us += o.world_ai_client_vehicles_us;
		world_ai_events_us += o.world_ai_events_us;
		world_attachments_us += o.world_attachments_us;
		world_attachment_orphans_us += o.world_attachment_orphans_us;
		world_attachment_child_pose_us += o.world_attachment_child_pose_us;
		world_attachment_riders_us += o.world_attachment_riders_us;
		world_throwables_us += o.world_throwables_us;
		world_weapons_us += o.world_weapons_us;
		world_projectiles_us += o.world_projectiles_us;
		world_destruction_us += o.world_destruction_us;
		world_housekeeping_us += o.world_housekeeping_us;
		match_us += o.match_us;
		rules_us += o.rules_us;
		replication_us += o.replication_us;
		replication_query_prep_us += o.replication_query_prep_us;
		replication_query_collect_us += o.replication_query_collect_us;
		replication_query_grid_us += o.replication_query_grid_us;
		replication_query_grid_span_us += o.replication_query_grid_span_us;
		replication_query_grid_bucket_us += o.replication_query_grid_bucket_us;
		replication_query_grid_workspace_us += o.replication_query_grid_workspace_us;
		replication_snapshot_us += o.replication_snapshot_us;
		replication_fan_us += o.replication_fan_us;
		replication_fan_setup_us += o.replication_fan_setup_us;
		replication_round_selection_us += o.replication_round_selection_us;
		replication_entity_selection_us += o.replication_entity_selection_us;
		replication_entity_setup_us += o.replication_entity_setup_us;
		replication_entity_scoring_us += o.replication_entity_scoring_us;
		replication_entity_los_us += o.replication_entity_los_us;
		replication_entity_los_terrain_us += o.replication_entity_los_terrain_us;
		replication_entity_los_sector_us += o.replication_entity_los_sector_us;
		replication_entity_sort_us += o.replication_entity_sort_us;
		replication_entity_budget_us += o.replication_entity_budget_us;
		replication_encode_us += o.replication_encode_us;
		replication_enqueue_us += o.replication_enqueue_us;
		return *this;
	}
};

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
// IMPORTANT (P7 guardrail) [D-NET-125]: this IS the C2S drain AND it owns the logic tick. Do NOT also
// keep a separate run_logic_tick (nor a parallel connection-table driver) when driving the runtime
// through Server_TickUpdate, or the C2S queue drains — and the sim advances — twice. The drain/emit
// primitives (netsim::drain_connection_c2s / emit_connection_s2c, connection_fan.h) are invoked ONLY
// from here over connection_list; the legacy NetSystem-as-ISystem was retired at P8.
void Server_TickUpdate(NapiNPServerCtx &ctx, ServerTickPerf *perf = nullptr);

// Re-arm every connection's ONE-SHOT minimap initial scan (the pool-2
// non-spawn-point sweep emit_minimap_overlay_state runs once per client
// epoch, then leaves to the SpawnPoint refresh + the 14-tick pool-1 phase
// walk). A mission restart resets each client view to empty retained map
// banks, so the host calls this alongside the baseline restore — the next
// producer invocation then re-sends the full persistent building/zone
// marker set to every in-match connection, loopback and remote alike.
void Server_RearmMinimapInitialScan(NapiNPServerCtx &ctx);

} // namespace opennova::np
