#include "simulation/simulation.h"

namespace godot {

// The session phase attribution folded straight onto the frame-stats board
// (ADR 0039): one native add per slot at the end of a session frame, while the
// profiling clocks run and the board captures. The keys get_session_perf()
// still flattens for the probes are the same fields; the board no longer
// round-trips through that Dictionary.

void Simulation::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
}

Ref<FrameStats> Simulation::get_frame_stats() const {
	return frame_stats_;
}

void Simulation::fold_frame_stats(const opennova::inmatch::FrameOutcome &p_outcome) {
	if (!runtime_profiling_enabled_ || !frame_stats_.is_valid() || p_outcome.ticks_run() <= 0) {
		return;
	}
	FrameStats &stats = **frame_stats_;
	if (!stats.is_capture_active()) {
		return;
	}
	const SessionPhasePerf &phase = frame_phase_perf_;
	const opennova::np::HostSessionPerf &host_session = phase.host_session;
	const opennova::np::ServerTickPerf &server = host_session.server;
	const auto add = [&stats](FrameStats::Slot p_slot, int64_t p_us) {
		stats.add(p_slot, p_us);
	};
	add(FrameStats::SIM_STEP, frame_sim_us_);
	add(FrameStats::SIM_SINK, frame_sink_us_);
	add(FrameStats::SIM_NET, phase.client_decode_us);
	// The host pump and server tick rows exist only where that pump ran; a
	// joiner or a direct (no-net) tick fills the World update rows alone, so
	// the residuals under Authoritative server tick never subtract a world
	// the server tick never contained.
	const bool authority_pump = listen_server_ || host_listen_;
	if (authority_pump) {
		add(FrameStats::SIM_HOST_PREP, phase.host_prep_us);
		add(FrameStats::SIM_HOST_PUMP, static_cast<int64_t>(host_session.total_us));
		add(FrameStats::SIM_HOST_RECEIVE, static_cast<int64_t>(host_session.receive_us));
		add(FrameStats::SIM_HOST_CONNECTIONS, static_cast<int64_t>(host_session.connections_us));
		add(FrameStats::SIM_HOST_ADAPTER, static_cast<int64_t>(host_session.adapter_us));
		add(FrameStats::SIM_SERVER_TICK, static_cast<int64_t>(host_session.server_us));
		add(FrameStats::SIM_SERVER_INPUT, static_cast<int64_t>(server.input_us));
	}
	add(FrameStats::SIM_SERVER_WORLD, static_cast<int64_t>(server.world_us));
	add(FrameStats::SIM_WORLD_SETUP, static_cast<int64_t>(server.world_setup_us));
	add(FrameStats::SIM_WORLD_SCRIPTS, static_cast<int64_t>(server.world_scripts_us));
	add(FrameStats::SIM_WORLD_AI, static_cast<int64_t>(server.world_ai_us));
	add(FrameStats::SIM_AI_REACTIONS, static_cast<int64_t>(server.world_ai_reactions_us));
	add(FrameStats::SIM_AI_COLLISION, static_cast<int64_t>(server.world_ai_collision_tables_us));
	add(FrameStats::SIM_AI_ENTITIES, static_cast<int64_t>(server.world_ai_entities_us));
	add(FrameStats::SIM_AI_INFANTRY, static_cast<int64_t>(server.world_ai_infantry_entities_us));
	add(FrameStats::SIM_AI_INFANTRY_REMOTE, static_cast<int64_t>(server.world_ai_infantry_remote_us));
	add(FrameStats::SIM_AI_INFANTRY_COMBAT, static_cast<int64_t>(server.world_ai_infantry_combat_us));
	add(FrameStats::SIM_AI_INFANTRY_ANIMATION, static_cast<int64_t>(server.world_ai_infantry_animation_us));
	add(FrameStats::SIM_AI_INFANTRY_COLLISION, static_cast<int64_t>(server.world_ai_infantry_collision_us));
	add(FrameStats::SIM_AI_INFANTRY_COLLISION_CONTACTS,
			static_cast<int64_t>(server.world_ai_infantry_collision_contacts_us));
	add(FrameStats::SIM_AI_INFANTRY_COLLISION_REPULSION,
			static_cast<int64_t>(server.world_ai_infantry_collision_repulsion_us));
	add(FrameStats::SIM_AI_INFANTRY_COLLISION_GROUND,
			static_cast<int64_t>(server.world_ai_infantry_collision_ground_us));
	add(FrameStats::SIM_AI_OTHER_ENTITIES, static_cast<int64_t>(server.world_ai_other_entities_us));
	add(FrameStats::SIM_AI_AUTH_VEHICLES, static_cast<int64_t>(server.world_ai_authority_vehicles_us));
	add(FrameStats::SIM_AI_VEHICLE_SCAN, static_cast<int64_t>(server.world_ai_vehicle_scan_us));
	add(FrameStats::SIM_AI_VEHICLE_MOTORS, static_cast<int64_t>(server.world_ai_vehicle_motors_us));
	add(FrameStats::SIM_AI_VEHICLE_RIDERS, static_cast<int64_t>(server.world_ai_vehicle_riders_us));
	add(FrameStats::SIM_AI_CLIENT_VEHICLES, static_cast<int64_t>(server.world_ai_client_vehicles_us));
	add(FrameStats::SIM_AI_EVENTS, static_cast<int64_t>(server.world_ai_events_us));
	add(FrameStats::SIM_WORLD_ATTACHMENTS, static_cast<int64_t>(server.world_attachments_us));
	add(FrameStats::SIM_ATTACHMENT_ORPHANS, static_cast<int64_t>(server.world_attachment_orphans_us));
	add(FrameStats::SIM_ATTACHMENT_CHILDREN, static_cast<int64_t>(server.world_attachment_child_pose_us));
	add(FrameStats::SIM_ATTACHMENT_RIDERS, static_cast<int64_t>(server.world_attachment_riders_us));
	add(FrameStats::SIM_WORLD_THROWABLES, static_cast<int64_t>(server.world_throwables_us));
	add(FrameStats::SIM_WORLD_WEAPONS, static_cast<int64_t>(server.world_weapons_us));
	add(FrameStats::SIM_WORLD_PROJECTILES, static_cast<int64_t>(server.world_projectiles_us));
	add(FrameStats::SIM_WORLD_DESTRUCTION, static_cast<int64_t>(server.world_destruction_us));
	add(FrameStats::SIM_WORLD_HOUSEKEEPING, static_cast<int64_t>(server.world_housekeeping_us));
	if (!authority_pump) {
		add(FrameStats::SIM_CLIENT_SETUP, static_cast<int64_t>(phase.client.setup_us));
		add(FrameStats::SIM_CLIENT_RECEIVE, static_cast<int64_t>(phase.client.receive_us));
		add(FrameStats::SIM_CLIENT_MAINTENANCE, static_cast<int64_t>(phase.client.maintenance_us));
		add(FrameStats::SIM_CLIENT_SEND, static_cast<int64_t>(phase.client.send_us));
		add(FrameStats::SIM_CLIENT_MATERIALIZE, static_cast<int64_t>(phase.joiner.materialize_us));
		add(FrameStats::SIM_CLIENT_MIRROR, static_cast<int64_t>(phase.joiner.mirror_us));
		add(FrameStats::SIM_CLIENT_PROXIES, static_cast<int64_t>(phase.joiner.proxies_us));
		add(FrameStats::SIM_CLIENT_WORLD, static_cast<int64_t>(phase.joiner.world_us));
		add(FrameStats::SIM_CLIENT_ATTACH, static_cast<int64_t>(phase.joiner.attach_us));
		add(FrameStats::SIM_CLIENT_PLAYER, static_cast<int64_t>(phase.joiner.player_us));
		add(FrameStats::SIM_ADM_RESOLVE, phase.adm_resolve_us);
		return;
	}
	add(FrameStats::SIM_MATCH, static_cast<int64_t>(server.match_us));
	add(FrameStats::SIM_SERVER_RULES, static_cast<int64_t>(server.rules_us));
	add(FrameStats::SIM_SERVER_REPLICATION, static_cast<int64_t>(server.replication_us));
	add(FrameStats::SIM_REPLICATION_QUERY_PREP, static_cast<int64_t>(server.replication_query_prep_us));
	add(FrameStats::SIM_REPLICATION_QUERY_COLLECT, static_cast<int64_t>(server.replication_query_collect_us));
	add(FrameStats::SIM_REPLICATION_QUERY_GRID, static_cast<int64_t>(server.replication_query_grid_us));
	add(FrameStats::SIM_REPLICATION_QUERY_GRID_SPAN, static_cast<int64_t>(server.replication_query_grid_span_us));
	add(FrameStats::SIM_REPLICATION_QUERY_GRID_BUCKET,
			static_cast<int64_t>(server.replication_query_grid_bucket_us));
	add(FrameStats::SIM_REPLICATION_QUERY_GRID_WORKSPACE,
			static_cast<int64_t>(server.replication_query_grid_workspace_us));
	add(FrameStats::SIM_REPLICATION_SNAPSHOT, static_cast<int64_t>(server.replication_snapshot_us));
	add(FrameStats::SIM_REPLICATION_FAN, static_cast<int64_t>(server.replication_fan_us));
	add(FrameStats::SIM_REPLICATION_FAN_SETUP, static_cast<int64_t>(server.replication_fan_setup_us));
	add(FrameStats::SIM_REPLICATION_ROUNDS, static_cast<int64_t>(server.replication_round_selection_us));
	add(FrameStats::SIM_REPLICATION_ENTITIES, static_cast<int64_t>(server.replication_entity_selection_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_SETUP, static_cast<int64_t>(server.replication_entity_setup_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_SCORE, static_cast<int64_t>(server.replication_entity_scoring_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_LOS, static_cast<int64_t>(server.replication_entity_los_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_LOS_TERRAIN,
			static_cast<int64_t>(server.replication_entity_los_terrain_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_LOS_SECTOR,
			static_cast<int64_t>(server.replication_entity_los_sector_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_SORT, static_cast<int64_t>(server.replication_entity_sort_us));
	add(FrameStats::SIM_REPLICATION_ENTITY_BUDGET, static_cast<int64_t>(server.replication_entity_budget_us));
	add(FrameStats::SIM_REPLICATION_ENCODE, static_cast<int64_t>(server.replication_encode_us));
	add(FrameStats::SIM_REPLICATION_ENQUEUE, static_cast<int64_t>(server.replication_enqueue_us));
	add(FrameStats::SIM_HOST_SEND, static_cast<int64_t>(host_session.send_us));
	add(FrameStats::SIM_HOST_PLAYER, phase.host_player_us);
	add(FrameStats::SIM_CLIENT_SETUP, static_cast<int64_t>(phase.client.setup_us));
	add(FrameStats::SIM_CLIENT_RECEIVE, static_cast<int64_t>(phase.client.receive_us));
	add(FrameStats::SIM_CLIENT_MAINTENANCE, static_cast<int64_t>(phase.client.maintenance_us));
	add(FrameStats::SIM_CLIENT_SEND, static_cast<int64_t>(phase.client.send_us));
	add(FrameStats::SIM_CLIENT_MATERIALIZE, static_cast<int64_t>(phase.joiner.materialize_us));
	add(FrameStats::SIM_CLIENT_MIRROR, static_cast<int64_t>(phase.joiner.mirror_us));
	add(FrameStats::SIM_CLIENT_PROXIES, static_cast<int64_t>(phase.joiner.proxies_us));
	add(FrameStats::SIM_CLIENT_WORLD, static_cast<int64_t>(phase.joiner.world_us));
	add(FrameStats::SIM_CLIENT_ATTACH, static_cast<int64_t>(phase.joiner.attach_us));
	add(FrameStats::SIM_CLIENT_PLAYER, static_cast<int64_t>(phase.joiner.player_us));
	add(FrameStats::SIM_ADM_RESOLVE, phase.adm_resolve_us);
}

} // namespace godot
