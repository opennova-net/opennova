#include <runtime/session/session.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova::inmatch;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

struct TickProbe final : TickTarget {
	std::vector<TickInput> inputs;
	int32_t logic_tick = 100;
	int reset_calls = 0;
	int close_calls = 0;
	TickStatus next_status = TickStatus::Ran;

	TickOutcome advance_mission_tick(const TickInput &input) override {
		inputs.push_back(input);
		TickOutcome out;
		out.status = next_status;
		if (next_status == TickStatus::Ran) out.logic_tick = ++logic_tick;
		if (next_status == TickStatus::SessionLost) {
			out.error = {SessionErrorCode::SessionLost, "peer left"};
		}
		return out;
	}

	bool reset_mission_to_baseline(SessionError &) override {
		++reset_calls;
		logic_tick = 100;
		return true;
	}

	void close_mission() override { ++close_calls; }
};

bool load(Session &session) {
	return session.begin_load().applied() && session.complete_load().applied();
}

} // namespace

int main() {
	using opennova::world::TickAccumulator;

	// Lifecycle transitions are explicit and invalid transitions are inert.
	{
		TickProbe target;
		Session session(target);
		if (!expect(session.state() == State::Unloaded,
				"new session is unloaded")) return 1;
		if (!expect(session.complete_load().code == TransitionCode::InvalidState,
				"load cannot complete before it begins")) return 1;
		if (!expect(load(session), "load reaches running")) return 1;
		if (!expect(session.pause().applied() &&
				session.state() == State::Paused,
				"single-player pauses")) return 1;
		if (!expect(session.resume().applied() &&
				session.state() == State::Running,
				"paused session resumes")) return 1;
		if (!expect(session.close().applied() && target.close_calls == 1 &&
				session.state() == State::Unloaded,
				"close releases once and unloads")) return 1;
		if (!expect(session.close().code == TransitionCode::NoOp &&
				target.close_calls == 1,
				"close is idempotent")) return 1;
	}

	// One frame sample feeds every catch-up tick, with edges consumed once.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput in;
		in.delta_seconds = TickAccumulator::kTickDt * 3.0;
		in.player.movement.forward = true;
		in.player.look_delta_x = 4.0f;
		in.player.held_action_bits = 0x10u;
		in.player.pressed_action_bits = 0x5u;
		in.player.sequence = 7;
		const FrameOutcome out = session.advance(in);
		if (!expect(out.status == FrameStatus::Ok && out.ticks_run() == 3,
				"three quanta run three ticks")) return 1;
		if (!expect(target.inputs.size() == 3 &&
				target.inputs[0].consume_one_shots &&
				target.inputs[0].player.held_action_bits == 0x10u &&
				target.inputs[0].player.pressed_action_bits == 0x5u &&
				target.inputs[0].player.look_delta_x == 4.0f,
				"first tick consumes the sampled edges")) return 1;
		if (!expect(!target.inputs[1].consume_one_shots &&
				target.inputs[1].player.held_action_bits == 0x10u &&
				target.inputs[1].player.pressed_action_bits == 0 &&
				target.inputs[1].player.look_delta_x == 0.0f &&
				target.inputs[1].player.movement.forward,
				"later catch-up ticks reuse held state without edges")) return 1;
	}

	// A zero-tick frame retains edge input until a tick actually runs.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput first;
		first.delta_seconds = TickAccumulator::kTickDt / 2.0;
		first.player.pressed_action_bits = 0x8u;
		first.player.look_delta_y = -3.0f;
		if (!expect(session.advance(first).ticks_run() == 0 && target.inputs.empty(),
				"half quantum runs no tick")) return 1;
		FrameInput second;
		second.delta_seconds = TickAccumulator::kTickDt / 2.0;
		const FrameOutcome out = session.advance(second);
		if (!expect(out.ticks_run() == 1 && target.inputs[0].player.pressed_action_bits == 0x8u &&
				target.inputs[0].player.look_delta_y == -3.0f,
				"zero-tick edges survive to the next tick")) return 1;
	}

	// The retail hitch clamp is owned here and drops the clamped backlog.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput hitch;
		hitch.delta_seconds = 1.0;
		if (!expect(session.advance(hitch).ticks_run() ==
					TickAccumulator::kMaxCatchupTicks,
				"hitch clamps to the fixed maximum")) return 1;
		target.inputs.clear();
		FrameInput tiny;
		tiny.delta_seconds = 0.001;
		if (!expect(session.advance(tiny).ticks_run() == 0,
				"clamped backlog is dropped")) return 1;
	}

	// Pause clears banked time; manual step and reset stay local-only.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput half;
		half.delta_seconds = TickAccumulator::kTickDt / 2.0;
		(void)session.advance(half);
		if (!expect(session.pause().applied(), "pause applies")) return 1;
		if (!expect(session.step_once().ticks_run() == 1,
				"paused local session can step once")) return 1;
		if (!expect(session.reset_to_baseline().applied() && target.reset_calls == 1 &&
				session.state() == State::Paused,
				"reset restores baseline and remains paused")) return 1;
		if (!expect(session.resume().applied(), "resume after reset")) return 1;
		if (!expect(session.advance(half).ticks_run() == 0,
				"pause/reset discarded the old half quantum")) return 1;
	}

	// Network roles cannot pause, step, or reset.
	{
		TickProbe target;
		Session session(target, Role::ListenHost);
		if (!load(session)) return 1;
		if (!expect(session.pause().code == TransitionCode::RejectedForNetworkRole,
				"network role rejects pause")) return 1;
		if (!expect(session.drive_one().ticks_run() == 1 &&
				session.last_perf().ticks == 1,
				"external network drive records one tick")) return 1;
		if (!expect(session.step_once().status == FrameStatus::NotRunning,
				"network role rejects manual step")) return 1;
		if (!expect(session.last_perf().ticks == 0,
				"rejected manual step clears stale frame perf")) return 1;
		if (!expect(session.reset_to_baseline().code ==
					TransitionCode::RejectedForNetworkRole,
				"network role rejects reset")) return 1;
	}

	// Joiners expose their pre-load connection state.
	{
		TickProbe target;
		Session session(target, Role::Joiner);
		if (!expect(session.begin_connect().applied() &&
				session.state() == State::Connecting,
				"joiner enters Connecting")) return 1;
		if (!expect(session.begin_load().applied() && session.complete_load().applied(),
				"connected joiner loads into Running")) return 1;
	}

	// A terminal tick cancels the rest of the batch and fails the session.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		target.next_status = TickStatus::SessionLost;
		FrameInput in;
		in.delta_seconds = TickAccumulator::kTickDt * 4.0;
		const FrameOutcome out = session.advance(in);
		if (!expect(out.terminal() && out.status == FrameStatus::SessionLost &&
				target.inputs.size() == 1 &&
				session.state() == State::Failed,
				"terminal tick aborts the batch and fails the session")) return 1;
	}

	std::printf("OK: in-match session lifecycle/cadence/input/failure\n");
	return 0;
}
