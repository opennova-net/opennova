// Simulation — the F3 dev tools' native reads (ADR 0042 d6): each forwards
// the ONE engine function behind a DevTools window or overlay record, with
// no Variant round-trip. C++-only; DevTools holds the Simulation natively.
#include "simulation/simulation_internal.h"

#include <runtime/world/inspect_markers.h>

bool Simulation::native_ai_debug(
		opennova::world::inspect::AiDebugReport &r_out) const {
	// The joiner's tooling AI pool is non-authoritative and never joins the
	// decoded view (the directory's rule), so a joiner reports nothing.
	if (!kernel_ || is_joiner()) return false;
	r_out = opennova::world::inspect::ai_debug_report(kernel_->world);
	return true;
}

bool Simulation::native_entity_markers(const opennova::world::inspect::EntityMarkerQuery &p_query,
		std::vector<opennova::world::inspect::EntityMarker> &r_out) const {
	r_out.clear();
	if (!kernel_) return false;
	opennova::world::inspect::EntityMarkerQuery query = p_query;
	// A joiner's tooling AI pool never joins the decoded view.
	query.with_brains = query.with_brains && !is_joiner();
	r_out = opennova::world::inspect::entity_markers(kernel_->world, query);
	return true;
}
