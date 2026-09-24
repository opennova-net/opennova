// The F3 debug oracles — see debug_oracles.h.

#include <runtime/mission/debug_oracles.h>

#include <runtime/mission/mission_kernel.h>
#include <runtime/world/geom.h>
#include <runtime/world/round_sim.h> // kOrganicStandInCenterZ / kOrganicStandInRadius

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace opennova::mission {

void collect_debug_hitboxes(MissionKernel &kernel, DebugHitboxReport &out) {
	collect_debug_hitboxes(kernel, out, DebugHitboxBudget{});
}

void collect_debug_hitboxes(MissionKernel &kernel, DebugHitboxReport &out, const DebugHitboxBudget &budget) {
	out.entities.clear();
	out.organics.clear();
	world::World &w = kernel.world;
	const int32_t entity_cap = budget.entity_cap;

	// Anchor on the caller's point, else the local player; every payload
	// shares the one debug budget.
	int32_t anchor[3] = { 0, 0, 0 };
	int32_t debug_range = -1;
	const world::EntityHandle local_player = w.cached.local_player;
	const world::Entity *lp = local_player.valid() ? w.registry.get(local_player) : nullptr;
	const bool anchored = budget.has_anchor || lp != nullptr;
	const world::Vec3 anchor_units = budget.has_anchor ? budget.anchor
			: (lp != nullptr ? lp->position : world::Vec3{});
	if (anchored) {
		anchor[0] = world::to_fixed(anchor_units.x);
		anchor[1] = world::to_fixed(anchor_units.y);
		anchor[2] = world::to_fixed(anchor_units.z);
		debug_range = static_cast<int32_t>(budget.range_units) << 16;
	}
	out.entities = kernel.hitboxes(anchor_units, anchored ? budget.range_units : -1.0f, entity_cap,
			budget.face_cap);

	// The posed pool-0 COBJ spheres from the exact person narrow phase share
	// the budget; the local avatar's instance is ensured for F3's late-spawn
	// demand even though the avatar is presentation-hidden, and one spare
	// query slot keeps its authored rows off the target budget.
	if (local_player.valid()) kernel.ensure_collision_instance(w, local_player);
	std::unordered_set<uint16_t> posed_handles;
	const std::vector<world::CollisionWorld::DebugPersonSection> people =
			kernel.collision.debug_person_sections(w, anchor, debug_range, entity_cap + 1);
	for (const world::CollisionWorld::DebugPersonSection &person : people) {
		if (person.handle == local_player) continue;
		const bool new_handle = posed_handles.find(person.handle.packed) == posed_handles.end();
		if (new_handle && posed_handles.size() >= static_cast<size_t>(entity_cap)) break;
		DebugHitboxOrganic o;
		o.handle = person.handle;
		o.section = person.section;
		std::copy(person.center, person.center + 3, o.center);
		o.radius_q16 = person.radius;
		o.authored_radius_q16 = person.authored_radius;
		o.masked = person.masked;
		out.organics.push_back(o);
		posed_handles.insert(person.handle.packed);
	}

	// The bounded stand-in for every remaining live pool-0 entity.
	const size_t pool0 = w.registry.pool_capacity(0);
	int fallback_entity_count = static_cast<int>(posed_handles.size());
	for (size_t s = 0; s < pool0; ++s) {
		if (fallback_entity_count >= entity_cap) break;
		const world::Entity *e = w.registry.get(world::EntityHandle{ static_cast<uint16_t>(s) });
		if (e == nullptr || e->handle == local_player || (e->engine_flags & 0x02000001u) != 0 ||
				posed_handles.find(static_cast<uint16_t>(s)) != posed_handles.end())
			continue;
		if (debug_range >= 0) {
			const int32_t ep[3] = { world::to_fixed(e->position.x), world::to_fixed(e->position.y),
				world::to_fixed(e->position.z) };
			if (std::llabs(static_cast<int64_t>(ep[0]) - anchor[0]) > debug_range ||
					std::llabs(static_cast<int64_t>(ep[1]) - anchor[1]) > debug_range ||
					std::llabs(static_cast<int64_t>(ep[2]) - anchor[2]) > debug_range)
				continue;
		}
		DebugHitboxOrganic o;
		o.handle = e->handle;
		o.section = 1;
		o.center[0] = world::to_fixed(e->position.x);
		o.center[1] = world::to_fixed(e->position.y);
		o.center[2] = world::to_fixed(e->position.z + world::kOrganicStandInCenterZ);
		o.radius_q16 = world::to_fixed(world::kOrganicStandInRadius);
		o.authored_radius_q16 = o.radius_q16;
		o.fallback = true;
		out.organics.push_back(o);
		++fallback_entity_count;
	}
}

void debug_pick_entity(MissionKernel &kernel, const double from[3], const double dir[3],
		double range_units, DebugPick &out) {
	out = DebugPick();
	world::World &w = kernel.world;
	out.tick = w.logic_tick;
	const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len <= 0.0) return;
	const double range = std::clamp(range_units, kDebugPickMinRangeUnits, kDebugPickMaxRangeUnits);

	world::ProjectileTrace trace;
	trace.start = world::FixedVec3{ world::to_fixed(static_cast<float>(from[0])),
		world::to_fixed(static_cast<float>(from[1])), world::to_fixed(static_cast<float>(from[2])) };
	trace.end = world::FixedVec3{ world::to_fixed(static_cast<float>(from[0] + dir[0] / len * range)),
		world::to_fixed(static_cast<float>(from[1] + dir[1] / len * range)),
		world::to_fixed(static_cast<float>(from[2] + dir[2] / len * range)) };
	// The local player owns the ray, so the retail ray context's self and
	// mount exclusions apply: an eye ray never picks the picker or the vehicle
	// they are mounted in [orig: the ray[18] mount exclusion feeding
	// Physics_RaycastAgainstBoneCollision @0x4e4cb0].
	trace.owner = w.cached.local_player;
	trace.radius_q16 = 0;
	trace.ammo_flags = 0;
	const world::CollisionWorld::RayDebugScope ray_scope(kernel.collision,
			world::CollisionWorld::RayDebugCategory::kPick);
	const world::ProjectileHit hit = kernel.collision.trace_projectile(w, trace);
	if (!hit.hit()) return;

	out.hit_position_q16[0] = hit.position_q16.x;
	out.hit_position_q16[1] = hit.position_q16.y;
	out.hit_position_q16[2] = hit.position_q16.z;
	out.hit_normal_q16[0] = hit.normal_q16.x;
	out.hit_normal_q16[1] = hit.normal_q16.y;
	out.hit_normal_q16[2] = hit.normal_q16.z;
	out.distance_units = static_cast<float>(range * (static_cast<double>(hit.t_q16) / 65536.0));
	out.section = hit.section_index;
	out.face = hit.face_index;
	out.bone = hit.bone_index;
	out.hit_zone = hit.hit_zone;
	out.surface_type = hit.surface_type;
	out.material_flags = hit.material_flags;

	switch (hit.hit_class) {
		case world::ProjectileHitClass::Terrain:
			out.blocked = DebugPick::Blocked::Terrain;
			return;
		case world::ProjectileHitClass::Water:
			out.blocked = DebugPick::Blocked::Water;
			return;
		default:
			break;
	}
	const world::Entity *ent = w.registry.get(hit.geometry_entity);
	if (ent == nullptr) {
		// A decoded wire proxy or an already-freed slot: the geometry hit but
		// carries no pickable identity (joined visual-only clients).
		out.blocked = DebugPick::Blocked::Proxy;
		return;
	}
	out.hit = true;
	switch (hit.hit_class) {
		case world::ProjectileHitClass::StaticEntity:
			out.hit_class = DebugPick::HitClass::Static;
			break;
		case world::ProjectileHitClass::DynamicEntity:
			out.hit_class = DebugPick::HitClass::Dynamic;
			break;
		default:
			out.hit_class = DebugPick::HitClass::Person;
			break;
	}
	out.entity = hit.geometry_entity;
	out.kind = world::spawn_origin_kind(ent->spawn_origin);
	out.index = static_cast<int32_t>(world::spawn_origin_index(ent->spawn_origin));
	out.bms_id = static_cast<int32_t>(ent->bms_id);
	out.net_id = static_cast<int32_t>(ent->net_id);
	out.item_id = static_cast<int32_t>(ent->item_id);
	out.name = ent->name;
	out.position = ent->position;
	out.bound_radius = static_cast<float>(ent->bound_radius);
}

} // namespace opennova::mission
