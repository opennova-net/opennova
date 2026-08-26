#include "inmatch/session.h"
#include <io/perf_clock.h>

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

Session::Session(TickTarget &target, Role role)
		: target_(target), role_(role) {}

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

TransitionResult Session::configure_role(Role role) {
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("role can change only while unloaded or failed"));
	}
	if (role_ == role) return rejected(TransitionCode::NoOp);
	role_ = role;
	return {TransitionCode::Applied, state_, state_, {}};
}

TransitionResult Session::begin_connect() {
	if (role_ != Role::Joiner) {
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
		const FrameInput &, bool consume_one_shots) {
	TickInput out;
	out.camera = latest_camera_;
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
		TickOutcome tick = target_.advance_mission_tick(
				merged_tick_input(input, consume_one_shots));
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
			role_ != Role::SinglePlayer) {
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
			role_ == Role::SinglePlayer;
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
	if (role_ != Role::SinglePlayer) {
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
	if (role_ != Role::SinglePlayer) {
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
	if (!target_.reset_mission_to_baseline(error)) {
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
	target_.close_mission();
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
