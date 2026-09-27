#include <runtime/world/inspect_markers.h>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <unordered_set>

namespace opennova::world::inspect {

std::vector<EntityMarker> entity_markers(const World &world, const EntityMarkerQuery &query) {
	std::unordered_set<uint16_t> brains;
	if (query.with_brains) {
		for (int i = 0; i < world.ai.count(); ++i) {
			if (const AiEntity *e = world.ai.at(i)) brains.insert(e->handle.packed);
		}
	}
	const float range_sq = query.range_units * query.range_units;
	std::vector<std::pair<float, EntityMarker>> near;
	EntityMarker selected_marker;
	bool selected_found = false;
	world.registry.for_each([&](const Entity &e) {
		if (e.item_id == 0) return;
		const bool is_selected = e.handle.packed == query.selected;
		const bool is_local = e.handle == world.cached.local_player;
		const float dx = e.position.x - query.anchor.x;
		const float dy = e.position.y - query.anchor.y;
		const float dz = e.position.z - query.anchor.z;
		const float dist_sq = dx * dx + dy * dy + dz * dz;
		// A hidden row and the local player never enter by range; the
		// selection alone brings them in.
		const bool in_range = query.range_units > 0.0f && dist_sq <= range_sq &&
				!e.hidden && !is_local;
		if (!is_selected && !in_range) return;
		EntityMarker m;
		m.handle = e.handle.packed;
		m.bms_id = e.bms_id;
		m.name = e.name;
		if (const std::string *item_name = world.tables.item_names.get(e.item_id)) m.item_name = *item_name;
		m.team = static_cast<int32_t>(e.team);
		m.health = e.health;
		m.health_max = e.health_max;
		m.alive = e.alive;
		m.local_player = is_local;
		m.has_brain = brains.count(e.handle.packed) != 0;
		m.selected = is_selected;
		m.mission_position = e.position;
		m.bound_radius = e.bound_radius;
		if (is_selected) {
			selected_marker = m;
			selected_found = true;
			if (!in_range) return;
		}
		near.emplace_back(dist_sq, std::move(m));
	});
	std::stable_sort(near.begin(), near.end(),
			[](const auto &a, const auto &b) { return a.first < b.first; });
	std::vector<EntityMarker> out;
	const size_t cap = query.cap > 0 ? static_cast<size_t>(query.cap) : 0;
	for (auto &entry : near) {
		if (out.size() >= cap) break;
		out.push_back(std::move(entry.second));
	}
	// The selection survives the cap and the range: it is what the user asked
	// to see.
	if (selected_found &&
			std::none_of(out.begin(), out.end(), [](const EntityMarker &m) { return m.selected; })) {
		out.push_back(std::move(selected_marker));
	}
	return out;
}

}  // namespace opennova::world::inspect
