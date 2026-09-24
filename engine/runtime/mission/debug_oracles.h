#pragma once

// THE F3 DEBUG ORACLES over the kernel (ADR 0039 dev tooling, no retail
// counterpart): the hitbox view — the round hit-detection reality around the
// local player — and the entity pick, one plain trace_projectile segment
// along a camera or crosshair ray. Mission space throughout; the embedder
// maps axes and marshals the records.

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission {

class MissionKernel;

// One posed person section sphere; `fallback` marks the bounded stand-in for
// an entity whose graphic supplies no usable authored sections.
struct DebugHitboxOrganic {
	world::EntityHandle handle;
	int32_t section = -1;
	int32_t center[3] = {}; // mission 16.16
	int32_t radius_q16 = 0;
	int32_t authored_radius_q16 = 0;
	bool masked = false;
	bool fallback = false;
};

struct DebugHitboxReport {
	std::vector<world::CollisionWorld::DebugHitboxEntity> entities;
	std::vector<DebugHitboxOrganic> organics;
};

// The shared debug budget: 80 units around the local player, 96 entities,
// 24000 faces; a preview with no player sweeps to the caps.
inline constexpr float kDebugHitboxRangeUnits = 80.0f;
inline constexpr int32_t kDebugHitboxEntityCap = 96;
inline constexpr int32_t kDebugHitboxFaceCap = 24000;

// A caller's budget over the same sweep: an explicit anchor (the F3 overlay
// anchors on the camera, so a free-flying spectator sees what it looks at)
// in place of the local player, and its own caps.
struct DebugHitboxBudget {
	bool has_anchor = false;
	world::Vec3 anchor{};
	float range_units = kDebugHitboxRangeUnits;
	int32_t entity_cap = kDebugHitboxEntityCap;
	int32_t face_cap = kDebugHitboxFaceCap;
};

// The transformed CFAC meshes of the nearby statics / vehicles, the posed
// pool-0 COBJ section spheres from the exact person narrow phase (the local
// avatar's collision instance is ensured first so F3's late-spawn demand
// still lands, one spare query slot keeping its authored rows off the target
// budget), then the bounded stand-in for every remaining live pool-0 entity
// (the same compatibility fallback RoundSim uses).
void collect_debug_hitboxes(MissionKernel &kernel, DebugHitboxReport &out);
void collect_debug_hitboxes(MissionKernel &kernel, DebugHitboxReport &out, const DebugHitboxBudget &budget);

struct DebugPick {
	enum class Blocked : uint8_t { None, Terrain, Water, Proxy };
	enum class HitClass : uint8_t { None, Static, Dynamic, Person };
	bool hit = false; // a pickable entity was hit
	Blocked blocked = Blocked::None; // why the ray stopped without one
	HitClass hit_class = HitClass::None;
	world::EntityHandle entity;
	int32_t kind = -1;
	int32_t index = -1;
	int32_t bms_id = 0;
	int32_t net_id = 0;
	int32_t item_id = 0;
	std::string name;
	world::Vec3 position{};
	float bound_radius = 0.0f;
	// The surface facts of any hit (a blocked one too).
	int32_t hit_position_q16[3] = {};
	int32_t hit_normal_q16[3] = {};
	float distance_units = 0.0f;
	int32_t section = -1;
	int32_t face = -1;
	int32_t bone = -1;
	int32_t hit_zone = -1;
	int32_t surface_type = -1;
	uint32_t material_flags = 0;
	uint32_t tick = 0;
};

inline constexpr double kDebugPickMinRangeUnits = 1.0;
inline constexpr double kDebugPickMaxRangeUnits = 2000.0;

// One plain geometric ray from `from` along `dir` (mission units, any length;
// a zero direction picks nothing) for `range_units` (clamped to the range
// above): exactly what a bullet would test — ammo_flags 0 (0x80 would bypass
// terrain, 0x4000000 would skip material-17 faces), persons walked, wire
// proxies excluded, the local player the owner so an eye ray never picks the
// picker or the vehicle they are mounted in (the ray[18] mount exclusion).
void debug_pick_entity(MissionKernel &kernel, const double from[3], const double dir[3],
		double range_units, DebugPick &out);

} // namespace opennova::mission
