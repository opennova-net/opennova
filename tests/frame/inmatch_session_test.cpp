#include <runtime/inmatch/session.h>

#include <runtime/controls/controls.h> // action_code (the head gate pins)
#include <runtime/hud/hud_toggles.h> // HudToggleState (the scene gate's view word)
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/radar_contacts.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../common/title_fixture.h"

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
	bool kernel_jo = test_titles::jo_world(kernel.world);
	std::vector<TickInput> inputs;
	std::vector<bool> rules_batch_flags; // World::rules.last_tick_of_batch as the tick saw it
	std::vector<bool> head_flags;        // World::rules.update_head_stepped as the tick saw it
	std::vector<uint32_t> ticks_seen;    // World::logic_tick at the tick's head
	std::vector<int32_t> frame_rates;     // every FR-counter value the session handed over
	int32_t logic_tick = 100;
	int close_calls = 0;
	TickStatus next_status = TickStatus::Ran;
	RoleKind probe_kind = RoleKind::SinglePlayer;

	// Smoothing-history scales the next tick raises, as the lose cine's edit fade
	// does from inside its update (WorldOutbox::frame_time_scales).
	// They are raised by the tick that finds `raise_after_ticks` ticks already run.
	std::vector<float> scales_to_raise;
	size_t raise_after_ticks = 0;

	TickProbe() { bind(kernel); kernel.world.logic_tick = 100; }
	RoleKind kind() const override { return probe_kind; }
	void run_tick(const TickInput &input) override {
		if (inputs.size() >= raise_after_ticks) {
			for (const float factor : scales_to_raise) kernel.world.out.frame_time_scales.push_back(factor);
			scales_to_raise.clear();
		}
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
	void close() override { ++close_calls; }
	void observe_frame_rate(int32_t fps) override { frame_rates.push_back(fps); }
	std::vector<uint32_t> profile_ticks; // the logic tick each /PROFILE frame leg saw
	void write_profile_frame() override { profile_ticks.push_back(kernel.world.logic_tick); }
	// The update count at which a joiner's final wait ends (0: never), once.
	size_t final_wait_end_after = 0;
	// The role reports its final wait (Role::in_final_wait) until the update that ends it.
	bool waiting = false;
	bool take_final_wait_end() override {
		if (final_wait_end_after == 0 || inputs.size() != final_wait_end_after) return false;
		final_wait_end_after = 0;
		waiting = false;
		return true;
	}
	bool in_final_wait() const override { return waiting; }
};

bool load(Session &session) {
	return session.begin_load().applied() && session.complete_load().applied();
}

} // namespace

int main(int argc, char **argv) {
	using opennova::world::TickAccumulator;

	// ctest inmatch_session_no_controls: a role no embedder named a binding catalog for reads none:
	// its first read aborts, naming the invariant, where Jointops.exe's catalog once stood in
	// (inmatch::Role::controls). The abort's handler exits clean, so ctest reads the message.
	if (argc > 1 && std::string(argv[1]) == "--no-controls") {
#if defined(_MSC_VER)
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no debug dialog, no report
#endif
		std::signal(SIGABRT, [](int) { std::_Exit(0); });
		const std::unique_ptr<TickProbe> unnamed(new TickProbe());
		(void)unnamed->controls();
		std::fputs("a role read a binding catalog no embedder named\n", stderr);
		return 1;
	}

	// Lifecycle transitions are explicit and invalid transitions are inert.
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput slow;
		slow.delta_seconds = TickAccumulator::kTickDt * 10.0;
		slow.since_render_seconds = 0.0;
		for (int frame = 0; frame < 4; ++frame) {
			const FrameOutcome out = session.advance(slow);
			if (!expect(out.ticks_run() == 0,
					"a re-based frame banks only the time since the last render")) return 1;
			// The three frames that count the start down present black, the scene frame's
			// clear ahead of its present; the fourth presents its scene
			// [orig: Render_ProcessMainSceneFrame @0x5CAE62..0x5CAE94, ahead of the present
			//  @0x5CAEA8 and the count-down @0x5CAF08].
			if (!expect(out.start_blank == (frame < 3),
					"the first three drawn frames past the start present black")) return 1;
		}
		// The full 160 ms banks again, smoothed against the start frames'
		// seed-only history: (7 * 16 + 2576 + 4) >> 3 = 336 units, 5 quanta,
		// 2 ticks.
		const FrameOutcome after_start = session.advance(slow);
		if (!expect(after_start.ticks_run() == 2 && !after_start.start_blank,
				"after the start frames the full frame time banks again")) return 1;

		// An unsampled render clock falls back to the frame delta, over the bank's
		// history: the residual 16 plus the 48 ms (768 units), smoothed against 336:
		// (7 * 336 + 784 + 4) >> 3 = 392 units, 6 quanta from phase 5, one tick (phase 8).
		FrameInput unsampled;
		unsampled.delta_seconds = TickAccumulator::kTickDt * 3.0;
		if (!expect(session.advance(unsampled).ticks_run() == 1,
				"no render sample: the frame delta banks")) return 1;
	}

	// Under the pause the scene frame still draws, so the start frames count down there too:
	// a start paused ahead of its first frame keeps the three it armed, and its first three
	// paused frames present black and re-base, the fourth not; each paused frame past a re-based
	// render banks only the time since it (its paused updates none), the fifth its full
	// 160 ms over the seed-only history, 2 updates, as the running frames above
	// [orig: Game_MainLoop @0x52BAA7..0x52BAC6 (the render callback @0x52BAC6), the re-base
	//  read @0x52BAC8..0x52BAD2; GameLoop_RenderFrame - the scene arm @0x521CE8;
	//  Render_ProcessMainSceneFrame - the menu @0x5CAE13 / @0x5CAE31, the clear
	//  @0x5CAE62..0x5CAE94, the count-down @0x5CAF08, the re-base flag @0x5CAF0E].
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		Session session(target);
		if (!load(session)) return 1;
		if (!expect(session.pause().applied() && session.state() == State::Paused,
				"paused start: the pause holds the start's armed frames")) return 1;
		FrameInput slow;
		slow.delta_seconds = TickAccumulator::kTickDt * 10.0;
		slow.since_render_seconds = 0.0;
		for (int frame = 0; frame < 4; ++frame) {
			const FrameOutcome out = session.advance(slow);
			if (!expect(out.scene_drawn && out.perf.updates == 0 && out.ticks_run() == 0,
					"paused start: a re-based paused frame banks only the time since the "
					"last render")) return 1;
			if (!expect(out.start_blank == (frame < 3),
					"paused start: the first three paused frames present black")) return 1;
		}
		const FrameOutcome after_start = session.advance(slow);
		if (!expect(after_start.perf.updates == 2 && after_start.ticks_run() == 0 &&
					!after_start.start_blank && session.state() == State::Paused,
				"paused start: after the start frames the paused frame banks its full time")) return 1;
	}

	// A joiner's mission start ends at its final wait for the S2C 0x0F, not at the load:
	// the update that leaves the wait ends the mode's initialize, so the frame drains no
	// further update, the main loop resets its bank, smoothing and phase, and its first frame,
	// whose render is that frame's, counts the first of the three re-based frames down: the
	// three frames after it bank only the time since a render, the fourth its full time. A
	// hitch frame there fast-forwards nothing (D-NET-862, V-ROLL-AW F2) [orig:
	// Game_StartMission @0x525E1F, the final wait @0x526295..0x5262E5;
	// Render_ProcessMainSceneFrame @0x5CAEFF..0x5CAF0E (the count-down @0x5CAF08);
	// Game_MainLoop @0x52B70F..0x52B757, @0x52B766].
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput wait;
		wait.delta_seconds = TickAccumulator::kTickDt * 10.0;
		wait.since_render_seconds = 0.0;
		// The probe reports no wait here, so these frames render and spend the load's re-based
		// frames (a joiner's wait frames render nothing and spend none: the render gate below);
		// either way the wait's end arms three afresh.
		for (int frame = 0; frame < 4; ++frame) (void)session.advance(wait);
		// The 0x0F lands in the first update of a 1 s frame: of the clamped bank's 32
		// updates, the others never run.
		FrameInput hitch;
		hitch.delta_seconds = 1.0;
		target.final_wait_end_after = target.inputs.size() + 1;
		if (!expect(session.advance(hitch).ticks_run() == 1,
				"final wait: the update that leaves it is the frame's last")) return 1;
		// No fast-forward: the three frames after it bank only the time since a render (here
		// none), against no smoothing history.
		FrameInput drawn;
		drawn.delta_seconds = 1.0;
		drawn.since_render_seconds = 0.0;
		for (int frame = 0; frame < 3; ++frame) {
			if (!expect(session.advance(drawn).ticks_run() == 0,
					"final wait: the frames after it bank no hitch")) return 1;
		}
		// The fourth banks its full 160 ms over the seed-only history: (7 * 16 + 2576 + 4) >> 3 =
		// 336 units, 5 quanta from phase 0, 2 ticks.
		FrameInput full;
		full.delta_seconds = TickAccumulator::kTickDt * 10.0;
		if (!expect(session.advance(full).ticks_run() == 2,
				"final wait: the main loop banks from a fresh history")) return 1;
	}

	// THE RENDER GATE (Session::renders). A joiner's final wait for the S2C 0x0F is a busy loop
	// inside the Game Loop mode's initialize that renders nothing; its passes are our frames'
	// updates, so a frame whose drain leaves the role inside the wait runs no render callback,
	// neither the scene nor the page (FrameOutcome::rendered and scene_drawn down), and spends
	// none of the load's three re-based frames; the frame whose drain takes the 0x0F renders. A
	// listen host's page frame renders, the page in the scene's place
	// [orig: Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5 (no
	//  GameLoop_RenderFrame); GameLoop_Init @0x526370 (the call @0x526377);
	//  Render_ProcessMainSceneFrame @0x5CAEFF..0x5CAF0E].
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		target.probe_kind = RoleKind::Joiner;
		target.waiting = true;
		Session session(target);
		if (!load(session)) return 1;
		if (!expect(!session.renders() && !session.draws_scene(nullptr),
				"render gate: the final wait renders nothing")) return 1;
		FrameInput wait;
		wait.delta_seconds = TickAccumulator::kTickDt * 10.0;
		wait.since_render_seconds = 0.0;
		const FrameOutcome first = session.advance(wait);
		if (!expect(first.status == FrameStatus::Ok && !first.rendered && !first.scene_drawn &&
					!first.start_blank,
				"render gate: a wait frame runs no render callback")) return 1;
		// The load's re-base is the first frame's alone: that frame rendered nothing, so the next
		// banks its full 160 ms over the seed-only history, its updates the wait's passes
		// ((7 * 16 + 2576 + 4) >> 3 = 336 units, 5 quanta from phase 0, 2 updates).
		const FrameOutcome second = session.advance(wait);
		if (!expect(second.ticks_run() == 2 && !second.rendered && !second.scene_drawn,
				"render gate: the wait spends none of the load's re-based frames")) return 1;
		target.final_wait_end_after = target.inputs.size() + 1;
		const FrameOutcome taken = session.advance(wait);
		if (!expect(taken.ticks_run() == 1 && taken.rendered && taken.scene_drawn &&
					session.renders(),
				"render gate: the frame whose drain takes the 0x0F renders")) return 1;
		// Its render is the first of the three past the start the wait's end armed: black.
		if (!expect(taken.start_blank,
				"render gate: the 0x0F frame's render presents black, the first of three")) return 1;
	}
	{
		opennova::hud::HudToggleState view_up;
		view_up.server_status_view = true;
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		target.probe_kind = RoleKind::ListenHost;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput paged;
		paged.delta_seconds = TickAccumulator::kTickDt;
		paged.hud_toggles = &view_up;
		const FrameOutcome out = session.advance(paged);
		if (!expect(out.rendered && !out.scene_drawn,
				"render gate: the status page renders, in the scene's place")) return 1;
	}

	// The status page stands in for the scene frame (Session::draws_scene): the session takes
	// the verdict after its drain, a dedicated host's frame always the page, a peer's while
	// the authority's view word is up, and runs the scene frame's own legs on it alone. Under
	// the page no start frame counts down (the next frame banks its full time); a session
	// peer's listener update, the render callback's first leg, still asks whether a cine
	// plays, which stops one past its last event; the frames drawn after it count the three
	// start frames down again
	// [orig: GameLoop_RenderFrame @0x521310 - `jz` @0x521355, the view word @0x521CD6,
	//  is_authority @0x521CDF..0x521CE6, Audio_UpdateListenerFromView @0x52131E ->
	//  Cine_IsPlaybackActive @0x43B40C; Render_ProcessMainSceneFrame @0x5CAEFF..0x5CAF0E
	//  (the re-base countdown), @0x5CADFC -> Cine_RenderOrEndPlayback @0x575A50].
	{
		opennova::hud::HudToggleState view_up;
		view_up.server_status_view = true;
		opennova::hud::HudToggleState view_down;
		{
			auto target_heap = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target_heap);
			TickProbe &target = *target_heap;
			target.probe_kind = RoleKind::ListenHost;
			Session session(target);
			if (!load(session)) return 1;
			// An SP end-of-round cine past its last event (an empty list ends at 0).
			target.kernel.world.epilog.active = true;
			target.kernel.world.epilog.frame = 0;
			FrameInput paged;
			paged.delta_seconds = TickAccumulator::kTickDt * 10.0;
			paged.since_render_seconds = 0.0;
			paged.hud_toggles = &view_up;
			const FrameOutcome first = session.advance(paged);
			if (!expect(!first.scene_drawn,
					"page: a listen host's view word stands the page in")) return 1;
			if (!expect(first.ticks_run() == 0,
					"page: the load's re-base still banks only the time since the render")) return 1;
			if (!expect(!target.kernel.world.epilog.active,
					"page: the listener update's cine test stops it under the page")) return 1;
			if (!expect(session.advance(paged).ticks_run() != 0,
					"page: no start frame counted down, so the next frame banks its time")) return 1;
			FrameInput drawn = paged;
			drawn.hud_toggles = &view_down;
			target.kernel.world.epilog.active = true;
			const FrameOutcome back = session.advance(drawn);
			if (!expect(back.scene_drawn && !back.world_skipped,
					"page: the view word down draws the scene and its world")) return 1;
			if (!expect(!target.kernel.world.epilog.active,
					"page: the drawn frame ends the cine past its end")) return 1;
		}
		{
			// The countdown waits for the frames drawn: after one page frame, the frames drawn
			// count the three start frames down from the first of them, so the fourth frame
			// drawn is still re-based (had the page frame counted one, it would bank).
			auto target_heap = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target_heap);
			TickProbe &target = *target_heap;
			target.probe_kind = RoleKind::ListenHost;
			Session session(target);
			if (!load(session)) return 1;
			FrameInput paged;
			paged.delta_seconds = 0.0;
			paged.since_render_seconds = 0.0;
			paged.hud_toggles = &view_up;
			(void)session.advance(paged);
			FrameInput idle = paged;
			idle.hud_toggles = &view_down;
			for (int frame = 0; frame < 3; ++frame) (void)session.advance(idle);
			FrameInput drawn;
			drawn.delta_seconds = TickAccumulator::kTickDt * 10.0;
			drawn.since_render_seconds = 0.0;
			drawn.hud_toggles = &view_down;
			if (!expect(session.advance(drawn).ticks_run() == 0,
					"page: the fourth frame drawn banks only the time since its render")) return 1;
			if (!expect(session.advance(drawn).ticks_run() != 0,
					"page: the countdown spent, the full frame time banks again")) return 1;
		}
		{
			auto target_heap = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target_heap);
			TickProbe &target = *target_heap;
			target.probe_kind = RoleKind::DedicatedHost;
			Session session(target);
			if (!load(session)) return 1;
			FrameInput frame;
			frame.delta_seconds = TickAccumulator::kTickDt;
			if (!expect(!session.advance(frame).scene_drawn,
					"page: a dedicated host's frame is always the page")) return 1;
			frame.hud_toggles = &view_down;
			if (!expect(!session.advance(frame).scene_drawn && !session.draws_scene(&view_down),
					"page: whatever its view word")) return 1;
		}
		{
			auto target_heap = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target_heap);
			TickProbe &target = *target_heap;
			target.probe_kind = RoleKind::Joiner;
			Session session(target);
			if (!load(session)) return 1;
			FrameInput frame;
			frame.delta_seconds = TickAccumulator::kTickDt;
			frame.hud_toggles = &view_up;
			if (!expect(session.advance(frame).scene_drawn,
					"page: a joiner is not the authority, it draws the scene")) return 1;
		}
	}

	// Pause and a manual step keep the bank and its smoothing history
	// (D-NET-1005): no pause writes g_TickPhase or g_FrameTimeSmoothedFp4
	// [orig: Game_MainLoop @0x52B630 (the mode init @0x52B715 / @0x52B71B, the EMA's
	// store @0x52B866, the drain's phase @0x52BA6B)]. Manual step stays local-only.
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
		Session session(target);
		if (!load(session)) return 1;
		FrameInput hitch;
		hitch.delta_seconds = 1.0;
		(void)session.advance(hitch);
		if (!expect(session.pause().applied(), "pause applies")) return 1;
		if (!expect(session.step_once().ticks_run() == 1,
				"paused local session can step once")) return 1;
		if (!expect(session.resume().applied(), "resume after the step")) return 1;
		// The stall drained its clamped 8000 units to 0 at phase 125; the 1 ms frame
		// fast-forwards through that history: (7 * 8000 + 16 + 4) >> 3 = 7002 units,
		// 109 quanta from phase 125, 27 ticks (a cold bank would drain nothing).
		FrameInput tiny;
		tiny.delta_seconds = 0.001;
		if (!expect(session.advance(tiny).ticks_run() == 27,
				"pause and step kept the stall's bank history")) return 1;
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
			int flip_at = -1;                              // the update whose pause row fires
			InputPacket sample_tick(uint32_t) override { return {}; }
			void run_update_rows(uint32_t logic_tick, bool new_sample) override {
				calls.emplace_back(logic_tick, new_sample);
			}
			// The pause row's record (catalog row 70, code 25), the drain's to flip.
			void queue_records(opennova::controls::InputEventQueue &records) override {
				if (static_cast<int>(calls.size()) - 1 == flip_at)
					(void)records.push({25, 0, 0, 0, false});
			}
		};
		auto target = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target);
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 8);
		opennova::world::Entity seed;
		seed.kind = opennova::world::EntityKind::Organic;
		seed.alive = true;
		world.cached.local_player = world.registry.spawn(0, seed);
		const int32_t weapon = 203; // Primary, 0x0C000801
		const int32_t stance = 169; // Crouch, 0x0C000C01
		const int32_t zoom = 361;   // radarout, 0x04000800
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
		// The pausing update shuts the audio channels down for the embedder's
		// audio device; the resuming update raises nothing [orig:
		// Input_HandleActionBinding @0x49AD40 - `cmp dword_A87050, 0; jz`
		// @0x49B543..0x49B54A, the Audio_ShutdownChannelsAndDeviceTable call @0x49B550].
		if (!expect(world.out.audio_channels_shutdown,
				"the pausing update requests the audio shutdown")) return 1;
		world.out.audio_channels_shutdown = false; // the embedder took it
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
				"the resuming update's rows read the device state the frame's head stepped, "
				"the next update steps its own")) return 1;
		if (!expect(world.hud_frames.main_frame - paused_counters.main_frame == 2 &&
				world.hud_frames.session_frame == paused_counters.session_frame,
				"the probe's ticks step no counters: the paused and the resuming head's "
				"paused steps alone")) return 1;
		if (!expect(!world.rules.pause_word && target->action_head_admits(weapon) &&
				target->action_head_admits(stance),
				"the resume admits the rows again")) return 1;
		if (!expect(!world.out.audio_channels_shutdown,
				"the resume shuts no audio down")) return 1;
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

	// Every record an update's drain reads dispatches, the ones behind a pause row
	// too: the keyboard pass has moved the whole key ring into the event queue before
	// the drain runs, so case 25's ring reset drops nothing, and each pause row's record
	// flips the word at once (Role::dispatch_record). A press, release and press of the
	// pause key inside one update pauses and resumes there: the records between meet the
	// head with the word set (the weapon select drops, the radar zoom runs) and the update
	// runs its tick unpaused. Each flip that leaves the word set shuts the audio channels
	// down. In a session the row's case returns before its flip, so the head's word stays
	// down.
	// [orig: Input_ProcessPlayerFrame @0x49D4C0 (the Input_ProcessKeyboardEvents
	// call @0x49D4C6); Input_ProcessFrame @0x49D520 (the drain @0x49D599..0x49D5E7);
	// Input_HandleActionBinding @0x49AD40 (case 25 @0x49B520: the in-session return
	// @0x49B527, `xor dword_A87050, 1` @0x49B52D, the ring reset's call @0x49B53E, the
	// word's test @0x49B543..0x49B54A ahead of the audio stop @0x49B550; the pause gate
	// @0x49ADDD..0x49ADE8); Input_ResetKeyQueue @0x760E00 (@0x760E02 / @0x760E07)]
	for (const RoleKind kind : {RoleKind::SinglePlayer, RoleKind::ListenHost}) {
		struct Drain final : TickInputSource {
			Role *role = nullptr;
			int32_t weapon = 0;
			int32_t zoom = 0;
			int calls = 0;
			bool weapon_between = true; // the weapon select's head between the presses
			bool zoom_between = false;  // the radar zoom's
			InputPacket sample_tick(uint32_t) override { return {}; }
			// The first update's queue: the pause row, the radar zoom, the pause row.
			void queue_records(opennova::controls::InputEventQueue &records) override {
				if (calls++ != 0) return;
				const opennova::controls::InputEventRecord pause{25,
						0, 0, 0, false};
				(void)records.push(pause);
				(void)records.push({zoom, 0, 0, 0, false});
				(void)records.push(pause);
			}
			// The radar zoom's record reaches the source between the flips.
			void dispatch_record(const opennova::controls::InputEventRecord &record) override {
				if (record.code != zoom) return;
				weapon_between = role->action_head_admits(weapon);
				zoom_between = role->action_head_admits(zoom);
			}
		};
		auto target = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target);
		target->probe_kind = kind;
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 8);
		opennova::world::Entity seed;
		seed.kind = opennova::world::EntityKind::Organic;
		seed.alive = true;
		world.cached.local_player = world.registry.spawn(0, seed);
		Drain drain;
		drain.role = target.get();
		drain.weapon = 203; // Primary, 0x0C000801
		drain.zoom = 361;   // radarout, 0x04000800
		FrameInput frame;
		frame.delta_seconds = TickAccumulator::kTickDt * 3.0;
		frame.tick_source = &drain;
		const FrameOutcome out = session.advance(frame);
		const bool single = kind == RoleKind::SinglePlayer;
		if (!expect(out.status == FrameStatus::Ok && out.ticks_run() == 3 &&
				session.state() == State::Running && !world.rules.pause_word,
				"a press, release and press of the pause row in one update leave it "
				"running")) return 1;
		if (!expect(drain.weapon_between == !single && drain.zoom_between,
				single ? "between the presses the head drops the weapon select and runs "
						"the radar zoom"
						: "in a session the head's word stays down")) return 1;
		// The first flip left the word set, so its case shut the audio down; in a
		// session the case returns before both.
		if (!expect(world.out.audio_channels_shutdown == single,
				"the flip that sets the word shuts the audio down")) return 1;
		if (!single) continue;
		// Under the pause the same queue lowers the word and raises it again: the
		// update stays paused, and the second flip shuts the audio down.
		world.out.audio_channels_shutdown = false;
		if (!expect(session.pause().applied(), "the pause")) return 1;
		drain.calls = 0;
		const FrameOutcome held = session.advance(frame);
		if (!expect(held.ticks_run() == 0 && session.state() == State::Paused &&
				world.rules.pause_word && world.out.audio_channels_shutdown,
				"a paused update's release and press stay paused and stop the audio")) return 1;
	}

	// A paused frame runs no tick but still runs the weather tick: retail's main
	// frame calls Environment_UpdateWeatherTick ahead of its pause test, so the
	// time of day keeps advancing (one advance per banked frame) and the
	// 310-tick minute counter keeps counting down through the pause.
	// [orig: Game_ProcessMainFrame @0x5263F0 -- Environment_UpdateWeatherTick
	//  @0x57E9B0 called @0x526774, the pause test @0x526779; the clock
	//  @0x57E9C7, the tickdown @0x57E9DA]
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
	}

	// Joiners expose their pre-load connection state.
	{
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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
		auto target_heap = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target_heap);
		TickProbe &target = *target_heap;
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

	// The drawn scene frame's world-skip word: a playing cine, or the command map as the open
	// menu screen. The frame's HUD pass (the radar step: here the incoming-lock tone it
	// re-registers) does not run, and the word reaches the local view, whose facial and
	// Inset passes it stops (world::FacialSystem::render_scene_passes,
	// world::LocalPlayerViewFrame::inset_scope_active). A cine past its last event stops at
	// the head's test and skips nothing.
	// [orig: Render_ProcessMainSceneFrame - ebx @0x5CA171..0x5CA190 (Cine_IsPlaybackActive
	//  @0x575960; UI_IsMenuScreenOpen && g_CmapScreenOpen), the skips @0x5CA4EA..0x5CA4EC and
	//  @0x5CA620..0x5CA640 past HUD_RenderAllOverlays @0x5CAD04; the tone's re-register
	//  HUD_RenderAllOverlays @0x5A8185..0x5A81BF]
	{
		auto target = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target);
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 4);
		opennova::world::Entity seed;
		seed.alive = true;
		seed.health = 100;
		const opennova::world::EntityHandle local = world.registry.spawn(0, seed);
		world.cached.local_player = local;
		world.ai.attach(local);
		target->kernel.local.radar.lock_tone = opennova::world::kRadarLockToneTicks;
		FrameInput frame;
		frame.delta_seconds = 0.0;
		frame.since_render_seconds = 0.0;
		frame.command_map_open = true;
		const FrameOutcome mapped = session.advance(frame);
		if (!expect(mapped.scene_drawn && mapped.world_skipped &&
				target->kernel.local.view.world_skipped,
				"world skip: the command map over the menu skips the world")) return 1;
		if (!expect(world.out.sound_emitters.empty() && !mapped.hud_pass_called,
				"world skip: no HUD pass re-registers the tone")) return 1;
		frame.command_map_open = false;
		const FrameOutcome drawn = session.advance(frame);
		if (!expect(!drawn.world_skipped && !target->kernel.local.view.world_skipped &&
				world.out.sound_emitters.size() == 1 && drawn.hud_pass_called,
				"world skip: the world drawn, the HUD pass re-registers the tone")) return 1;
		// The F1 help shuts the call site in camera mode 0: the world draws, the HUD pass
		// (its tone, the embedder's overlay pass on hud_pass_called) does not [orig:
		// Render_ProcessMainSceneFrame - `cmp g_HelpScreenOpen, 0` @0x5CACF7, the call @0x5CAD04].
		opennova::hud::HudToggleState help;
		help.help_open = true;
		frame.hud_toggles = &help;
		const FrameOutcome helped = session.advance(frame);
		if (!expect(helped.scene_drawn && !helped.world_skipped && !helped.hud_pass_called &&
				world.out.sound_emitters.size() == 1,
				"call site: the F1 help calls no HUD pass")) return 1;
		frame.hud_toggles = nullptr;
		opennova::world::CineEvent event;
		event.duration = 50;
		world.epilog.events.push_back(event);
		world.epilog.active = true;
		world.epilog.frame = 0;
		const FrameOutcome cine = session.advance(frame);
		if (!expect(cine.world_skipped && world.epilog.active &&
				world.out.sound_emitters.size() == 1,
				"world skip: a cine before its last event skips the world")) return 1;
		world.epilog.frame = 50;
		const FrameOutcome ended = session.advance(frame);
		if (!expect(!ended.world_skipped && !world.epilog.active,
				"world skip: a cine past its last event stops at the head")) return 1;
	}

	// The world block's map overlay clear: a drawn frame that draws its world closes an open
	// map for a body with the dead bit. A live body keeps it; a frame that skips its world
	// and the listen host's status page run no world block, so the updates they run keep it
	// too, until the next frame that draws the world.
	// [orig: Render_ProcessMainSceneFrame - the world-skip jump @0x5CA620..0x5CA640,
	//  `cmp g_MapOverlayMode, 0` @0x5CAC50, `test byte ptr [ecx+24h], 2` @0x5CAC63, the clear
	//  @0x5CAC6D; GameLoop_RenderFrame's view word @0x521CD6]
	{
		auto target = std::make_unique<TickProbe>();
		test_titles::jo_controls(*target);
		target->probe_kind = RoleKind::ListenHost;
		Session session(*target);
		if (!load(session)) return 1;
		opennova::world::World &world = target->kernel.world;
		world.registry.configure_pool(0, 4);
		opennova::world::Entity seed;
		seed.alive = true;
		seed.health = 100;
		const opennova::world::EntityHandle local = world.registry.spawn(0, seed);
		world.cached.local_player = local;
		opennova::hud::HudMapControl &map = target->kernel.local.hud_map_control;
		map.mode = 2;
		opennova::hud::HudToggleState page;
		page.server_status_view = true;
		FrameInput frame; // no main-loop clock sample: every frame banks its delta
		frame.delta_seconds = 0.0;
		(void)session.advance(frame);
		if (!expect(map.mode == 2, "map clear: a live body keeps the map")) return 1;
		world.registry.get(local)->flags |= opennova::world::kEntityFlagDead;
		frame.command_map_open = true;
		// Six ticks' time a frame: the bank smooths each frame's time with 7/8 of the history
		// the idle frame left, and only every fourth 4 ms quantum runs an update, so three
		// ticks' time banks one update on this frame and none on the next (the status page's)
		// [orig: Game_MainLoop @0x52B630 - the EMA @0x52B83C..0x52B866, the drain
		//  @0x52BA08..0x52BA6B].
		frame.delta_seconds = TickAccumulator::kTickDt * 6.0;
		const FrameOutcome skipped = session.advance(frame);
		if (!expect(skipped.world_skipped && skipped.ticks_run() > 0 && map.mode == 2,
				"map clear: a frame that skips its world keeps it, its updates too")) return 1;
		frame.command_map_open = false;
		frame.hud_toggles = &page;
		const FrameOutcome paged = session.advance(frame);
		if (!expect(!paged.scene_drawn && paged.ticks_run() > 0 && map.mode == 2,
				"map clear: the status page keeps it")) return 1;
		frame.hud_toggles = nullptr;
		frame.delta_seconds = 0.0;
		const FrameOutcome drawn = session.advance(frame);
		if (!expect(drawn.scene_drawn && !drawn.world_skipped && map.mode == 0,
				"map clear: the frame that draws the world closes the dead body's map")) return 1;
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
		test_titles::jo_controls(*target);
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
		test_titles::jo_controls(*target);
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
		test_titles::jo_controls(*target);
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
	// (D-NET-1003; world::TickAccumulator carries the witness).
	// [orig: Game_MainLoop @0x52B630 -- the elapsed @0x52B798..0x52B7A4]
	{
		const auto catchup = [](double long_frame) {
			auto target = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target);
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

	// The lose cine's edit fade scales the bank's smoothing history from inside an
	// update; the session applies each scale after the tick that raised it, before
	// any later tick of the frame and before the next frame's bank, and the frame
	// that raised it keeps the ticks its bank already counted. A 16 ms stream's
	// 272-unit history x1.66f is 451, and the next 16 ms frame smooths to 429
	// units, two ticks where the unscaled stream runs one. A 48 ms frame's three
	// ticks stand when the second raises the scale; its 784-unit history x1.66f is
	// 1301, and the next 16 ms frame smooths to 1172 units, 18 quanta from phase 12,
	// five ticks where the unscaled stream's 720 units run three
	// (world::TickAccumulator::scale_smoothing_history carries the witness).
	// [orig: Game_ScaleSmoothedFrameTime @0x52B5D0, called @0x5726C9 / @0x572775
	//  inside the update (Game_ProcessMainFrame @0x5263F0 -> Entity_UpdateAllEntities
	//  @0x52674B -> Cinematic_EpilogUpdate @0x4C2230); Game_MainLoop's EMA read
	//  @0x52B845, the word's one reader]
	{
		const auto next_frame = [](double first_seconds, bool scaled, size_t raise_after,
				int32_t first_ticks) -> int {
			auto target = std::make_unique<TickProbe>();
			test_titles::jo_controls(*target);
			Session session(*target);
			if (!load(session)) return -1;
			target->raise_after_ticks = raise_after;
			if (scaled) target->scales_to_raise = {1.66f};
			FrameInput first;
			first.delta_seconds = first_seconds;
			if (session.advance(first).ticks_run() != first_ticks) return -2;
			if (!target->kernel.world.out.frame_time_scales.empty()) return -3;
			FrameInput in;
			in.delta_seconds = TickAccumulator::kTickDt;
			return static_cast<int>(session.advance(in).ticks_run());
		};
		const double one = TickAccumulator::kTickDt;
		const double three = TickAccumulator::kTickDt * 3.0;
		if (!expect(next_frame(one, false, 0, 1) == 1 && next_frame(one, true, 0, 1) == 2,
				"the scaled history banks the next frame's extra tick")) return 1;
		if (!expect(next_frame(three, false, 1, 3) == 3 && next_frame(three, true, 1, 3) == 5,
				"a mid-frame scale leaves its frame's ticks and reaches the next bank")) return 1;
	}

	std::printf("OK: in-match session lifecycle/cadence/input/failure\n");
	return 0;
}
