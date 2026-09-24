// Simulation — the F3 dev tools' native reads (ADR 0042 d6): each forwards
// the ONE engine function behind a DevTools window or overlay record, with
// no Variant round-trip. C++-only; DevTools holds the Simulation natively.
#include "simulation/simulation_internal.h"

#include <runtime/mission/debug_oracles.h>
#include <runtime/mission/script_debug_report.h>
#include <runtime/world/inspect_local_player.h>
#include <runtime/world/collision_debug_rows.h>
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

bool Simulation::native_ray_debug_rows(std::vector<opennova::world::RayDebugRow> &r_rows,
		int32_t &r_ttl) const {
	r_rows.clear();
	if (!kernel_) return false;
	r_ttl = kernel_->collision.ray_debug_ttl_ticks();
	opennova::world::ray_debug_rows(kernel_->collision, kernel_->world.logic_tick, r_rows);
	return true;
}

bool Simulation::native_contact_debug_rows(std::vector<opennova::world::ContactDebugRow> &r_rows,
		int32_t &r_ttl) const {
	r_rows.clear();
	if (!kernel_) return false;
	r_ttl = opennova::world::CollisionWorld::kContactDebugTtlTicks;
	opennova::world::contact_debug_rows(kernel_->world, kernel_->collision, kernel_->world.logic_tick, r_rows);
	return true;
}

bool Simulation::native_script_report(opennova::mission::ScriptDebugReport &r_out) const {
	if (!kernel_) return false;
	r_out = opennova::mission::script_debug_report(*kernel_);
	return true;
}

bool Simulation::native_local_player_report(opennova::world::inspect::LocalPlayerReport &r_out) const {
	if (!kernel_) {
		r_out = opennova::world::inspect::LocalPlayerReport{};
		return false;
	}
	return opennova::world::inspect::local_player_report(kernel_->world, kernel_->local, &kernel_->collision,
			r_out);
}

bool Simulation::native_hitbox_debug(const opennova::world::Vec3 &p_anchor,
		opennova::mission::DebugHitboxReport &r_out) {
	if (!kernel_) return false;
	opennova::mission::DebugHitboxBudget budget;
	budget.has_anchor = true;
	budget.anchor = p_anchor;
	// The overlay draws every face it gets; a smaller face budget keeps the
	// 6 Hz refresh and the draw list light.
	budget.face_cap = 4000;
	opennova::mission::collect_debug_hitboxes(*kernel_, r_out, budget);
	return true;
}
