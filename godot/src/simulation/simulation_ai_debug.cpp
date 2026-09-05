// Simulation — the AI debug binding leg (ADR 0042 d6): the native join push
// for the F3 AI window (DevTools), reading the ONE engine join,
// world::inspect::ai_debug_report.
#include "simulation/simulation_internal.h"

bool Simulation::native_ai_debug(
		opennova::world::inspect::AiDebugReport &r_out) const {
	// The joiner's tooling AI pool is non-authoritative and never joins the
	// decoded view (the directory's rule), so a joiner reports nothing.
	if (!kernel_ || is_joiner()) return false;
	r_out = opennova::world::inspect::ai_debug_report(kernel_->world);
	return true;
}
