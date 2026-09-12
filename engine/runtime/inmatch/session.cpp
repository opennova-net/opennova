#include <runtime/inmatch/session.h>

#include <base/io/perf_clock.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/mission/mission_kernel.h>

#include <utility>

namespace opennova::inmatch {

// The session banks real time and dispatches one target call per 16 ms quantum,
// matching the original outer/logic-loop split [orig: Game_MainLoop @ 0x52b630
// -> Game_ProcessMainFrame @ 0x5263f0].

namespace {

SessionError invalid_transition(const char *message) {
	return {SessionErrorCode::InvalidTransition, message};
}

} // namespace

Session::Session(Role &role) : role_(&role), kind_(role.kind()) {}

void Role::apply_input(const TickInput &input) {
	mission::MissionKernel &kernel = *kernel_;
	const bool spectating = spectator();
	const world::PlayerInput no_movement{};
	const world::PlayerInput &movement = spectating ? no_movement : input.player.movement;
	kernel.local.set_movement_keys(movement.forward, movement.back, movement.left,
			movement.right, movement.lean_left, movement.lean_right, movement.jump);
    kernel.local.set_view_keys(movement.free_look, movement.look_up,
            movement.look_down, movement.turn_left, movement.turn_right);
	if (!spectating && (input.player.look_delta_x != 0.0f || input.player.look_delta_y != 0.0f))
		kernel.local.look(input.player.look_delta_x, input.player.look_delta_y);
	kernel.local.set_weapon_input(
			!spectating && (input.player.held_action_bits & HELD_FIRE) != 0,
			!spectating && (input.player.pressed_action_bits & PRESSED_FIRE) != 0,
			!spectating && (input.player.pressed_action_bits & PRESSED_RELOAD) != 0);
	// The medic-call edge is an action binding, not weapon state: it fires its
	// request immediately like retail's binding dispatch (the gates and the
	// cooldown live in request_medic).
	if (!spectating && (input.player.pressed_action_bits & PRESSED_MEDIC_REQUEST) != 0)
		request_medic();
}

bool Role::request_medic() {
	mission::MissionKernel &kernel = *kernel_;
	// The session/entity gates: a replica runtime and a local entity; the
	// dead-bit and cooldown gates are the local player's.
	if (client_runtime() == nullptr || !kernel.world.cached.local_player.valid()) return false;
	const bool local_dead = kind() == RoleKind::Joiner
			? client_runtime()->local_player_dead() : kernel.local.local_player_dead();
	if (!kernel.local.medic_request_allowed(local_dead)) return false;
	if (!send_medic_request()) return false;
	kernel.local.stamp_medic_request();
	return true;
}

world::LocalViewSessionInputs Role::view_session_inputs_for(
		const ClientRuntime *runtime, bool joiner, bool local_dead) {
	// What the arbiter reads from the session: the net layer sits above the
	// world group, so its client state crosses as plain values.
	world::LocalViewSessionInputs s;
	s.in_session = runtime != nullptr;
	s.joiner = joiner;
	// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
	// role's view folds (the listen host's own loopback included)
	// [orig: g_death_screen_active, NapiNPClientMsg_0x00A @0x42ff88..0x43002b].
	s.death_screen_active = runtime != nullptr && runtime->state().death_screen_active;
	s.death_screen_submode = runtime != nullptr ? runtime->state().death_screen_submode : 0;
	s.end_round_known = runtime != nullptr && runtime->state().end_round.known;
	s.local_dead = local_dead;
	s.death_camera_target_known = runtime != nullptr;
	if (runtime != nullptr) {
		const replication::ClientDeathCameraTarget &t = runtime->state().death_camera;
		s.death_camera_target[0] = t.x;
		s.death_camera_target[1] = t.y;
		s.death_camera_target[2] = t.z;
	}
	return s;
}

int64_t Session::now_us() {
	return static_cast<int64_t>(io::perf_now_us());
}

TransitionResult Session::transition(State to) {
	TransitionResult out;
	out.from = state_;
	out.to = to;
	if (state_ == to) {
		out.code = TransitionCode::NoOp;
		return out;
	}
	state_ = to;
	out.code = TransitionCode::Applied;
	return out;
}

TransitionResult Session::rejected(
		TransitionCode code, SessionError error) const {
	TransitionResult out;
	out.code = code;
	out.from = state_;
	out.to = state_;
	out.error = std::move(error);
	return out;
}

TransitionResult Session::configure_role(Role &role) {
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("role can change only while unloaded or failed"));
	}
	if (role_ == &role && kind_ == role.kind()) return rejected(TransitionCode::NoOp);
	role_ = &role;
	kind_ = role.kind();
	return {TransitionCode::Applied, state_, state_, {}};
}

TransitionResult Session::begin_connect() {
	if (kind_ != RoleKind::Joiner) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("only a joiner can enter Connecting"));
	}
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("connect requires Unloaded or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Connecting);
}

TransitionResult Session::begin_load() {
	if (state_ != State::Unloaded &&
			state_ != State::Connecting &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load requires Unloaded, Connecting, or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Loading);
}

TransitionResult Session::complete_load() {
	if (state_ != State::Loading) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load completion requires Loading"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Running);
}

TransitionResult Session::fail(SessionError error) {
	if (!error) error = {SessionErrorCode::TickFailed, "session failed"};
	last_error_ = error;
	reset_bank();
	TransitionResult out = transition(State::Failed);
	out.error = std::move(error);
	return out;
}

void Session::latch_input(const FrameInput &input) {
	pending_input_.movement = input.player.movement;
	pending_input_.look_delta_x += input.player.look_delta_x;
	pending_input_.look_delta_y += input.player.look_delta_y;
	pending_input_.held_action_bits = input.player.held_action_bits;
	pending_input_.pressed_action_bits |= input.player.pressed_action_bits;
	pending_input_.sequence = input.player.sequence;
	latest_camera_ = input.camera;
}

TickInput Session::merged_tick_input(
		const FrameInput &input, bool consume_one_shots) {
	TickInput out;
	out.camera = latest_camera_;
	out.viewport_height = input.viewport_height;
	out.player = pending_input_;
	out.consume_one_shots = consume_one_shots;
	if (!consume_one_shots) {
		out.player.look_delta_x = 0.0f;
		out.player.look_delta_y = 0.0f;
		out.player.pressed_action_bits = 0;
	}
	return out;
}

void Session::consume_pending_one_shots() {
	pending_input_.look_delta_x = 0.0f;
	pending_input_.look_delta_y = 0.0f;
	pending_input_.pressed_action_bits = 0;
}

FrameOutcome Session::run_ticks(int32_t due, const FrameInput &input) {
	FrameOutcome out;
	out.status = FrameStatus::Ok;
	out.state = state_;
	const int64_t tick_start = now_us();
	bool consume_one_shots = true;
	for (int32_t i = 0; i < due; ++i) {
		TickInput tick_input = merged_tick_input(input, consume_one_shots);
		// [orig: Game_MainLoop @0x52ba21..0x52ba3a -- dword_24E0E80 = 1 when
		//  less than one 16 ms tick of backlog remains after this quantum, 0
		//  while catching up (and always 0 under g_cineFixedStepMode, which
		//  the port does not model)]
		tick_input.last_tick_of_batch = i + 1 == due;
		TickOutcome tick = run_one_tick(tick_input);
		if (tick.terminal()) {
			out.status = tick.status == TickStatus::SessionLost
					? FrameStatus::SessionLost : FrameStatus::Fatal;
			out.error = tick.error;
			fail(tick.error ? tick.error : SessionError{
					SessionErrorCode::TickFailed, "mission tick failed"});
			out.state = state_;
			break;
		}
		if (!tick.ran()) continue;
		out.ticks.push_back(std::move(tick));
		if (consume_one_shots) {
			consume_pending_one_shots();
			consume_one_shots = false;
		}
	}
	out.perf.tick_us = now_us() - tick_start;
	out.perf.ticks = out.ticks_run();
	return out;
}

// One fixed tick: the shared input prologue through the role, the role's
// tick, then the outcome and the shell's observer.
TickOutcome Session::run_one_tick(const TickInput &input) {
	TickOutcome out;
	if (role_ == nullptr || role_->kernel() == nullptr) {
		out.status = TickStatus::Fatal;
		out.error = {SessionErrorCode::TickFailed, "no role is bound to the session"};
		return out;
	}
	role_->kernel()->world.rules.last_tick_of_batch = input.last_tick_of_batch;
	role_->apply_input(input);
	const int64_t tick_start = now_us();
	role_->run_tick(input);
	out.tick_us = now_us() - tick_start;
	out.net_us = role_->last_net_us();
	out.logic_tick = static_cast<int32_t>(role_->kernel()->world.logic_tick);
	if (observer_ != nullptr) observer_->after_tick();
	SessionError lost;
	if (role_->session_lost(lost)) {
		out.status = TickStatus::SessionLost;
		out.error = lost;
		return out;
	}
	out.status = TickStatus::Ran;
	if (observer_ != nullptr && !observer_->accept_tick(out)) {
		out.status = TickStatus::SessionLost;
		out.error = {SessionErrorCode::SessionLost, "the tick observer cancelled the batch"};
	}
	return out;
}

FrameOutcome Session::advance(const FrameInput &input) {
	const int64_t frame_start = now_us();
	FrameOutcome out;
	out.state = state_;
	if (state_ != State::Running) {
		out.status = FrameStatus::NotRunning;
		out.perf.frame_us = now_us() - frame_start;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_ticks(accumulator_.bank(input.delta_seconds), input);
	out.perf.frame_us = now_us() - frame_start;
	last_perf_ = out.perf;
	return out;
}

FrameOutcome Session::step_once(const FrameInput &input) {
	const int64_t frame_start = now_us();
	FrameOutcome out;
	out.state = state_;
	if (state_ != State::Paused ||
			kind_ != RoleKind::SinglePlayer) {
		out.status = FrameStatus::NotRunning;
		out.perf.frame_us = now_us() - frame_start;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_ticks(1, input);
	out.perf.frame_us = now_us() - frame_start;
	last_perf_ = out.perf;
	return out;
}

FrameOutcome Session::drive_one(const FrameInput &input) {
	const int64_t frame_start = now_us();
	FrameOutcome out;
	out.state = state_;
	const bool local_paused = state_ == State::Paused &&
			kind_ == RoleKind::SinglePlayer;
	if (state_ != State::Running && !local_paused) {
		out.status = FrameStatus::NotRunning;
		out.perf.frame_us = now_us() - frame_start;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_ticks(1, input);
	out.perf.frame_us = now_us() - frame_start;
	last_perf_ = out.perf;
	return out;
}

TransitionResult Session::pause() {
	if (kind_ != RoleKind::SinglePlayer) {
		return rejected(TransitionCode::RejectedForNetworkRole,
				{SessionErrorCode::NetworkRoleLocked,
						"network sessions cannot pause"});
	}
	if (state_ == State::Paused)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Running) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("pause requires Running"));
	}
	reset_bank();
	return transition(State::Paused);
}

TransitionResult Session::resume() {
	if (state_ == State::Running)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Paused) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("resume requires Paused"));
	}
	reset_bank();
	return transition(State::Running);
}

TransitionResult Session::reset_to_baseline() {
	if (kind_ != RoleKind::SinglePlayer) {
		return rejected(TransitionCode::RejectedForNetworkRole,
				{SessionErrorCode::NetworkRoleLocked,
						"network sessions cannot reset"});
	}
	if (state_ != State::Running &&
			state_ != State::Paused) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("reset requires Running or Paused"));
	}
	const State from = state_;
	SessionError error;
	if (role_ == nullptr || !role_->reset_to_baseline(error)) {
		if (!error) error = {SessionErrorCode::TickFailed,
				"mission baseline reset failed"};
		return fail(error);
	}
	reset_bank();
	state_ = State::Paused;
	return {TransitionCode::Applied, from,
			State::Paused, {}};
}

TransitionResult Session::close() {
	if (state_ == State::Unloaded)
		return rejected(TransitionCode::NoOp);
	const State from = state_;
	state_ = State::Stopping;
	if (role_ != nullptr) role_->close();
	reset_bank();
	last_error_ = {};
	state_ = State::Unloaded;
	return {TransitionCode::Applied, from, state_, {}};
}

void Session::reset_bank() {
	accumulator_.reset();
	pending_input_ = {};
	latest_camera_ = {};
	last_perf_ = {};
}

} // namespace opennova::inmatch
