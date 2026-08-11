#include "frame/frame_driver.h"

namespace opennova::frame {

bool FrameDriver::advance_one(const FrameHooks &hooks, int32_t &r_logic_tick) {
	const int64_t sim_start = now_us();
	const bool did_tick = hooks.step ? hooks.step() : false;
	perf_.sim_us += now_us() - sim_start;
	if (!did_tick) {
		return false;
	}
	r_logic_tick = hooks.logic_tick ? hooks.logic_tick() : 0;
	// Invalidate before delivering effects: any owned spawn seeded during this
	// tick and the fixed-tick particle advance both observe this exact
	// client-view pose, even inside a multi-tick catch-up batch.
	if (hooks.begin_effect_tick) {
		hooks.begin_effect_tick(r_logic_tick);
	}
	// Round-bound ammo move groups are particle-simulation state; reconcile
	// them before the fixed-tick consumer advances the effect world so birth,
	// motion, and release all happen on the exact owning round tick.
	if (hooks.sync_fixed_effects) {
		hooks.sync_fixed_effects();
	}
	const int64_t effects_start = now_us();
	if (hooks.drain_effects) {
		hooks.drain_effects();
	}
	perf_.effects_us += now_us() - effects_start;
	if (hooks.fixed_tick_completed) {
		hooks.fixed_tick_completed(r_logic_tick);
	}
	return true;
}

int32_t FrameDriver::run_frame(double dt, const FrameHooks &hooks) {
	const int64_t frame_start = now_us();
	perf_ = FramePerf{};
	// Terrain compiles its patch packet first (the foliage leg consumes its
	// detail-cell handoff), foliage dispatches against the same pre-tick
	// camera, then the listener stamps before the batch so this batch's fires
	// gate their propagation delay against the current camera [orig: the
	// listener global updates before the entity/fire processing; @ 0x528e57].
	if (hooks.terrain) {
		hooks.terrain();
	}
	if (hooks.foliage) {
		hooks.foliage();
	}
	if (hooks.stamp_listener) {
		hooks.stamp_listener();
	}
	// The bank/clamp arithmetic [orig: Game_MainLoop @ 0x52b630 — 16 ms
	// quanta, the ~31-tick catch-up clamp].
	const int32_t n = accum_.bank(dt);
	int32_t ran = 0;
	if (n <= 0) {
		// Retail evaluates entity submission every render frame: camera mode
		// and local attach/detach change between fixed ticks, so the row
		// present must not wait for the next 62.5 Hz quantum. The tick-driven
		// passes stay tick-driven — no gameplay state advanced here.
		if (hooks.present_rows) {
			const int64_t present_start = now_us();
			hooks.present_rows();
			perf_.present_us = now_us() - present_start;
		}
	} else {
		int32_t logic_tick = last_logic_tick_;
		for (int32_t i = 0; i < n; ++i) {
			if (advance_one(hooks, logic_tick)) {
				++ran;
			}
		}
		last_logic_tick_ = logic_tick;
		// Present ONCE after the whole catch-up batch (the original renders
		// once per outer iteration, however many ticks drained).
		if (hooks.present_frame) {
			const int64_t present_start = now_us();
			hooks.present_frame();
			perf_.present_us = now_us() - present_start;
		}
	}
	// The local-view present closes the ladder on EVERY render frame (the
	// camera changes between fixed ticks exactly like the entity rows), so
	// the occlusion/iris legs below read the view the imminent render uses
	// (D-RORD-8) [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0 builds the
	// view before collect+submit].
	if (hooks.present_local_view) {
		const int64_t view_start = now_us();
		hooks.present_local_view();
		perf_.present_us += now_us() - view_start;
	}
	// The post-batch frame legs, in the one fixed order the shell used to
	// hand-sequence: session drive, world-driven weather, the blink gates
	// (only after a batch that ran), the render-occlusion frame, the iris
	// samples, the audio pass.
	if (hooks.net_drive) {
		hooks.net_drive();
	}
	if (hooks.weather) {
		hooks.weather();
	}
	if (ran > 0 && hooks.blink_gates) {
		hooks.blink_gates();
	}
	if (hooks.occlusion_frame) {
		hooks.occlusion_frame();
	}
	if (hooks.iris_samples) {
		hooks.iris_samples();
	}
	if (hooks.audio) {
		hooks.audio(ran);
	}
	perf_.did_tick = ran > 0;
	perf_.ticks = ran;
	perf_.tick_us = now_us() - frame_start;
	return ran;
}

bool FrameDriver::run_single(const FrameHooks &hooks) {
	const int64_t frame_start = now_us();
	perf_ = FramePerf{};
	if (hooks.stamp_listener) {
		hooks.stamp_listener();
	}
	int32_t logic_tick = last_logic_tick_;
	const bool did_tick = advance_one(hooks, logic_tick);
	if (did_tick) {
		last_logic_tick_ = logic_tick;
		if (hooks.present_frame) {
			const int64_t present_start = now_us();
			hooks.present_frame();
			perf_.present_us = now_us() - present_start;
		}
		if (hooks.present_local_view) {
			const int64_t view_start = now_us();
			hooks.present_local_view();
			perf_.present_us += now_us() - view_start;
		}
	}
	perf_.did_tick = did_tick;
	perf_.ticks = did_tick ? 1 : 0;
	perf_.tick_us = now_us() - frame_start;
	return did_tick;
}

} // namespace opennova::frame
