#include <runtime/world/destruction.h>
#include "vehicle_motor_detail.h"
#include <runtime/world/angle.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include <base/io/bam.h>

#include <cmath>

namespace opennova::world {

// The AI think call and the installed per-tick update callback are distinct
// callers in retail; do not deduplicate them by frame number.
// [orig: Entity_ProcessFallingDeathPhysics @0x461D30;
// Entity_UpdatePool1Slot @0x4B8E2A..0x4B8E53]
void entity_process_falling_death(
		World &world, Entity &e, const terrain::TerrainHeightField *terrain, float water_height) {
	detail::vehicle_release_damage_effects(world, e);
	stamp_saved_live_pose(e);
	auto &m = e.veh;
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(e.item_id);
	if (traits != nullptr && traits->static_death) {
		m.vel_x = m.vel_y = m.slide_z = 0;
	} else {
		if (e.position.z + e.bound_radius >= water_height)
			m.slide_z = io::bam_sub(m.slide_z, 334);
		else {
			m.vel_x >>= 1;
			m.vel_y >>= 1;
			m.slide_z = -4096;
		}
		int32_t ground = INT32_MIN;
		const int32_t pos[3] = { to_fixed(e.position.x), to_fixed(e.position.y),
			to_fixed(e.position.z) };
		if (world.ai.collision != nullptr) {
			ground = world.ai.collision->raycast_ground(
					world, e.handle, pos, 0, 0, 0x10000, 0x200000, &e.ground_target);
		} else if (terrain != nullptr && terrain->valid()) {
			ground = to_fixed(terrain::height_field_height_world_bilinear(
					*terrain, e.position.x, -e.position.y));
			e.ground_target = {};
		}
		if (ground != INT32_MIN && traits != nullptr) {
			if (entity_placement_matrix(e).m[10] >= 0)
				ground = io::bam_sub(ground, to_fixed(std::abs(traits->husk_rest_min_z)));
			else
				ground = io::bam_add(ground, to_fixed(std::abs(traits->husk_rest_max_z)));
		}
		const int32_t z = io::bam_add(pos[2], m.slide_z);
		if (ground == INT32_MIN || z > ground) {
			e.position = { float(from_fixed(io::bam_add(pos[0], m.vel_x))),
				float(from_fixed(io::bam_add(pos[1], m.vel_y))), float(from_fixed(z)) };
		} else {
			// Full old pose is retained; only the vertical component stops.
			m.slide_z = 0;
		}
	}
	if (AiEntity *body = world.ai.for_handle(e.handle)) {
		for (int axis = 0; axis < 3; ++axis)
			body->net_saved_live_pose[axis] = e.saved_live_pos[axis];
		body->pos[0] = to_fixed(e.position.x);
		body->pos[1] = to_fixed(e.position.y);
		body->pos[2] = to_fixed(e.position.z);
		body->vel_x = m.vel_x;
		body->vel_z = m.vel_y;
	}
}

} // namespace opennova::world
