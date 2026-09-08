#include "vehicle_motor_detail.h"
#include "world.h"

namespace opennova::world::detail {
namespace {
void clear_lane(Entity &e, const VehicleTraits &traits, uint8_t lane) {
	const auto &definition = traits.trails[lane - 1];
	if (definition.effect.empty())
		return;
	for (int i = 0; i < 16; ++i)
		if ((definition.mask & (1u << i)) != 0)
			e.veh.trails.points[i] = {};
	if (lane <= 2)
		e.veh.trails.effect_state[lane - 1] = 0;
}
Vec3 vector_from_fixed(const int32_t value[3]) {
	return { float(from_fixed(value[0])), float(from_fixed(value[1])),
		float(from_fixed(value[2])) };
}
} // namespace

// All four definitions address the same sixteen emitter handles. A second
// lane touching a live point updates its controls without replacing its effect.
// [orig: Entity_UpdateBoneTrailEffects @0x4589C0]
void vehicle_update_trail_lane(
		World &world, Entity &e, const VehicleTraits &traits, uint8_t lane, int32_t intensity) {
	if (lane < 1 || lane > 4 || e.veh.movement_effects_disabled)
		return;
	const auto &definition = traits.trails[lane - 1];
	if (definition.effect.empty() || definition.mask == 0)
		return;
	const uint16_t previous = lane <= 2 ? e.veh.trails.effect_state[lane - 1] : 0;
	if (intensity == 0 || (!definition.secondary_effect.empty() && previous != 0 && previous != 1))
		clear_lane(e, traits, lane);
	if (intensity == 0 || !world.out.fire_sounds.listener_valid())
		return;
	const auto &camera = world.out.fire_sounds.listener();
	const int64_t dx = int64_t(to_fixed(e.position.x)) - to_fixed(camera.x);
	const int64_t dy = int64_t(to_fixed(e.position.y)) - to_fixed(camera.y);
	const int32_t limit = ((world.logic_tick & 3u) == 0 ? 600 : 300) << 16;
	if (std::sqrt(double(dx) * dx + double(dy) * dy) > limit)
		return;
	const auto frame = entity_placement_matrix(e);
	const uint32_t magnitude = vehicle_trail_magnitude_q16(intensity);
	for (int i = 0; i < traits.trail_point_count; ++i) {
		if ((definition.mask & (1u << i)) == 0)
			continue;
		auto &point = e.veh.trails.points[i];
		int32_t pos[3], dir[3];
		frame.transform_point(traits.trail_points[i].position, pos);
		frame.rotate_point(traits.trail_points[i].direction, dir);
		if ((e.flags & 0x8000u) != 0)
			pos[2] = world.env.water_z;
		if (point.definition == 0)
			point.definition = lane;
		point.position = vector_from_fixed(pos);
		point.direction = vector_from_fixed(dir);
		point.magnitude_q16 = magnitude;
		point.source_tick = world.logic_tick;
	}
	if (lane <= 2)
		e.veh.trails.effect_state[lane - 1] = 1;
}

// The contact solve releases the outgoing medium's masks at the crossing.
// Ground uses W2/W3; bikes, tanks, and boats carry both lanes in each medium.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0;
// Entity_ProcessLightVehiclePhysics @0x479600; Entity_ProcessPlatformPhysics @0x481870]
void vehicle_trail_water_transition(Entity &e, const VehicleTraits &traits, bool was_water) {
	if (e.veh.movement_effects_disabled)
		return;
	const bool water = (e.flags & 0x8000u) != 0;
	if (water == was_water)
		return;
	if (water) {
		if (traits.family != VehicleFamily::Ground)
			clear_lane(e, traits, 1);
		clear_lane(e, traits, 2);
	} else {
		clear_lane(e, traits, 3);
		if (traits.family != VehicleFamily::Ground)
			clear_lane(e, traits, 4);
	}
}

// Per-family call sites retain their authored command/current-speed choices.
// Bike dry trails clamp reverse motion; ground suppresses its tiny coast tail.
// [orig: Entity_UpdateVehiclePhysics @0x48AF00; Entity_UpdateLightVehiclePhysics @0x483FE0;
// Entity_UpdateTankVehiclePhysics @0x488AB0; Entity_UpdateWatercraftPhysics @0x48D480]
void vehicle_sample_trails(
		World &world, Entity &e, const VehicleTraits &traits, int32_t target_speed) {
	const auto &m = e.veh;
	if ((e.flags & 0x8000u) != 0) {
		if ((world.logic_tick & 1u) != 0)
			return;
		vehicle_update_trail_lane(world, e, traits, 3, m.cmd_speed);
		if (traits.family != VehicleFamily::Ground)
			vehicle_update_trail_lane(world, e, traits, 4, m.speed);
	} else {
		if ((world.logic_tick & 3u) != 0)
			return;
		int32_t command = m.cmd_speed, motion = m.speed;
		if (traits.family == VehicleFamily::Ground) {
			if (target_speed == 0 && m.speed_accel == 0 && io::bam_abs(motion) < 48)
				motion = 0;
		} else {
			if (traits.family == VehicleFamily::Bike) {
				command = std::max(0, command);
				motion = std::max(0, motion);
			}
			vehicle_update_trail_lane(world, e, traits, 1, command);
		}
		if (traits.family == VehicleFamily::Watercraft)
			motion = command;
		vehicle_update_trail_lane(world, e, traits, 2, motion);
	}
}
} // namespace opennova::world::detail
