// Simulation's first-class Godot adapter to the portable MissionSession.
// Lifecycle, input deposit, fixed cadence, catch-up, and cancellation stay in
// engine/net/npruntime. Godot supplies one synchronous typed tick sink so its
// presentation devices consume a tick before the next catch-up tick runs.
#include "simulation/nova_simulation_internal.h"

#include <godot_cpp/classes/time.hpp>

using namespace novasim;

namespace {

Ref<MissionFrameOutcome> godot_outcome(
		const opennova::np::FrameOutcome &p_native) {
	Ref<MissionFrameOutcome> out;
	out.instantiate();
	out->assign(p_native);
	return out;
}

} // namespace

opennova::np::MissionSessionRole Simulation::configured_session_role() const {
	if (joiner_) return opennova::np::MissionSessionRole::Joiner;
	if (host_listen_) {
		return host_serve_and_play_
				? opennova::np::MissionSessionRole::ListenHost
				: opennova::np::MissionSessionRole::DedicatedHost;
	}
	return opennova::np::MissionSessionRole::SinglePlayer;
}

bool Simulation::begin_session_load() {
	using State = opennova::np::MissionSessionState;
	const State state = mission_session_.state();
	// A pre-connected joiner deliberately carries its live socket into load.
	// Every other prior session, including Failed, closes its concrete target
	// before a replacement world is installed.
	if (state != State::Unloaded && state != State::Connecting) {
		(void)mission_session_.close();
	}
	if (mission_session_.state() != State::Connecting) {
		const opennova::np::TransitionResult role =
				mission_session_.configure_role(configured_session_role());
		if (role.code != opennova::np::TransitionCode::Applied &&
				role.code != opennova::np::TransitionCode::NoOp) {
			return false;
		}
	}
	return mission_session_.begin_load().applied();
}

void Simulation::complete_session_load() {
	if (mission_session_.state() !=
			opennova::np::MissionSessionState::Loading) return;
	if (!mission_session_.complete_load().applied()) return;
	// Direct/local simulations historically start paused. Live GameFramePipeline
	// resumes them after presentation setup; network roles must keep pumping.
	if (mission_session_.role() ==
			opennova::np::MissionSessionRole::SinglePlayer) {
		(void)mission_session_.pause();
	}
}

void Simulation::fail_session_load(const char *p_message) {
	world_installed_ = false;
	(void)mission_session_.fail({opennova::np::SessionErrorCode::LoadFailed,
			p_message != nullptr ? p_message : "mission load failed"});
}

bool Simulation::is_loaded() const {
	if (!world_installed_) return false;
	const opennova::np::MissionSessionState state = mission_session_.state();
	return state == opennova::np::MissionSessionState::Running ||
			state == opennova::np::MissionSessionState::Paused;
}

bool Simulation::pause_session() {
	const opennova::np::TransitionResult out = mission_session_.pause();
	return out.applied() || out.code == opennova::np::TransitionCode::NoOp;
}

bool Simulation::resume_session() {
	const opennova::np::TransitionResult out = mission_session_.resume();
	return out.applied() || out.code == opennova::np::TransitionCode::NoOp;
}

bool Simulation::reset_session() {
	const opennova::np::TransitionResult out =
			mission_session_.reset_to_baseline();
	return out.applied();
}

void Simulation::fail_session(const String &p_reason) {
	const CharString reason = p_reason.utf8();
	(void)mission_session_.fail({opennova::np::SessionErrorCode::SessionLost,
			std::string(reason.get_data(), static_cast<size_t>(reason.length()))});
}

void Simulation::close_session() {
	(void)mission_session_.close();
}

void Simulation::close_mission() {
	leave_net_session();
}

bool Simulation::reset_mission_to_baseline(
		opennova::np::SessionError &r_error) {
	if (!world_installed_ || !have_baseline_) {
		r_error = {opennova::np::SessionErrorCode::TickFailed,
				"mission baseline is unavailable"};
		return false;
	}
	restore_world_baseline();
	return true;
}

opennova::np::TickOutcome Simulation::advance_mission_tick(
		const opennova::np::TickInput &p_input) {
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

	const int64_t sim_start = Time::get_singleton()->get_ticks_usec();
	const bool did_tick = advance_world_tick();
	frame_sim_us_ += Time::get_singleton()->get_ticks_usec() - sim_start;
	frame_net_us_ += static_cast<int64_t>(get_last_net_tick_us());
	if (!did_tick) return {};
	// The dead-player map-mode clear rides every advanced tick — retail's
	// render-frame gate, observed before the presenters read the mode.
	tick_hud_map_death_gate();
	if (is_session_lost()) {
		return {opennova::np::TickStatus::SessionLost,
				static_cast<int32_t>(get_logic_tick()),
				{opennova::np::SessionErrorCode::SessionLost,
						std::string(get_session_loss_reason().utf8().get_data())}};
	}

	opennova::np::TickOutcome tick;
	tick.status = opennova::np::TickStatus::Ran;
	tick.logic_tick = static_cast<int32_t>(get_logic_tick());
	if (session_tick_sink_.is_valid()) {
		Ref<MissionTickOutcome> value;
		value.instantiate();
		value->assign(tick);
		const int64_t sink_start = Time::get_singleton()->get_ticks_usec();
		const Variant accepted = session_tick_sink_.call(value);
		frame_sink_us_ += Time::get_singleton()->get_ticks_usec() - sink_start;
		if (accepted.get_type() == Variant::BOOL && !static_cast<bool>(accepted)) {
			tick.status = opennova::np::TickStatus::SessionLost;
			tick.error = {opennova::np::SessionErrorCode::SessionLost,
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
	opennova::np::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	session_tick_sink_ = p_tick_sink;
	const opennova::np::FrameOutcome outcome = mission_session_.advance(input);
	session_tick_sink_ = Callable();
	return godot_outcome(outcome);
}

Ref<MissionFrameOutcome> Simulation::step_session_frame(
		const Ref<MissionFrameInput> &p_input,
		const Callable &p_tick_sink) {
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	opennova::np::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	session_tick_sink_ = p_tick_sink;
	const opennova::np::FrameOutcome outcome = mission_session_.step_once(input);
	session_tick_sink_ = Callable();
	return godot_outcome(outcome);
}

bool Simulation::step() {
	// Focused probes deposit input directly on Simulation before asking for one
	// deterministic tick. Preserve that public seam without adding a second tick
	// path: snapshot the concrete target's held/edge latches into the same typed
	// frame value MissionSession consumes. Direct look input has already updated
	// player_input_'s composed heading/pitch, so it must not be replayed as a
	// second pixel delta here.
	opennova::np::FrameInput input;
	input.player.movement = player_input_;
	input.player.held_action_bits = local_weapon_.fire_held
			? MissionFrameInput::HELD_FIRE : 0u;
	input.player.pressed_action_bits =
			(local_weapon_.fire_pressed ? MissionFrameInput::PRESSED_FIRE : 0u) |
			(local_weapon_.reload_pressed ? MissionFrameInput::PRESSED_RELOAD : 0u);
	return mission_session_.drive_one(input).ticks_run() == 1;
}

Dictionary Simulation::get_session_perf() const {
	const opennova::np::FramePerf &perf = mission_session_.last_perf();
	Dictionary out;
	out["frame_us"] = perf.frame_us;
	out["tick_us"] = perf.tick_us;
	out["sim_us"] = frame_sim_us_;
	out["sink_us"] = frame_sink_us_;
	out["net_us"] = frame_net_us_;
	out["ticks"] = perf.ticks;
	return out;
}
