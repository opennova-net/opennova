// The Simulation's runtime profiling state (ADR 0043 d9): the one opt-in gate
// for every native timer and counter, the session frame's step and sink spans,
// the last tick's sim and net times and projectile traces, and the dev tools'
// frame-stats board the frame folds them onto. Plain data with no behavior.
#pragma once

#include <cstdint>

#include <runtime/world/collision.h> // CollisionWorld::TraceProfile

#include "devtools/frame_stats.h"

namespace godot {

struct SimulationProfilingState {
	int64_t frame_sim_us = 0;
	int64_t frame_sink_us = 0;
	// The dev tools' frame-stats board (ADR 0039): fold_frame_stats() lands the
	// kernel's tick profile (every touched SIM_* slot, ADR 0043 d5) plus the
	// shell-side step/sink spans on it natively at the end of a session frame.
	// Ordinary play leaves runtime_profiling_enabled false, so the profile is
	// inactive and no producer reads a clock.
	Ref<FrameStats> frame_stats;
	uint64_t last_sim_tick_us = 0;
	uint64_t last_net_tick_us = 0;
	// One opt-in gate for every native runtime timer/counter. Retail play keeps
	// this false; F3 Stats and the manual probe share the public ownership seam.
	bool runtime_profiling_enabled = false;
	// The last tick's projectile traces (take_projectile_trace_profile).
	opennova::world::CollisionWorld::TraceProfile last_trace_profile;
};

} // namespace godot
