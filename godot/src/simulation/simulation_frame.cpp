// Simulation's first-class Godot adapter to the portable inmatch::Session.
// Lifecycle, input deposit, fixed cadence, catch-up, and cancellation stay in
// engine/net/inmatch. Godot supplies one synchronous typed tick sink so its
// presentation devices consume a tick before the next catch-up tick runs.
#include "simulation/simulation_internal.h"

#include <godot_cpp/classes/time.hpp>

using namespace sim_internal;

namespace {

Ref<MissionFrameOutcome> godot_outcome(
		const opennova::inmatch::FrameOutcome &p_native) {
	Ref<MissionFrameOutcome> out;
	out.instantiate();
	out->assign(p_native);
	return out;
}

} // namespace

opennova::inmatch::Role Simulation::configured_session_role() const {
	if (joiner_) return opennova::inmatch::Role::Joiner;
	if (host_listen_) {
		return host_serve_and_play_
				? opennova::inmatch::Role::ListenHost
				: opennova::inmatch::Role::DedicatedHost;
	}
	return opennova::inmatch::Role::SinglePlayer;
}

bool Simulation::begin_session_load() {
	using State = opennova::inmatch::State;
	const State state = session_.state();
	// A pre-connected joiner deliberately carries its live socket into load.
	// Every other prior session, including Failed, closes its concrete target
	// before a replacement world is installed.
	if (state != State::Unloaded && state != State::Connecting) {
		(void)session_.close();
	}
	if (session_.state() != State::Connecting) {
		const opennova::inmatch::TransitionResult role =
				session_.configure_role(configured_session_role());
		if (role.code != opennova::inmatch::TransitionCode::Applied &&
				role.code != opennova::inmatch::TransitionCode::NoOp) {
			return false;
		}
	}
	return session_.begin_load().applied();
}

void Simulation::complete_session_load() {
	if (session_.state() != opennova::inmatch::State::Loading) return;
	if (!session_.complete_load().applied()) return;
	// Direct/local simulations historically start paused. Live GameFramePipeline
	// resumes them after presentation setup; network roles must keep pumping.
	if (session_.role() == opennova::inmatch::Role::SinglePlayer) {
		(void)session_.pause();
	}
}

void Simulation::fail_session_load(const char *p_message) {
	world_installed_ = false;
	(void)session_.fail({opennova::inmatch::SessionErrorCode::LoadFailed,
			p_message != nullptr ? p_message : "mission load failed"});
}

bool Simulation::is_loaded() const {
	if (!world_installed_) return false;
	const opennova::inmatch::State state = session_.state();
	return state == opennova::inmatch::State::Running ||
			state == opennova::inmatch::State::Paused;
}

bool Simulation::pause_session() {
	const opennova::inmatch::TransitionResult out = session_.pause();
	return out.applied() || out.code == opennova::inmatch::TransitionCode::NoOp;
}

bool Simulation::resume_session() {
	const opennova::inmatch::TransitionResult out = session_.resume();
	return out.applied() || out.code == opennova::inmatch::TransitionCode::NoOp;
}

bool Simulation::reset_session() {
	const opennova::inmatch::TransitionResult out = session_.reset_to_baseline();
	return out.applied();
}

void Simulation::close_session() {
	(void)session_.close();
}

void Simulation::close_mission() {
	leave_net_session();
}

bool Simulation::reset_mission_to_baseline(
		opennova::inmatch::SessionError &r_error) {
	if (!world_installed_ || !have_baseline_) {
		r_error = {opennova::inmatch::SessionErrorCode::TickFailed,
				"mission baseline is unavailable"};
		return false;
	}
	restore_world_baseline();
	return true;
}

opennova::inmatch::TickOutcome Simulation::advance_mission_tick(
		const opennova::inmatch::TickInput &p_input) {
	const opennova::world::PlayerInput &movement = p_input.player.movement;
	set_player_input(movement.forward, movement.back, movement.left,
			movement.right, movement.lean_left, movement.lean_right,
			movement.jump);
	if (p_input.player.look_delta_x != 0.0f ||
			p_input.player.look_delta_y != 0.0f) {
		add_local_player_look(p_input.player.look_delta_x,
				p_input.player.look_delta_y);
	}
	set_local_player_weapon_input(
			(p_input.player.held_action_bits & MissionFrameInput::HELD_FIRE) != 0,
			(p_input.player.pressed_action_bits &
					MissionFrameInput::PRESSED_FIRE) != 0,
			(p_input.player.pressed_action_bits &
					MissionFrameInput::PRESSED_RELOAD) != 0);
	// The medic-call edge is an action binding, not weapon state: it fires
	// its request immediately like retail's binding dispatch (the gates and
	// cooldown live in request_local_player_medic).
	if ((p_input.player.pressed_action_bits &
				MissionFrameInput::PRESSED_MEDIC_REQUEST) != 0) {
		request_local_player_medic();
	}

	const bool profiling = runtime_profiling_enabled_;
	const int64_t sim_start =
			profiling ? Time::get_singleton()->get_ticks_usec() : 0;
	const bool did_tick = advance_world_tick();
	if (profiling) {
		frame_sim_us_ += Time::get_singleton()->get_ticks_usec() - sim_start;
		frame_net_us_ += static_cast<int64_t>(get_last_net_tick_us());
	}
	if (!did_tick) return {};
	// The dead-player map-mode clear rides every advanced tick — retail's
	// render-frame gate, observed before the presenters read the mode.
	tick_hud_map_death_gate();
	if (is_session_lost()) {
		return {opennova::inmatch::TickStatus::SessionLost,
				static_cast<int32_t>(get_logic_tick()),
				{opennova::inmatch::SessionErrorCode::SessionLost,
						std::string(get_session_loss_reason().utf8().get_data())}};
	}

	opennova::inmatch::TickOutcome tick;
	tick.status = opennova::inmatch::TickStatus::Ran;
	tick.logic_tick = static_cast<int32_t>(get_logic_tick());
	if (session_tick_sink_.is_valid()) {
		Ref<MissionTickOutcome> value;
		value.instantiate();
		value->assign(tick);
		const int64_t sink_start =
				profiling ? Time::get_singleton()->get_ticks_usec() : 0;
		const Variant accepted = session_tick_sink_.call(value);
		if (profiling)
			frame_sink_us_ += Time::get_singleton()->get_ticks_usec() - sink_start;
		if (accepted.get_type() == Variant::BOOL && !static_cast<bool>(accepted)) {
			tick.status = opennova::inmatch::TickStatus::SessionLost;
			tick.error = {opennova::inmatch::SessionErrorCode::SessionLost,
					"Godot frame pipeline cancelled the tick batch"};
		}
	}
	return tick;
}

Ref<MissionFrameOutcome> Simulation::advance_session_frame(
		const Ref<MissionFrameInput> &p_input,
		const Callable &p_tick_sink) {
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	frame_phase_perf_ = {};
	opennova::inmatch::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	session_tick_sink_ = p_tick_sink;
	const opennova::inmatch::FrameOutcome outcome = session_.advance(input);
	session_tick_sink_ = Callable();
	fold_frame_stats(outcome);
	return godot_outcome(outcome);
}

Ref<MissionFrameOutcome> Simulation::step_session_frame(
		const Ref<MissionFrameInput> &p_input,
		const Callable &p_tick_sink) {
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	frame_phase_perf_ = {};
	opennova::inmatch::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	session_tick_sink_ = p_tick_sink;
	const opennova::inmatch::FrameOutcome outcome = session_.step_once(input);
	session_tick_sink_ = Callable();
	fold_frame_stats(outcome);
	return godot_outcome(outcome);
}

bool Simulation::step() {
	// Focused probes deposit input directly on Simulation before asking for one
	// deterministic tick. Preserve that public seam without adding a second tick
	// path: snapshot the concrete target's held/edge latches into the same typed
	// frame value inmatch::Session consumes. Direct look input has already updated
	// player_input_'s composed heading/pitch, so it must not be replayed as a
	// second pixel delta here.
	opennova::inmatch::FrameInput input;
	input.player.movement = player_input_;
	input.player.held_action_bits = local_weapon_.fire_held
			? MissionFrameInput::HELD_FIRE : 0u;
	input.player.pressed_action_bits =
			(local_weapon_.fire_pressed ? MissionFrameInput::PRESSED_FIRE : 0u) |
			(local_weapon_.reload_pressed ? MissionFrameInput::PRESSED_RELOAD : 0u);
	return session_.drive_one(input).ticks_run() == 1;
}

Dictionary Simulation::get_session_perf() const {
	const opennova::inmatch::FramePerf &perf = session_.last_perf();
	Dictionary out;
	out["frame_us"] = perf.frame_us;
	out["tick_us"] = perf.tick_us;
	out["ticks"] = perf.ticks;
	if (!runtime_profiling_enabled_)
		return out;
	// The phase attribution (frame_phase_perf_) exists only while the native
	// clocks run; the keys are the Stats board's vocabulary (the board itself
	// receives them through fold_frame_stats; this Dictionary is the probes').
	const SessionPhasePerf &phase = frame_phase_perf_;
	const opennova::np::HostSessionPerf &host_session = phase.host_session;
	const opennova::np::ServerTickPerf &server = host_session.server;
	out["sim_us"] = frame_sim_us_;
	out["sink_us"] = frame_sink_us_;
	out["net_us"] = frame_net_us_;
	out["host_prep_us"] = phase.host_prep_us;
	out["host_pump_us"] = static_cast<int64_t>(host_session.total_us);
	out["host_receive_us"] = static_cast<int64_t>(host_session.receive_us);
	out["host_connections_us"] = static_cast<int64_t>(host_session.connections_us);
	out["host_adapter_us"] = static_cast<int64_t>(host_session.adapter_us);
	out["server_tick_us"] = static_cast<int64_t>(host_session.server_us);
	out["server_input_us"] = static_cast<int64_t>(server.input_us);
	out["server_world_us"] = static_cast<int64_t>(server.world_us);
	out["world_setup_us"] = static_cast<int64_t>(server.world_setup_us);
	out["world_scripts_us"] = static_cast<int64_t>(server.world_scripts_us);
	out["world_ai_us"] = static_cast<int64_t>(server.world_ai_us);
	out["world_ai_reactions_us"] = static_cast<int64_t>(server.world_ai_reactions_us);
	out["world_ai_collision_tables_us"] = static_cast<int64_t>(server.world_ai_collision_tables_us);
	out["world_ai_entities_us"] = static_cast<int64_t>(server.world_ai_entities_us);
	out["world_ai_infantry_entities_us"] = static_cast<int64_t>(server.world_ai_infantry_entities_us);
	out["world_ai_infantry_remote_us"] = static_cast<int64_t>(server.world_ai_infantry_remote_us);
	out["world_ai_infantry_combat_us"] = static_cast<int64_t>(server.world_ai_infantry_combat_us);
	out["world_ai_infantry_animation_us"] = static_cast<int64_t>(server.world_ai_infantry_animation_us);
	out["world_ai_infantry_collision_us"] = static_cast<int64_t>(server.world_ai_infantry_collision_us);
	out["world_ai_infantry_collision_contacts_us"] = static_cast<int64_t>(server.world_ai_infantry_collision_contacts_us);
	out["world_ai_infantry_collision_repulsion_us"] = static_cast<int64_t>(server.world_ai_infantry_collision_repulsion_us);
	out["world_ai_infantry_collision_ground_us"] = static_cast<int64_t>(server.world_ai_infantry_collision_ground_us);
	out["world_ai_other_entities_us"] = static_cast<int64_t>(server.world_ai_other_entities_us);
	out["world_ai_authority_vehicles_us"] = static_cast<int64_t>(server.world_ai_authority_vehicles_us);
	out["world_ai_vehicle_scan_us"] = static_cast<int64_t>(server.world_ai_vehicle_scan_us);
	out["world_ai_vehicle_motors_us"] = static_cast<int64_t>(server.world_ai_vehicle_motors_us);
	out["world_ai_vehicle_riders_us"] = static_cast<int64_t>(server.world_ai_vehicle_riders_us);
	out["world_ai_client_vehicles_us"] = static_cast<int64_t>(server.world_ai_client_vehicles_us);
	out["world_ai_events_us"] = static_cast<int64_t>(server.world_ai_events_us);
	out["world_attachments_us"] = static_cast<int64_t>(server.world_attachments_us);
	out["world_attachment_orphans_us"] = static_cast<int64_t>(server.world_attachment_orphans_us);
	out["world_attachment_child_pose_us"] = static_cast<int64_t>(server.world_attachment_child_pose_us);
	out["world_attachment_riders_us"] = static_cast<int64_t>(server.world_attachment_riders_us);
	out["world_throwables_us"] = static_cast<int64_t>(server.world_throwables_us);
	out["world_weapons_us"] = static_cast<int64_t>(server.world_weapons_us);
	out["world_projectiles_us"] = static_cast<int64_t>(server.world_projectiles_us);
	out["world_destruction_us"] = static_cast<int64_t>(server.world_destruction_us);
	out["world_housekeeping_us"] = static_cast<int64_t>(server.world_housekeeping_us);
	out["match_us"] = static_cast<int64_t>(server.match_us);
	out["replication_query_prep_us"] = static_cast<int64_t>(server.replication_query_prep_us);
	out["replication_query_collect_us"] = static_cast<int64_t>(server.replication_query_collect_us);
	out["replication_query_grid_us"] = static_cast<int64_t>(server.replication_query_grid_us);
	out["replication_query_grid_span_us"] = static_cast<int64_t>(server.replication_query_grid_span_us);
	out["replication_query_grid_bucket_us"] = static_cast<int64_t>(server.replication_query_grid_bucket_us);
	out["replication_query_grid_workspace_us"] = static_cast<int64_t>(server.replication_query_grid_workspace_us);
	out["replication_snapshot_us"] = static_cast<int64_t>(server.replication_snapshot_us);
	out["replication_fan_us"] = static_cast<int64_t>(server.replication_fan_us);
	out["replication_fan_setup_us"] = static_cast<int64_t>(server.replication_fan_setup_us);
	out["replication_round_selection_us"] = static_cast<int64_t>(server.replication_round_selection_us);
	out["replication_entity_selection_us"] = static_cast<int64_t>(server.replication_entity_selection_us);
	out["replication_entity_setup_us"] = static_cast<int64_t>(server.replication_entity_setup_us);
	out["replication_entity_scoring_us"] = static_cast<int64_t>(server.replication_entity_scoring_us);
	out["replication_entity_los_us"] = static_cast<int64_t>(server.replication_entity_los_us);
	out["replication_entity_los_terrain_us"] = static_cast<int64_t>(server.replication_entity_los_terrain_us);
	out["replication_entity_los_sector_us"] = static_cast<int64_t>(server.replication_entity_los_sector_us);
	out["replication_entity_sort_us"] = static_cast<int64_t>(server.replication_entity_sort_us);
	out["replication_entity_budget_us"] = static_cast<int64_t>(server.replication_entity_budget_us);
	out["replication_encode_us"] = static_cast<int64_t>(server.replication_encode_us);
	out["replication_enqueue_us"] = static_cast<int64_t>(server.replication_enqueue_us);
	out["server_rules_us"] = static_cast<int64_t>(server.rules_us);
	out["server_replication_us"] = static_cast<int64_t>(server.replication_us);
	out["host_send_us"] = static_cast<int64_t>(host_session.send_us);
	out["host_player_us"] = phase.host_player_us;
	out["client_decode_us"] = phase.client_decode_us;
	out["client_setup_us"] = static_cast<int64_t>(phase.client.setup_us);
	out["client_receive_us"] = static_cast<int64_t>(phase.client.receive_us);
	out["client_maintenance_us"] = static_cast<int64_t>(phase.client.maintenance_us);
	out["client_send_us"] = static_cast<int64_t>(phase.client.send_us);
	out["adm_resolve_us"] = phase.adm_resolve_us;
	return out;
}
