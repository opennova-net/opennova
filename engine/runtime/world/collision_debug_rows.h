// The collision debug captures as drawable rows (dev tooling, not a ported
// surface): the ray rings and the contact ring CollisionWorld records while a
// dev-tools window or overlay arms them, filtered by the SAME presentation
// state the capture carries — the category/kind mask and the fade window —
// so the F3 Rays/Physics filters decide what the Game-view overlay draws.
// One walk per call, oldest first; positions stay 16.16 mission fixed.
#pragma once

#include <runtime/world/geom.h>

#include <cstdint>
#include <vector>

namespace opennova::world {

class CollisionWorld;
class World;

struct RayDebugRow {
	uint8_t category = 0; // CollisionWorld::RayDebugCategory
	uint8_t result = 0;   // kRayDebugClear / Hit / Blocked
	uint32_t age_ticks = 0;
	FixedVec3 start;
	FixedVec3 end;
	FixedVec3 hit;
};

// Every captured ray whose category bit is set in the capture's mask and
// whose age is within its TTL (ages counted against now_tick, wrapped stamps
// from a restarted clock dropped).
void ray_debug_rows(const CollisionWorld &collision, uint32_t now_tick, std::vector<RayDebugRow> &out);

struct ContactDebugRow {
	uint8_t kind = 0;         // CollisionWorld::ContactDebugKind
	uint8_t hit_class = 0xFF; // ProjectileHitClass for the trace kinds
	uint32_t age_ticks = 0;
	FixedVec3 pos;
	uint16_t target = 0xFFFF; // the touched body's packed handle (0xFFFF none)
	bool target_live = false;
	Vec3 target_position{};   // mission units, when live
	float target_bound_radius = 0.0f;
};

// Every captured contact whose kind bit is set in the capture's mask and
// whose age is within the flash window, with the touched body's live
// position and bound radius looked up in the registry.
void contact_debug_rows(const World &world, const CollisionWorld &collision, uint32_t now_tick,
		std::vector<ContactDebugRow> &out);

}  // namespace opennova::world
