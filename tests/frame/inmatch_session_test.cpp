#include <runtime/inmatch/session.h>

#include <runtime/mission/mission_kernel.h>

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

// A probe role over a bare kernel: records every tick input, counts the
// session's reset/close calls, and can report the session lost.
struct TickProbe final : Role {
	opennova::mission::MissionKernel kernel;
	std::vector<TickInput> inputs;
	std::vector<bool> rules_batch_flags; // World::rules.last_tick_of_batch as the tick saw it
	std::vector<int32_t> frame_rates;     // every FR-counter value the session handed over
	int32_t logic_tick = 100;
	int reset_calls = 0;
	int close_calls = 0;
	TickStatus next_status = TickStatus::Ran;
	RoleKind probe_kind = RoleKind::SinglePlayer;

	TickProbe() { bind(kernel); kernel.world.logic_tick = 100; }
	RoleKind kind() const override { return probe_kind; }
	void run_tick(const TickInput &input) override {
		inputs.push_back(input);
		rules_batch_flags.push_back(kernel.world.rules.last_tick_of_batch);
		if (next_status == TickStatus::Ran) kernel.world.logic_tick = static_cast<uint32_t>(++logic_tick);
	}
	bool session_lost(SessionError &error) const override {
		if (next_status != TickStatus::SessionLost) return false;
		error = {SessionErrorCode::SessionLost, "peer left"};
		return true;
	}
	bool reset_to_baseline(SessionError &) override {
		++reset_calls;
		logic_tick = 100;
		kernel.world.logic_tick = 100;
		return true;
	}
	void close() override { ++close_calls; }
	void observe_frame_rate(int32_t fps) override { frame_rates.push_back(fps); }
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
		// dword_24E0E80: 0 while catching up, 1 on the batch's last tick, and
		// World::rules carries the copy before the role's tick runs.
		bool batch_flags_ok = target.rules_batch_flags.size() == target.inputs.size();
		for (size_t i = 0; batch_flags_ok && i < target.inputs.size(); ++i) {
			const bool last = i + 1 == target.inputs.size();
			batch_flags_ok = target.inputs[i].last_tick_of_batch == last &&
					target.rules_batch_flags[i] == last;
		}
		if (!expect(batch_flags_ok,
				"only the batch's last tick carries last_tick_of_batch")) return 1;
	}

	// A zero-tick frame retains edge input until a tick actually runs: a cold
	// bank drains nothing under 4 ms (16 + 32 units), and the next 14 ms frame
	// drains one quantum on the tick phase.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput first;
		first.delta_seconds = 0.002;
		first.player.pressed_action_bits = 0x8u;
		first.player.look_delta_y = -3.0f;
		if (!expect(session.advance(first).ticks_run() == 0 && target.inputs.empty(),
				"a sub-quantum frame runs no tick")) return 1;
		FrameInput second;
		second.delta_seconds = 0.014;
		const FrameOutcome out = session.advance(second);
		if (!expect(out.ticks_run() == 1 && target.inputs[0].player.pressed_action_bits == 0x8u &&
				target.inputs[0].player.look_delta_y == -3.0f,
				"zero-tick edges survive to the next tick")) return 1;
	}

	// The hitch clamp is owned here: a stall caps at 500 ms of bank, and the
	// frame after it is smoothed against that clamped history — retail's
	// post-stall fast-forward, not a dropped backlog
	// [orig: Game_MainLoop @0x52B630 — clamp @0x52B83E, EMA @0x52B85B].
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput hitch;
		hitch.delta_seconds = 1.0;
		if (!expect(session.advance(hitch).ticks_run() ==
					TickAccumulator::kRetailMaxCatchupTicks,
				"a hitch clamps to 500 ms of quanta")) return 1;
		FrameInput tiny;
		tiny.delta_seconds = 0.001;
		// (7 * 8000 + 16 + 4) >> 3 = 7002 units -> 109 quanta from phase 125 -> 27.
		if (!expect(session.advance(tiny).ticks_run() == 27,
				"the frame after a stall fast-forwards")) return 1;
	}

	// The session hands the FR counter to the role once per banked frame,
	// ahead of that frame's ticks: 0 until the first 2000 ms window closes
	// (a steady 16 ms stream closes it on frame 118), then its average
	// [orig: Game_MainLoop g_statsAvgFps @0x52B98F, drain @0x52BA08].
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput frame;
		frame.delta_seconds = TickAccumulator::kTickDt;
		for (int i = 0; i < 117; ++i) (void)session.advance(frame);
		if (!expect(target.frame_rates.size() == 117 && target.frame_rates.back() == 0,
				"the rate reads 0 until the first window closes")) return 1;
		(void)session.advance(frame);
		if (!expect(target.frame_rates.size() == 118 && target.frame_rates.back() == 58,
				"the closed window's average reaches the role")) return 1;
		if (!expect(session.drive_one().ticks_run() == 1 && target.frame_rates.size() == 118,
				"an external single-tick drive banks nothing and hands over nothing")) return 1;
	}

	// The mission start never banks the load, and the three frames drawn
	// after it never bank their own render: each following frame banks only
	// the time since the previous render [orig: Game Loop mode @0x82F340 ->
	// Game_StartMission @0x525e1f; Render_ProcessMainSceneFrame
	// @0x5caeff..0x5caf0e; Game_MainLoop @0x52B75C, @0x52bac8..0x52bad2].
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput slow;
		slow.delta_seconds = TickAccumulator::kTickDt * 10.0;
		slow.since_render_seconds = 0.0;
		for (int frame = 0; frame < 4; ++frame) {
			if (!expect(session.advance(slow).ticks_run() == 0,
					"a re-based frame banks only the time since the last render")) return 1;
		}
		// The full 160 ms banks again, smoothed against the start frames'
		// seed-only history: (7 * 16 + 2576 + 4) >> 3 = 336 units, 5 quanta,
		// 2 ticks.
		if (!expect(session.advance(slow).ticks_run() == 2,
				"after the start frames the full frame time banks again")) return 1;

		// An unsampled render clock falls back to the frame delta.
		if (!expect(session.reset_to_baseline().applied() &&
				session.resume().applied(), "restart runs again")) return 1;
		FrameInput unsampled;
		unsampled.delta_seconds = TickAccumulator::kTickDt * 3.0;
		if (!expect(session.advance(unsampled).ticks_run() == 3,
				"no render sample: the frame delta banks")) return 1;
	}

	// Pause clears the bank and its smoothing history; manual step and reset
	// stay local-only.
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput hitch;
		hitch.delta_seconds = 1.0;
		(void)session.advance(hitch);
		if (!expect(session.pause().applied(), "pause applies")) return 1;
		if (!expect(session.step_once().ticks_run() == 1,
				"paused local session can step once")) return 1;
		if (!expect(session.reset_to_baseline().applied() && target.reset_calls == 1 &&
				session.state() == State::Paused,
				"reset restores baseline and remains paused")) return 1;
		if (!expect(session.resume().applied(), "resume after reset")) return 1;
		// Without the resets this 1 ms frame would fast-forward 27 ticks
		// through the stall's clamped history; a cold bank drains nothing.
		FrameInput tiny;
		tiny.delta_seconds = 0.001;
		if (!expect(session.advance(tiny).ticks_run() == 0,
				"pause/reset discarded the stall's bank history")) return 1;
	}

	// Network roles cannot pause, step, or reset.
	{
		TickProbe target;
		target.probe_kind = RoleKind::ListenHost;
		Session session(target);
		if (!load(session)) return 1;
		if (!expect(session.pause().code == TransitionCode::RejectedForNetworkRole,
				"network role rejects pause")) return 1;
		if (!expect(session.drive_one().ticks_run() == 1 &&
				session.last_perf().ticks == 1,
				"external network drive records one tick")) return 1;
		if (!expect(target.inputs.size() == 1 && target.inputs[0].last_tick_of_batch &&
				target.rules_batch_flags[0],
				"a one-tick drive is its own batch end")) return 1;
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
		target.probe_kind = RoleKind::Joiner;
		Session session(target);
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
