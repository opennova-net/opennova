// Simulation — the F3 dev tools' native reads (ADR 0042 d6): each forwards
// the ONE engine function behind a DevTools window or overlay record, with
// no Variant round-trip. C++-only; DevTools holds the Simulation natively.
#include "simulation/simulation_internal.h"

#include <runtime/mission/debug_oracles.h>
#include <runtime/mission/script_debug_report.h>
#include <runtime/world/inspect_local_player.h>
#include <runtime/world/collision_debug_rows.h>
#include <runtime/world/inspect_markers.h>

#include "simulation/body_trace_row.h" // one body readout record

#include <algorithm>
#include <cstdint>

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

// game_debug's fire_event: the engine's request (BmsEventSystem::force_fire), refused off the authority (a
// joiner runs no BMS events) and with the engine's words for a refused one.
Error Simulation::debug_fire_event(int p_event, String &r_reason) {
	r_reason = String();
	if (!kernel_ || is_joiner()) return ERR_UNAVAILABLE;
	const opennova::mission::FireRequest request =
			p_event < 0 ? opennova::mission::FireRequest::NoEvent : kernel_->events.force_fire(static_cast<size_t>(p_event));
	if (request == opennova::mission::FireRequest::Armed) return OK;
	r_reason = String(opennova::mission::fire_request_words(request));
	return ERR_INVALID_PARAMETER;
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

// The body readout (an instrument; engine world/body_trace.h): the held records from a seq, the
// next seq, and the watched handles. The body_trace probe reads these.
TypedArray<BodyTraceRow> Simulation::debug_body_trace(int64_t p_from_seq) const {
	TypedArray<BodyTraceRow> rows;
	if (!kernel_) return rows;
	const uint32_t from = static_cast<uint32_t>(
			std::clamp<int64_t>(p_from_seq, 0, static_cast<int64_t>(UINT32_MAX)));
	for (const opennova::world::BodyTraceRecord &record : kernel_->world.body_trace.since(from)) {
		Ref<BodyTraceRow> row;
		row.instantiate();
		row->assign(record);
		rows.push_back(row);
	}
	return rows;
}

int64_t Simulation::debug_body_trace_next_seq() const {
	return kernel_ ? static_cast<int64_t>(kernel_->world.body_trace.next_seq()) : 0;
}

void Simulation::debug_set_body_trace_watch(const PackedInt32Array &p_handles) {
	if (!kernel_) return;
	opennova::world::BodyTrace &trace = kernel_->world.body_trace;
	trace.clear_watch();
	const int64_t count = std::min<int64_t>(p_handles.size(),
			static_cast<int64_t>(opennova::world::BodyTrace::kWatchSlots));
	for (int64_t i = 0; i < count; ++i) {
		const int32_t handle = p_handles[i];
		trace.watch[static_cast<size_t>(i)] =
				handle >= 0 && handle < 0xFFFF ? static_cast<uint16_t>(handle) : uint16_t{0xFFFF};
	}
}

int64_t Simulation::debug_local_body_handle() const {
	return kernel_ && kernel_->world.cached.local_player.valid()
			? static_cast<int64_t>(kernel_->world.cached.local_player.packed) : -1;
}
