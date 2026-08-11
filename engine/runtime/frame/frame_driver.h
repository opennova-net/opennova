// The game frame: the embedder-side loop the shell used to hand-mirror in
// GDScript, owned by the engine per ADR 0033 (R1). One driver runs the
// original main loop's shape — bank wall-clock, drain it in fixed 62.5 Hz
// quanta (the catch-up batch), present ONCE after the batch — and then the
// per-frame legs in one fixed order. The embedding shell supplies the legs as
// hooks (the device side: node writes, GPU uploads, audio players); every
// DECISION and the ORDER live here.
// [orig: Game_MainLoop @ 0x52b630 — the 1/16-ms bank, the 4-ms drain quanta,
//  the ~31-tick clamp, and render-once-per-outer-iteration;
//  Game_ProcessMainFrame @ 0x5263f0 is one drained tick]
#ifndef OPENNOVA_FRAME_FRAME_DRIVER_H
#define OPENNOVA_FRAME_FRAME_DRIVER_H

#include <world/tick_accumulator.h>

#include <cstdint>
#include <functional>

namespace opennova::frame {

// The per-frame device legs, in the exact slot order run_frame invokes them.
// Any leg may be empty (a role that lacks it — a dedicated serve-mode
// session has no listener, ONED previews no foliage). The legs are sampled by reference per
// call; the shell installs them once at mission setup.
struct FrameHooks {
	// The terrain frame compile+apply for this camera — first, before the tick
	// batch: its draw list's detail-cell handoff is the foliage leg's input, so
	// the two run in producer order against the same pre-tick camera (ADR 0033
	// R2; the self-driven _process walk this replaces ran at arbitrary Godot
	// scheduling relative to the foliage leg).
	std::function<void()> terrain;
	// Foliage dispatch for this camera — before the tick batch (the shell's
	// witnessed order: the dispatcher renders against the pre-tick camera).
	std::function<void()> foliage;
	// The camera listener stamp — before the batch, so fires gate their
	// propagation delay against the current camera (the retail frame order:
	// the listener global updates before entity/fire processing)
	// [orig: listener_pos @ 0x24D6630; the @ 0x528e57 no-listener peer gate].
	std::function<void()> stamp_listener;
	// One authoritative 62.5 Hz logic step. Returns true when the tick ran
	// (the sim's own gates may decline). REQUIRED.
	std::function<bool()> step;
	// The world's logic-tick counter, read after a step that ran (feeds the
	// two tick-stamped legs below). REQUIRED when either of those is set.
	std::function<int32_t()> logic_tick;
	// The four per-tick follow legs, invoked only after a step that ran, in
	// this order: the effect-pose snapshot rebind, the throwable fixed-tick
	// reconcile (round-bound ammo groups move on the owning round tick), the
	// side-effect drain+emit, and the fixed-tick completion broadcast
	// (presentation-only fixed-step systems advance here, never on render
	// delta).
	std::function<void(int32_t logic_tick)> begin_effect_tick;
	std::function<void()> sync_fixed_effects;
	std::function<void()> drain_effects;
	std::function<void(int32_t logic_tick)> fixed_tick_completed;
	// Present the entity rows alone (the zero-tick render frame still
	// re-evaluates submission: camera mode and local attach/detach change
	// between fixed ticks) and the full present ladder (rows + the tick-driven
	// passes) after a batch that ran.
	std::function<void()> present_rows;
	std::function<void()> present_frame;
	// The local-player VIEW presentation — camera + viewmodel placement from
	// the just-simulated player state. Every render frame, closing the present
	// ladder, so the post-batch occlusion/iris legs read the same view the
	// imminent render uses (D-RORD-8) [orig: the render frame builds its view
	// from the current player state before collect+submit,
	// Render_ProcessMainSceneFrame @ 0x5ca0f0].
	std::function<void()> present_local_view;
	// The post-batch frame legs, in order: session-drive observation, the
	// world-driven weather tick, the occlusion blink gates (only when at
	// least one logic tick ran), the render-occlusion frame, the iris
	// exposure samples, and the audio pass.
	std::function<void()> net_drive;
	std::function<void()> weather;
	std::function<void()> blink_gates;
	std::function<void()> occlusion_frame;
	std::function<void()> iris_samples;
	std::function<void(int32_t ticks_run)> audio;
};

// Wall-clock perf spans for the last frame (microsecond stamps are supplied
// by the embedder's clock hook so the driver stays deterministic in tests).
struct FramePerf {
	int64_t tick_us = 0;     // whole batch incl. present
	int64_t sim_us = 0;      // summed step() time
	int64_t effects_us = 0;  // summed drain time
	int64_t present_us = 0;  // the present ladder
	bool did_tick = false;
	int32_t ticks = 0;
};

class FrameDriver {
public:
	// The realtime embedder entry [orig: Game_MainLoop @ 0x52b630]: bank `dt`
	// through the accumulator, run 0..kMaxCatchupTicks steps, then present —
	// the full ladder after a batch that ran, the rows alone on a zero-tick
	// frame — and then the post-batch legs in slot order. Returns the number
	// of logic ticks run.
	int32_t run_frame(double dt, const FrameHooks &hooks);

	// The deterministic single-step primitive (debug Step / isolated tests):
	// listener stamp, exactly one step attempt, the per-tick legs when it ran,
	// then the full present ladder. No accumulator interaction. Returns
	// whether the tick ran. The post-batch frame legs do NOT run here — the
	// realtime entry owns them.
	bool run_single(const FrameHooks &hooks);

	// Discard banked wall-clock (Play after pause/load must not burst).
	void reset_bank() { accum_.reset(); }

	world::TickAccumulator &accumulator() { return accum_; }
	const FramePerf &perf() const { return perf_; }

	// The embedder's monotonic clock (microseconds). Defaults to a null clock
	// (all perf spans zero) so tests stay deterministic.
	void set_clock(std::function<int64_t()> clock) { clock_ = std::move(clock); }

private:
	int64_t now_us() const { return clock_ ? clock_() : 0; }
	// One drained tick + its per-tick legs [orig: Game_ProcessMainFrame
	// @ 0x5263f0]. Fills sim/effects spans; returns whether the step ran.
	bool advance_one(const FrameHooks &hooks, int32_t &r_logic_tick);

	world::TickAccumulator accum_;
	FramePerf perf_;
	std::function<int64_t()> clock_;
	int32_t last_logic_tick_ = 0;
};

} // namespace opennova::frame

#endif // OPENNOVA_FRAME_FRAME_DRIVER_H
