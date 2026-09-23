#include "carrier_motion.h"
#include "angle.h"
#include "infantry.h"
#include "vehicle_motor_detail.h"

#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>

namespace opennova::world::detail {

// The misleadingly named retail reset writes each rider's Health to zero,
// clears its attacker, and chooses the crash death animation; it does not detach.
// [orig: Entity_ResetOccupantAnimSlots @0x463B20]
void vehicle_kill_crash_occupants(World &world, Entity &e) {
	if (!world.ai.is_authority)
		return;
	auto kill = [&](EntityHandle h) {
		if (Entity *rider = world.registry.get(h)) {
			rider->death_anim_state = compute_death_anim_state(0, 2, 2);
			rider->health = 0;
			rider->last_attacker = {};
		}
	};
	for (const auto &seat : e.seats)
		kill(seat.occupant);
	kill(e.primary_occupant);
}

// Expiry follows the sleep test, so a newly expired row still solves this tick.
// [orig: @0x47602E..0x476048; @0x47973F..0x479759; @0x47C457..0x47C471;
// @0x47F192..0x47F1AC; @0x481A65..0x481A7F]
void vehicle_expire_contact_wake(World &world, Entity &e) {
	if (((e.flags | e.engine_flags) & 0x40u) != 0 &&
			io::bam_sub(int32_t(world.logic_tick), int32_t(e.veh.contact_wake_tick)) > 200) {
		e.flags &= ~0x40u;
		e.engine_flags &= ~0x40u;
		e.veh.contact_wake_tick = 0;
	}
}

// Both retail rebuilders replace the reference up axis with vertical, then
// preserve forward and rebuild the other two axes by normalized crosses.
// [orig: Entity_BuildOrientationFromVectors @0x458DF0 (down);
// Entity_RebuildOrientationMatrixFromAxes @0x4632E0 (up)]
void vehicle_rebuild_rest_orientation(World &world, Entity &e, bool inverted) {
	auto &m = e.veh;
	const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	int64_t raw[3] = { basis.q22.m[0] >> 6, basis.q22.m[4] >> 6, basis.q22.m[8] >> 6 };
	int32_t forward[3], side[3], up[3] = { 0, 0, inverted ? -65536 : 65536 };
	q16_normalize(raw, forward);
	q16_cross(up, forward, raw);
	q16_normalize(raw, side);
	q16_cross(forward, side, raw);
	q16_normalize(raw, up);
	vehicle_axes_to_euler(forward, side, up, m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	m.crashed = 0;
	m.byte_2ef = 0;
	m.settle_2f0 = inverted ? 1 : 0;
	if (inverted) {
		vehicle_smoke_effect(world, e, true);
		vehicle_smoke_effect(world, e);
		m.movement_effects_disabled = true;
	} else {
		vehicle_smoke_effect(world, e, true);
		vehicle_clear_chassis(m);
	}
}

// Parked-state maintenance is also reached through the sleep returns.
// [orig: ground @0x47C35D..0x47C443; tank @0x475F6B..0x47601B;
// aircraft @0x47F06D..0x47F182; boat @0x481A20..0x481A5C]
void vehicle_rest_state(World &world, Entity &e, VehicleFamily family) {
	auto &m = e.veh;
	const auto basis = vehicle_euler_basis(m.yaw_bam, m.air_pitch_bam, m.air_roll_bam);
	const int32_t up = basis.q22.m[10] >> 6;
	m.grounded = up > 4096 && !m.crashed;
	if (world.rules.logic_authority && family != VehicleFamily::Tank) {
		const bool parked = family == VehicleFamily::Watercraft
				? m.settle_2f0 != 0
				: m.crashed || m.settle_2f0 || (family == VehicleFamily::Ground && m.wreck_2fc);
		e.flags = parked ? e.flags | 0x10u : e.flags & ~0x10u;
		e.engine_flags = parked ? e.engine_flags | 0x10u : e.engine_flags & ~0x10u;
	}
	if (family == VehicleFamily::Watercraft)
		return;
	if ((e.flags & 0x10u) != 0 && up > 0) {
		if (vehicle_family_uses_direct_air_mover(family))
			vehicle_rebuild_rest_orientation(world, e, true);
		else
			m.settle_2f0 = 1;
	} else if ((e.flags & 0x10u) == 0 && up < 0) {
		vehicle_rebuild_rest_orientation(world, e, false);
	}
}

// The mover-entry and received-target air samples share the same ground
// query. The latter keeps the support result while restoring the live pose.
// [orig: Entity_CalcAverageGroundHeight @0x457230; aircraft @0x490998..0x490B33]
int32_t vehicle_ground_height_at(
		World &world, Entity &vehicle, const VehicleTraits &traits, const int32_t position[3]) {
	auto &m = vehicle.veh;
	const bool air = vehicle_family_uses_direct_air_mover(traits.family);
	int32_t pos[3] = { position[0], position[1], position[2] };
	if (air)
		pos[2] = io::bam_sub(pos[2], m.air_probe_z_off);
	int32_t ground = INT32_MIN;
	if (world.ai.collision != nullptr) {
		ground = world.ai.collision->raycast_ground(world, vehicle.handle, pos, 0, 0, 0x10000,
				air ? 0x300000 : 0x200000, &vehicle.ground_target);
	} else if (world.tables.terrain != nullptr) {
		ground = calc_average_ground_height(*world.tables.terrain, pos, 0, GroundClearance{});
		vehicle.ground_target = {};
	}
	if (air && ground != INT32_MIN) {
		if (world.registry.get(vehicle.primary_occupant) != nullptr)
			ground = std::max(ground, world.env.water_z);
		ground = io::bam_add(ground, brain_ground_offset(world, vehicle));
	}
	return ground;
}

// Ground/tank/bike test the entity-update counter; boat/air stagger the tick
// by 36 * DcbId.
// [orig: `test byte ptr dword_24C1948,7` in Entity_UpdateVehiclePhysics
//  @0x48AFB9..0x48AFD6, Entity_UpdateLightVehiclePhysics @0x484099,
//  Entity_UpdateTankVehiclePhysics @0x488B69 and Entity_ProcessInfantryPhysics
//  @0x46E178; the tick + 36 * DcbId stagger in Entity_UpdateWatercraftPhysics
//  @0x48D51F, Entity_UpdateAircraftPhysics @0x4903A8..0x4903D8 and
//  Entity_ProcessAirVehiclePhysics @0x46FA24/@0x46FA77;
//  Entity_RaycastGroundHeightAndObject @0x414320]
void vehicle_refresh_ground_link(World &world, Entity &vehicle, const VehicleTraits &traits) {
	auto &m = vehicle.veh;
	const bool air = vehicle_family_uses_direct_air_mover(traits.family);
	const bool staggered = air || traits.family == VehicleFamily::Watercraft;
	const uint32_t phase = staggered ? world.logic_tick + 36u * uint32_t(vehicle.net_id)
	                                 : world.entity_update_counter;
	if ((phase & 7u) != 0 && !(air && m.ground_cache == INT32_MIN))
		return;
	const int32_t pos[3] = { to_fixed(vehicle.position.x), to_fixed(vehicle.position.y),
		to_fixed(vehicle.position.z) };
	const int32_t ground = vehicle_ground_height_at(world, vehicle, traits, pos);
	if (air && ground != INT32_MIN)
		m.ground_cache = ground;
}

// Full parent delta, without the infantry capsule offset/radius detach.
// [orig: Entity_UpdateVehiclePhysics @0x48AF00, carrier block;
// Entity_UpdateWatercraftPhysics @0x48D6DA..0x48DACD;
// Entity_UpdateAircraftPhysics @0x4905BC..0x49095B]
void vehicle_follow_carrier(World &world, Entity &vehicle) {
	const Entity *carrier = world.registry.get(vehicle.ground_target);
	if (carrier == nullptr || carrier == &vehicle)
		return;
	CarrierMotionPose pose;
	carrier_pose_fixed(vehicle, pose.pos, pose.yaw_bam, pose.pitch_bam, pose.roll_bam);
	follow_carrier_motion(*carrier, pose);
	vehicle.position = { float(from_fixed(pose.pos[0])), float(from_fixed(pose.pos[1])),
		float(from_fixed(pose.pos[2])) };
	auto &m = vehicle.veh;
	m.yaw_bam = pose.yaw_bam;
	m.air_pitch_bam = pose.pitch_bam;
	m.air_roll_bam = pose.roll_bam;
	vehicle.yaw = static_cast<int16_t>(std::lround(mission_yaw_deg_from_bam_heading(m.yaw_bam)));
	vehicle.pitch =
			static_cast<int16_t>(std::lround(double(m.air_pitch_bam) * 360.0 / 4294967296.0));
	vehicle.roll = static_cast<int16_t>(std::lround(double(m.air_roll_bam) * 360.0 / 4294967296.0));
}

int32_t vehicle_probe_pass(World &world, Entity &vehicle, const int32_t (*probes)[3],
		const int32_t *radii, int count, int32_t soft, int32_t hard, PlatProbeForce *forces,
		int32_t px, int32_t py, int32_t pz, EntityHandle *hit_entity) {
	const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
	const bool boat = traits != nullptr && traits->family == VehicleFamily::Watercraft;
	int32_t severity = 0;
	for (int i = 0; i < count; ++i) {
		forces[i] = {};
		// Indoors suppresses terrain only; decks and other models still solve.
		// [orig: Entity_CheckCollisionState @0x462A30, @0x462AE0..0x462AFA]
		if (((vehicle.flags | vehicle.engine_flags) & 0x800000u) == 0)
			severity = std::max(severity,
					plat_terrain_probe(world, probes[i][0], probes[i][1], probes[i][2], radii[i],
							soft, hard, forces[i], !boat && i < 4));
	}
	EntityHandle hit;
	if (world.ai.collision != nullptr) {
		const int32_t position[3] = { px, py, pz };
		severity = std::max(severity,
				world.ai.collision->resolve_vehicle_probes(
						world, vehicle, position, probes, radii, count, soft, hard, forces, hit));
	}
	if (hit_entity != nullptr)
		*hit_entity = hit;
	return severity;
}

namespace {

bool mass_exchange_target(const World &world, const Entity &vehicle, const Entity *other,
		int32_t &our_mass, int32_t &their_mass) {
	if (other == nullptr || (other->item_attrib & 0x40u) == 0)
		return false;
	const VehicleTraits *ours = world.vehicles.traits.get(vehicle.item_id);
	const VehicleTraits *theirs = world.vehicles.traits.get(other->item_id);
	if (ours == nullptr || theirs == nullptr)
		return false;
	our_mass = ours->mass;
	their_mass = theirs->mass;
	return int64_t(their_mass) < 2LL * our_mass &&
			other->bound_radius < 2.0f * vehicle.bound_radius &&
			int64_t(our_mass) + their_mass != 0;
}

int32_t length_q16(int32_t x, int32_t y, int32_t z) {
	return static_cast<int32_t>(
			std::min(std::sqrt(double(x) * x + double(y) * y + double(z) * z), 2147418112.0));
}

} // namespace

void vehicle_contact_mass_share(World &world, const Entity &vehicle, EntityHandle hit, int32_t &dx,
		int32_t &dy, int32_t *depths, int count) {
	int32_t our_mass = 0, their_mass = 0;
	if (!mass_exchange_target(world, vehicle, world.registry.get(hit), our_mass, their_mass))
		return;
	const int64_t sum = int64_t(our_mass) + their_mass;
	dx = int32_t(int64_t(dx) * their_mass / sum);
	dy = int32_t(int64_t(dy) * their_mass / sum);
	for (int i = 0; i < count; ++i)
		depths[i] = int32_t(int64_t(depths[i]) * their_mass / sum);
}

// Damage and momentum are deliberately different gates: only damage requires
// authority. Momentum is inside the scrape edge, even if no sound is authored.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0, @0x47CD00..0x47CF55;
// Entity_ProcessPlatformPhysics @0x481870, @0x482336..0x48253D;
// Entity_ProcessWheeledVehiclePhysics @0x475DE0, @0x4769B8..0x476BBF;
// Entity_ProcessLightVehiclePhysics @0x479600; Entity_ProcessAircraftContactPhysics @0x47EF10]
void vehicle_contact_impact(World &world, Entity &vehicle, const VehicleTraits &traits,
		int severity, EntityHandle hit, int32_t px, int32_t py, int32_t pz) {
	auto &m = vehicle.veh;
	if (severity < 2)
		m.collision_sound_latched = false;
	if (severity != 3)
		return;
	const int32_t impact = length_q16(m.vel_x, m.vel_y, m.slide_z);
	if (world.ai.is_authority && (vehicle.flags & 0x4000000u) == 0 &&
			traits.family != VehicleFamily::Tank && vehicle.health > 0) {
		const int32_t moved = vehicle.saved_live_valid
				? length_q16(io::bam_sub(px, vehicle.saved_live_pos[0]),
						  io::bam_sub(py, vehicle.saved_live_pos[1]),
						  io::bam_sub(pz, vehicle.saved_live_pos[2]))
				: impact;
		if (traits.unit_type == 3) {
			if (impact > 29300 && moved > 29300)
				vehicle.health = 0;
		} else if (traits.family == VehicleFamily::Watercraft && traits.physics != 0) {
			if (impact > 29300 && moved > 29300)
				vehicle.health = std::max(0, vehicle.health - 100);
		} else {
			const int32_t speed = io::bam_sub(m.speed, m.speed >> ((traits.torque + 2) & 31));
			const float scale = traits.physics == 0 ? 2.499999936844688e-5f
					: traits.mass > 3				? 2.499999936844688e-5f
													: 1.249999968422344e-5f;
			const double damage = double(io::bam_abs(speed)) * traits.mass * double(scale);
			vehicle.health = damage >= vehicle.health ? 0 : vehicle.health - int32_t(damage);
		}
		if (vehicle.health == 0)
			vehicle.last_attacker = {};
	}
	if (m.collision_sound_latched || impact <= 2344)
		return;
	m.collision_sound_latched = true;
	world.vehicles.play_contact_sound(vehicle, traits, 26);
	Entity *other = world.registry.get(hit);
	int32_t our_mass = 0, their_mass = 0;
	if (!mass_exchange_target(world, vehicle, other, our_mass, their_mass))
		return;
	const int64_t mass = int64_t(our_mass) + their_mass;
	const int32_t transfer[3] = { int32_t(int64_t(io::bam_sub(m.vel_x, other->veh.vel_x)) *
										  our_mass / mass),
		int32_t(int64_t(io::bam_sub(m.vel_y, other->veh.vel_y)) * our_mass / mass),
		int32_t(int64_t(io::bam_sub(m.slide_z, other->veh.slide_z)) * our_mass / mass) };
	other->veh.vel_x = io::bam_add(other->veh.vel_x, bam_mul_wrap(3, transfer[0]) >> 2);
	other->veh.vel_y = io::bam_add(other->veh.vel_y, bam_mul_wrap(3, transfer[1]) >> 2);
	other->veh.slide_z = io::bam_add(other->veh.slide_z, bam_mul_wrap(3, transfer[2]) >> 2);
	other->position.x =
			float(from_fixed(io::bam_add(to_fixed(other->position.x), transfer[0] >> 2)));
	other->position.y =
			float(from_fixed(io::bam_add(to_fixed(other->position.y), transfer[1] >> 2)));
	other->position.z =
			float(from_fixed(io::bam_add(to_fixed(other->position.z), transfer[2] >> 2)));
}

// A landing reaction runs once at the first positive probe depth, before
// the spring loop consumes the accumulated free-fall sinks.
// [orig: ground @0x47D9A5..0x47DABD; tank @0x477DAC..0x477EEE;
// bike @0x47A592..0x47A708; aircraft @0x480428..0x480557]
void vehicle_landing_damage(World &world, Entity &e, const VehicleTraits &t, const int32_t *depth,
		int count, int32_t up) {
	auto &m = e.veh;
	if (((e.flags | e.engine_flags) & 0x4000000u) != 0)
		return;
	const int wheels = t.family == VehicleFamily::Bike ? 2 : 4;
	bool touched = false;
	for (int i = 0; i < count; ++i)
		touched |= depth[i] > 0;
	if (!touched)
		return;
	for (int i = 0; i < wheels; ++i)
		if (m.plat_acc[i] <= 5000)
			return;
	const int32_t fall = io::bam_abs(m.slide_z);
	if (fall > 3000)
		world.vehicles.play_contact_sound(e, t, 36);
	if (!world.ai.is_authority || fall <= 15000)
		return;
	const bool tank = t.family == VehicleFamily::Tank;
	const double scale = tank ? double(0.0002500000118743628f)
			: t.mass > 3	  ? double(0.02500000037252903f)
							  : double(0.005000000353902578f);
	double damage = double(bam_mul_wrap(fall, t.mass)) * scale;
	if (t.family == VehicleFamily::Bike)
		damage *= double(0.10000000149011612f);
	if (tank ? up < 0 : up > 0)
		damage *= 0.5;
	if (damage > double(e.health))
		e.health = 0;
	else
		e.health = int16_t(uint16_t(uint32_t(e.health) - uint32_t(int64_t(damage))));
	if (e.health <= 0) {
		e.health = 0;
		e.last_attacker = {};
	}
}

// Inverted hulls crushed onto their roof during a long fall die even when
// the ordinary impact severity is small. [orig: @0x47DBE8..0x47DC4C;
// tank @0x478024..0x47809D; @0x47AA96..0x47AAF1; @0x480749..0x4807AA]
void vehicle_crush_damage(
		World &world, Entity &e, const VehicleTraits &t, const int32_t *spine, int32_t up) {
	const auto &m = e.veh;
	if (up >= 0 || !world.ai.is_authority || ((e.flags | e.engine_flags) & 0x4000000u) != 0 ||
			m.slide_z >= -100000 || (spine[0] == 0 && spine[1] == 0 && spine[2] == 0))
		return;
	const int wheels = t.family == VehicleFamily::Bike ? 2 : 4;
	for (int i = 0; i < wheels; ++i)
		if (m.plat_acc[i] <= 7000)
			return;
	e.health = 0;
	e.last_attacker = {};
}

} // namespace opennova::world::detail
