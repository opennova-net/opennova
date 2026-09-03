#include "simulation/simulation.h"

namespace godot {

// The session phase attribution folded straight onto the frame-stats board
// (ADR 0039): the kernel's ONE tick profile (ADR 0043 d5) carries every
// SIM_* span and count the engine's tick legs touched this render frame; the
// fold adds each touched slot once, so a role that never ran a phase never
// samples its row, exactly as the former per-role field lists did.

void Simulation::set_frame_stats(const Ref<FrameStats> &p_stats) {
	frame_stats_ = p_stats;
}

Ref<FrameStats> Simulation::get_frame_stats() const {
	return frame_stats_;
}

void Simulation::fold_frame_stats(const opennova::inmatch::FrameOutcome &p_outcome) {
	if (!runtime_profiling_enabled_ || !frame_stats_.is_valid() || p_outcome.ticks_run() <= 0) {
		return;
	}
	FrameStats &stats = **frame_stats_;
	if (!stats.is_capture_active()) {
		return;
	}
	stats.add(FrameStats::SIM_STEP, frame_sim_us_);
	stats.add(FrameStats::SIM_SINK, frame_sink_us_);
	if (kernel_ == nullptr) return;
	kernel_->profile.for_each_touched(
			[&stats](opennova::devtools::Slot p_slot, int64_t p_sum) {
				stats.add(static_cast<int>(p_slot), p_sum);
			});
}

} // namespace godot
