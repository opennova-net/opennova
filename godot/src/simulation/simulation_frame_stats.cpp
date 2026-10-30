#include "simulation/simulation.h"

namespace godot {

// The session phase attribution folded straight onto the frame-stats board
// (ADR 0039): the kernel's ONE tick profile (ADR 0043 d5) carries every
// SIM_* span and count the engine's tick legs touched this render frame; the
// fold adds each touched slot once, so a role that never ran a phase never
// samples its row, exactly as the former per-role field lists did.

void Simulation::set_frame_stats(const Ref<FrameStats> &p_stats) {
	profiling_.frame_stats = p_stats;
}

Ref<FrameStats> Simulation::get_frame_stats() const {
	return profiling_.frame_stats;
}

void Simulation::fold_frame_stats(const opennova::inmatch::FrameOutcome &p_outcome) {
	if (!profiling_.runtime_profiling_enabled || !profiling_.frame_stats.is_valid() ||
			p_outcome.ticks_run() <= 0) {
		return;
	}
	FrameStats &stats = **profiling_.frame_stats;
	if (!stats.is_capture_active()) {
		return;
	}
	stats.add(FrameStats::SIM_STEP, profiling_.frame_sim_us);
	stats.add(FrameStats::SIM_SINK, profiling_.frame_sink_us);
	if (kernel_ == nullptr) return;
	kernel_->profile.for_each_touched(
			[&stats](opennova::devtools::Slot p_slot, int64_t p_sum) {
				stats.add(static_cast<int>(p_slot), p_sum);
			});
}

// Every captured frame, as MissionRoot's SIM_ADVANCE is, so the phases and their parent cover
// the same frames (a frame of no tick banks and runs its legs too).
void Simulation::fold_frame_phases(
		const opennova::inmatch::FramePerf &p_perf, int64_t p_marshal_us) {
	if (!profiling_.runtime_profiling_enabled || !profiling_.frame_stats.is_valid()) return;
	FrameStats &stats = **profiling_.frame_stats;
	if (!stats.is_capture_active()) return;
	stats.add(FrameStats::SIM_FRAME_BANK, p_perf.bank_us);
	stats.add(FrameStats::SIM_FRAME_UPDATES, p_perf.tick_us);
	stats.add(FrameStats::SIM_FRAME_INPUT_ROWS, p_perf.input_rows_us);
	stats.add(FrameStats::SIM_FRAME_INPUT_DRAIN, p_perf.input_drain_us);
	stats.add(FrameStats::SIM_FRAME_INPUT_APPLY, p_perf.input_apply_us);
	stats.add(FrameStats::SIM_FRAME_RENDER_LEGS, p_perf.render_legs_us);
	stats.add(FrameStats::SIM_FRAME_MARSHAL, p_marshal_us);
	stats.add(FrameStats::SIM_FRAME_LOCK_SLEEPS, p_perf.lock_sleeps);
}

} // namespace godot
