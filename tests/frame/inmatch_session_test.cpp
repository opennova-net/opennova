#include <runtime/inmatch/session.h>

#include <runtime/controls/controls.h> // action_code (the head gate pins)
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/radar_contacts.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
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
	std::vector<bool> head_flags;        // World::rules.update_head_stepped as the tick saw it
	std::vector<uint32_t> ticks_seen;    // World::logic_tick at the tick's head
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
		head_flags.push_back(kernel.world.rules.update_head_stepped);
		ticks_seen.push_back(kernel.world.logic_tick);
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
	std::vector<uint32_t> profile_ticks; // the logic tick each /PROFILE frame leg saw
	void write_profile_frame() override { profile_ticks.push_back(kernel.world.logic_tick); }
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
	// [orig: Game_MainLoop g_StatsAvgFps @0x52B98F, drain @0x52BA08].
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

		// An unsampled render clock falls back to the frame delta. The restart is no
		// mode init, so the bank keeps its history: the residual 16 plus the 48 ms
		// (768 units), smoothed against 336: (7 * 336 + 784 + 4) >> 3 = 392 units,
		// 6 quanta from phase 5, one tick (phase 8).
		if (!expect(session.reset_to_baseline().applied() &&
				session.resume().applied(), "restart runs again")) return 1;
		FrameInput unsampled;
		unsampled.delta_seconds = TickAccumulator::kTickDt * 3.0;
		if (!expect(session.advance(unsampled).ticks_run() == 1,
				"no render sample: the frame delta banks")) return 1;
	}

	// Pause, a manual step and a reset keep the bank and its smoothing history
	// (D-NET-NEW-LI-8): no pause writes g_TickPhase or g_FrameTimeSmoothedFp4, and
	// a restart is no mode init [orig: Game_MainLoop @0x52B630 (the mode init
	// @0x52B715 / @0x52B71B, the EMA @0x52B845, the drain's phase @0x52BA6B);
	// Game_RestartRoundSP @0x5263A0]. Manual step and reset stay local-only.
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
		// The stall drained its clamped 8000 units to 0 at phase 125; the 1 ms frame
		// fast-forwards through that history: (7 * 8000 + 16 + 4) >> 3 = 7002 units,
		// 109 quanta from phase 125, 27 ticks (a cold bank would drain nothing).
		FrameInput tiny;
		tiny.delta_seconds = 0.001;
		if (!expect(session.advance(tiny).ticks_run() == 27,
				"pause/reset kept the stall's bank history")) return 1;
	}

	// The pause row's flip applies at the update whose input frame reads it: a
	// three-update catch-up frame whose second update pauses runs one role tick, not
	// three; the pausing update has stepped its HUD counters unpaused ahead of its
	// input frame and then runs the paused legs, and the frozen tick is its own. The
	// next frame's second update resumes: its counters stepped paused ahead of its
	// input frame, its tick runs with the head stepped (no second step), and the
	// frame's last update samples anew. The action rows' head drops a bit-0 row
	// (the weapon select, the stance) while the word holds and passes the radar zoom
	// (0x04000800, no bit 0). [orig: Game_ProcessMainFrame @0x5263F0 (`add
	// g_CurrentTick` @0x5265B4, Game_TickHudFrameCounters @0x5265D5, Input_ProcessFrame
	// @0x52661D, Server_TickUpdate @0x5266AE, Entity_UpdateAllEntities @0x52674B);
	// Entity_UpdateAllEntities @0x4C2100 (@0x4C2103); Input_HandleActionBinding
	// @0x49AD40 (case 25 @0x49B520, the pause gate @0x49ADDD..0x49ADE8)]
	{
		struct PauseRow final : TickInputSource {
			std::vector<std::pair<uint32_t, bool>> calls; // (logic tick, new sample)
			int flip_at = -1;                              // the call whose pause row fires
			InputPacket sample_tick(uint32_t) override { return {}; }
			bool run_update_rows(uint32_t logic_tick, bool new_sample) override {
				calls.emplace_back(logic_tick, new_sample);
				return static_cast<int>(calls.size()) - 1 == flip_at;
			}
		};
		auto target = std::make_unique<TickProbe>();
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 8);
		opennova::world::Entity seed;
		seed.kind = opennova::world::EntityKind::Organic;
		seed.alive = true;
		world.cached.local_player = world.registry.spawn(0, seed);
		const int32_t weapon = opennova::controls::action_code(30); // Primary, 0x0C000801
		const int32_t stance = opennova::controls::action_code(10); // Crouch, 0x0C000C01
		const int32_t zoom = opennova::controls::action_code(48);   // radarout, 0x04000800
		if (!expect(world.cached.local_player.valid() && target->action_head_admits(weapon) &&
				target->action_head_admits(stance) && target->action_head_admits(zoom),
				"the rows pass the head while running")) return 1;
		PauseRow rows;
		rows.flip_at = 1;
		const opennova::world::HudFrameCounters before = world.hud_frames;
		FrameInput frame;
		frame.delta_seconds = TickAccumulator::kTickDt * 3.0;
		frame.tick_source = &rows;
		const FrameOutcome paused = session.advance(frame);
		if (!expect(paused.status == FrameStatus::Ok && paused.ticks_run() == 1 &&
				target->inputs.size() == 1 && session.state() == State::Paused &&
				paused.state == State::Paused,
				"the second update pauses: one role tick, not three")) return 1;
		if (!expect(world.logic_tick == 101 && target->profile_ticks ==
				std::vector<uint32_t>({101u, 101u}),
				"the frozen tick is the pausing update's own, its paused legs and the "
				"next update's at it")) return 1;
		if (!expect(rows.calls == std::vector<std::pair<uint32_t, bool>>(
				{{100u, false}, {101u, true}, {101u, false}}),
				"every update polls its rows, a later running one over its own sample")) return 1;
		if (!expect(world.hud_frames.main_frame - before.main_frame == 2 &&
				world.hud_frames.session_frame - before.session_frame == 1 &&
				world.hud_frames.hud_frame - before.hud_frame == 1,
				"the pausing update's counters step unpaused, the paused one's paused")) return 1;
		if (!expect(world.rules.pause_word && !target->action_head_admits(weapon) &&
				!target->action_head_admits(stance) && target->action_head_admits(zoom),
				"the pause word drops the bit-0 rows and passes the radar zoom")) return 1;
		rows.calls.clear();
		rows.flip_at = 1;
		const opennova::world::HudFrameCounters paused_counters = world.hud_frames;
		const FrameOutcome resumed = session.advance(frame);
		if (!expect(resumed.status == FrameStatus::Ok && resumed.ticks_run() == 2 &&
				session.state() == State::Running && target->inputs.size() == 3,
				"the second update resumes and the frame's last runs too")) return 1;
		if (!expect(target->head_flags == std::vector<bool>({false, true, false}) &&
				target->inputs[1].head_stepped && target->ticks_seen[1] == 101 &&
				target->ticks_seen[2] == 102 && world.logic_tick == 103,
				"the resuming tick runs at the frozen tick with its head stepped")) return 1;
		if (!expect(rows.calls == std::vector<std::pair<uint32_t, bool>>(
				{{101u, false}, {101u, false}, {102u, true}}),
				"the resuming update reads the frame's sample, the next its own")) return 1;
		if (!expect(world.hud_frames.main_frame - paused_counters.main_frame == 2 &&
				world.hud_frames.session_frame == paused_counters.session_frame,
				"the probe's ticks step no counters: the paused and the resuming head's "
				"paused steps alone")) return 1;
		if (!expect(!world.rules.pause_word && target->action_head_admits(weapon) &&
				target->action_head_admits(stance),
				"the resume admits the rows again")) return 1;
		// World::begin_tick skips its own step on the resuming tick.
		const opennova::world::HudFrameCounters head = world.hud_frames;
		world.rules.update_head_stepped = true;
		(void)world.begin_tick(true, opennova::world::TickPhase::Gameplay);
		if (!expect(world.hud_frames.main_frame == head.main_frame,
				"a stepped head steps no counter at the tick")) return 1;
		world.rules.update_head_stepped = false;
		(void)world.begin_tick(true, opennova::world::TickPhase::Gameplay);
		if (!expect(world.hud_frames.main_frame == head.main_frame + 1 &&
				world.hud_frames.hud_frame == head.hud_frame + 1,
				"an unstepped head steps them at the tick")) return 1;
	}

	// A paused frame runs no tick but still runs the weather tick: retail's main
	// frame calls Environment_UpdateWeatherTick ahead of its pause test, so the
	// time of day keeps advancing (one advance per banked frame) and the
	// 310-tick minute counter keeps counting down through the pause.
	// [orig: Game_ProcessMainFrame @0x5263F0 -- Environment_UpdateWeatherTick
	//  @0x57E9B0 called @0x526774, the pause test @0x526779; the clock
	//  @0x57E9C7, the tickdown @0x57E9DA]
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		auto &weather = target.kernel.world.weather;
		weather.tod_fixed24 = 0;
		weather.tod_advance_per_tick = 0x100;
		weather.tod_epoch_tickdown = 310;
		if (!expect(session.pause().applied(), "the weather pause applies")) return 1;
		FrameInput paused;
		paused.delta_seconds = TickAccumulator::kTickDt * 3.0;
		const FrameOutcome out = session.advance(paused);
		if (!expect(out.status == FrameStatus::NotRunning && target.inputs.empty(),
				"a paused frame runs no tick")) return 1;
		if (!expect(weather.tod_fixed24 == 0x300u && weather.tod_epoch_tickdown == 307,
				"each paused frame steps the time of day and its minute counter")) return 1;
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

	// The frame statistics: every 62 logic updates publish the frames drawn
	// since, the frame whose drain crosses the 62nd not yet among them
	// [orig: Game_ProcessMainFrame @0x5267ab..0x5267df; GameLoop_RenderFrame
	// @0x521cf9].
	{
		TickProbe target;
		Session session(target);
		if (!load(session)) return 1;
		if (!expect(session.frame_statistics().frames_last_second == 0,
				"no second has passed yet")) return 1;
		FrameInput in;
		in.delta_seconds = TickAccumulator::kTickDt;
		int32_t total = 0;
		int32_t crossing_frame = -1;
		for (int32_t frame = 0; frame < 200 && crossing_frame < 0; ++frame) {
			total += session.advance(in).ticks_run();
			if (total >= 62) crossing_frame = frame;
		}
		if (!expect(crossing_frame > 0 &&
				session.frame_statistics().frames_last_second == crossing_frame,
				"the 62nd update publishes the frames drawn before its frame")) return 1;
	}

	// A paused single-player frame's updates run the legs retail's main frame
	// runs outside its pause tests: the pending fire-sound slots count down and
	// play, the incoming-lock tone drains one per update (31 -> 28 over three),
	// the tracer pool ages its channels, and the updates and the renders count
	// into the 62-update second as a running frame's do.
	// [orig: Game_ProcessMainFrame @0x5263F0 -- the Sound_TickPendingSlots call
	//  @0x526697, the CEffectEmitterPool_Tick call @0x526758, the 62-update
	//  second @0x5267AB..0x5267DF, the pause tests @0x5265A0 / @0x526779;
	//  Sound_TickPendingSlots @0x529310 (the g_LockOnMeToneTicks drain
	//  @0x5293A5..0x5293AF); GameLoop_RenderFrame @0x521CF9]
	{
		auto target = std::make_unique<TickProbe>();
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		target->kernel.local.radar.lock_tone = opennova::world::kRadarLockToneTicks;
		world.out.fire_sounds.set_listener({});
		world.out.fire_sounds.play_throttled_interface("PAUSED_SET", {}, 3, 10);
		const int slot = world.round_sim.trails.alloc(1);
		world.round_sim.trails.append(slot, {});            // a fresh channel: count -1 -> 0
		world.round_sim.trails.append(slot, {1.0f, 0.0f, 0.0f}); // one point, age 0
		if (!expect(world.out.fire_sounds.pending_count() == 1 && slot >= 0,
				"a pending slot and a live tracer channel")) return 1;
		if (!expect(session.pause().applied(), "the drains' pause applies")) return 1;
		FrameInput paused;
		paused.delta_seconds = TickAccumulator::kTickDt * 3.0;
		if (!expect(session.advance(paused).status == FrameStatus::NotRunning &&
				target->inputs.empty(), "a paused frame of three updates runs no tick")) return 1;
		if (!expect(world.out.fire_sounds.pending_count() == 0 &&
				world.out.fire_sounds.drain().size() == 1,
				"the pending slot counts down and plays through the pause")) return 1;
		if (!expect(world.round_sim.trails.channels[static_cast<size_t>(slot)].age == 3,
				"the tracer channel ages once per paused update")) return 1;
		if (!expect(target->kernel.local.radar.lock_tone == 28,
				"the incoming-lock tone drains once per paused update")) return 1;
		// The /PROFILE frame leg sits past the pause tests too: each paused update
		// runs it at the tick the pause froze [orig: Game_ProcessMainFrame -- the
		// write @0x526879..0x5268E7; ++g_CurrentTick inside the gate @0x5265A0..0x5265B4].
		if (!expect(target->profile_ticks.size() == 3 &&
				target->profile_ticks[0] == 100 && target->profile_ticks[2] == 100,
				"each paused update runs the /PROFILE leg at the frozen tick")) return 1;
	}
	// Each paused update also runs the third-person camera's update, as every
	// running update does: a driver's chase anchor keeps easing toward the
	// (held) carrier, a sixteenth a update across and a thirty-second up, and a
	// dead tracked body's distance keeps reeling in. The orbit-yaw bits stay
	// clear in single player (only the death screen's chase sub-mode raises
	// them, and only a client receive sets the death screen), so the orbit holds.
	// [orig: Game_ProcessMainFrame @0x5263F0 -- the tracked-entity test
	//  @0x526767, the ThirdPersonCamera_Update call @0x52676F, the pause test
	//  @0x526779; ThirdPersonCamera_Update @0x437AF0 -- the mounted target
	//  @0x437B1F..0x437B63, the ease @0x437C56..0x437C79, the reel
	//  @0x437CC0..0x437D02; DeathScreen_FilterInputAction @0x52AD50;
	//  NapiNPClientMsg_SetSpectatorMode @0x425A18; NapiNPClientMsg_0x00A @0x42FFA0]
	{
		auto target = std::make_unique<TickProbe>();
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 8);
		opennova::world::Entity seed;
		seed.kind = opennova::world::EntityKind::Organic;
		seed.alive = true;
		world.cached.local_player = world.registry.spawn(0, seed);
		opennova::world::PlayerViewState &view = target->kernel.local.view;
		view.third_person = true;
		view.tp_anchor_valid = true;
		view.mount.control_seat = true;
		view.mount.carrier_pos_q16[0] = 0x100000;
		view.mount.carrier_pos_q16[1] = -0x80000;
		view.mount.carrier_pos_q16[2] = 0x40000;
		view.mount.bound_radius.q16 = 0x20000; // (24576 * r + 0x8000) >> 16 = 0xC000: floored to 1.0
		view.chase_distance_q16 = 0x80000;
		const int32_t orbit_yaw = view.chase_orbit_yaw;
		target->kernel.local.view_tracker.camera_tracked_dead = true;
		if (!expect(world.cached.local_player.valid() && session.pause().applied(),
				"the camera's pause applies over a tracked driver")) return 1;
		const int32_t carrier_target[3] = {0x100000, -0x80000, 0x40000 + 0x10000};
		int32_t anchor[3] = {0, 0, 0};
		int32_t distance = 0x80000;
		for (int update = 0; update < 3; ++update) {
			for (int i = 0; i < 3; ++i) {
				anchor[i] += i == 2 ? (carrier_target[i] - anchor[i] + 16) >> 5
						: (carrier_target[i] - anchor[i] + 8) >> 4;
			}
			if (distance > 458752) distance -= 0x10000;
			else if (distance > 196608) {
				if (distance - 196608 <= 16) distance = 0x30000;
				else distance -= (distance - 196608) >> 4;
			}
		}
		FrameInput paused;
		paused.delta_seconds = TickAccumulator::kTickDt * 3.0;
		if (!expect(session.advance(paused).status == FrameStatus::NotRunning &&
				target->inputs.empty(), "the camera's paused frame of three updates runs no tick")) return 1;
		if (!expect(view.tp_anchor_q16[0] == anchor[0] && view.tp_anchor_q16[1] == anchor[1] &&
				view.tp_anchor_q16[2] == anchor[2] && anchor[0] == 0x2D100,
				"the chase anchor eases once per paused update")) return 1;
		if (!expect(view.chase_distance_q16 == distance && distance == 427008,
				"the dead target's distance reels in once per paused update")) return 1;
		if (!expect(view.chase_orbit_yaw == orbit_yaw,
				"the orbit holds: no single-player input raises its bits")) return 1;
	}
	{
		auto target = std::make_unique<TickProbe>();
		Session session(*target);
		if (!load(session)) return 1;
		if (!expect(session.pause().applied(), "the statistics' pause applies")) return 1;
		const uint32_t main0 = target->kernel.world.hud_frames.main_frame;
		FrameInput in;
		in.delta_seconds = TickAccumulator::kTickDt;
		int32_t crossing_frame = -1;
		for (int32_t frame = 0; frame < 200 && crossing_frame < 0; ++frame) {
			session.advance(in);
			if (target->kernel.world.hud_frames.main_frame - main0 >= 62) crossing_frame = frame;
		}
		if (!expect(crossing_frame > 0 &&
				session.frame_statistics().frames_last_second == crossing_frame,
				"paused updates publish the frames drawn before the 62nd's frame")) return 1;
	}

	// The session banks the frame's elapsed as the shell measures it: a 300 ms
	// frame in a steady 16 ms stream runs 17 ticks beyond a 16 ms frame's over the
	// EMA's catch-up, where Godot's process step capped at 8/60 s ran 6
	// (D-NET-NEW-LI-6; world::TickAccumulator carries the witness).
	// [orig: Game_MainLoop @0x52B630 -- the elapsed @0x52B798..0x52B7A4]
	{
		const auto catchup = [](double long_frame) {
			auto target = std::make_unique<TickProbe>();
			Session session(*target);
			if (!load(session)) return -1;
			FrameInput in;
			in.delta_seconds = TickAccumulator::kTickDt;
			for (int i = 0; i < 60; ++i) session.advance(in);
			const size_t before = target->inputs.size();
			FrameInput lng;
			lng.delta_seconds = long_frame;
			session.advance(lng);
			for (int i = 0; i < 120; ++i) session.advance(in);
			return static_cast<int>(target->inputs.size() - before);
		};
		const int stream = catchup(TickAccumulator::kTickDt);
		if (!expect(stream > 0 && catchup(0.300) - stream == 17 &&
				catchup(8.0 / 60.0) - stream == 6,
				"a 300 ms frame keeps its time; the 8/60 s cap dropped it")) return 1;
	}

	std::printf("OK: in-match session lifecycle/cadence/input/failure\n");
	return 0;
}
