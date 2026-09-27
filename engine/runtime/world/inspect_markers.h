// world::inspect::entity_markers — the entity facts a world-space overlay
// labels (ADR 0042 d5: the one engine function per fact): position, bound
// radius, name, team and health for the rows near an anchor, plus the
// selected row wherever it is. Cheap enough to call every logic tick (one
// registry walk, no card, no AI join beyond the "has a brain" bit).
#pragma once

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {
class World;
}

namespace opennova::world::inspect {

struct EntityMarker {
	uint16_t handle = EntityHandle::kInvalid;
	int32_t bms_id = 0;
	std::string name;
	std::string item_name; // items.def display name; empty without a def
	int32_t team = -1;
	int32_t health = 0;
	int32_t health_max = 0;
	bool alive = false;
	bool local_player = false;
	bool has_brain = false;
	bool selected = false;
	Vec3 mission_position{};
	float bound_radius = 0.0f; // 0 = unstamped
};

struct EntityMarkerQuery {
	Vec3 anchor{};              // mission frame (the camera eye)
	float range_units = 0.0f;   // 0 = the selection alone
	int32_t cap = 64;           // nearest first
	uint16_t selected = EntityHandle::kInvalid;
	bool with_brains = true;    // a joiner's tooling leaves the AI pool out
};

// The live rows carrying a wire type within range of the anchor, nearest
// first and capped, with the selected row always included (and flagged).
std::vector<EntityMarker> entity_markers(const World &world, const EntityMarkerQuery &query);

}  // namespace opennova::world::inspect
