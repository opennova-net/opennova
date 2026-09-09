#include "vehicle_system.h"
#include "world.h"
#include "angle.h"
#include "vehicle_motor_detail.h"
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>

namespace opennova::world {
namespace {
using io::bam_add;
using io::bam_sub;
int32_t mul22(int32_t a, int32_t b) {
	return static_cast<int32_t>((int64_t(a) * b) >> 22);
}
struct Trig {
	int32_t c[3], s[3];
	explicit Trig(const int32_t pose[6], bool inverse = false) {
		for (int i = 0; i < 3; ++i) {
			const double radians = double(pose[i + 3]) * 1.4629627251502471e-9;
			c[i] = int32_t(std::cos(radians) * 4194304.0);
			s[i] = int32_t(std::sin(radians) * (inverse ? -4194304.0 : 4194304.0));
		}
	}
};
void read_pose(const Entity &e, int32_t pose[6]) {
	carrier_pose_fixed(e, pose, pose[3], pose[4], pose[5]);
}
void write_pose(Entity &e, const int32_t pose[6]) {
	e.position = { float(from_fixed(pose[0])), float(from_fixed(pose[1])),
		float(from_fixed(pose[2])) };
	e.veh.yaw_seeded = true;
	e.veh.yaw_bam = pose[3];
	e.veh.air_pitch_bam = pose[4];
	e.veh.air_roll_bam = pose[5];
	e.yaw = static_cast<int16_t>(std::lround(mission_yaw_deg_from_bam_heading(pose[3])));
	e.pitch = static_cast<int16_t>(std::lround(double(pose[4]) * 360.0 / 4294967296.0));
	e.roll = static_cast<int16_t>(std::lround(double(pose[5]) * 360.0 / 4294967296.0));
}
int32_t respawn_ground(World &world, Entity &e) {
	const int32_t pos[3] = { to_fixed(e.position.x), to_fixed(e.position.y),
		to_fixed(e.position.z) };
	if (world.ai.collision != nullptr)
		return world.ai.collision->raycast_ground(
				world, e.handle, pos, 0, 0, 0x10000, 0x100000, &e.ground_target);
	if (world.tables.terrain != nullptr) {
		e.ground_target = {};
		return calc_average_ground_height(*world.tables.terrain, pos, 0, GroundClearance{});
	}
	return INT32_MIN;
}
void mirror_pose(World &world, Entity &e) {
	if (AiEntity *ai = world.ai.for_handle(e.handle)) {
		int32_t pose[6];
		read_pose(e, pose);
		std::copy_n(pose, 3, ai->pos);
		std::copy_n(pose, 3, ai->net_saved_live_pose);
		ai->heading = pose[3];
		ai->health = e.health;
		ai->team = e.team;
		ai->vel_x = e.veh.vel_x;
		ai->vel_z = e.veh.vel_y;
	}
}
} // namespace

// [orig: Game_StartMission @0x526012..0x52606B;
// Entity_TransformWorldToLocal @0x43BB50]
void VehicleSystem::capture_spawn_pose(Entity &e) {
	auto &m = e.veh;
	read_pose(e, m.spawn_pose);
	e.spawn_position = e.position;
	m.spawn_team = e.team;
	m.spawn_support = e.ground_target;
	if (const Entity *support = world_.registry.get(m.spawn_support)) {
		int32_t parent[6];
		read_pose(*support, parent);
		const Trig t(parent, true);
		const int32_t x = bam_sub(m.spawn_pose[0], parent[0]);
		const int32_t y = bam_sub(m.spawn_pose[1], parent[1]);
		const int32_t z = bam_sub(m.spawn_pose[2], parent[2]);
		const int32_t tx = bam_sub(mul22(x, t.c[0]), mul22(y, t.s[0]));
		const int32_t ty = bam_add(mul22(x, t.s[0]), mul22(y, t.c[0]));
		const int32_t tz = bam_add(mul22(tx, t.s[1]), mul22(z, t.c[1]));
		m.spawn_pose[0] = bam_sub(mul22(tx, t.c[1]), mul22(z, t.s[1]));
		m.spawn_pose[1] = bam_sub(mul22(ty, t.c[2]), mul22(tz, t.s[2]));
		m.spawn_pose[2] = bam_add(mul22(ty, t.s[2]), mul22(tz, t.c[2]));
		m.spawn_pose[3] = bam_sub(m.spawn_pose[3], parent[3]);
	}
	m.spawn_pose_valid = true;
}

// Pitch and roll pass through unchanged: only the saved yaw is relative.
// [orig: Entity_TransformLocalToWorld @0x43BD00]
bool VehicleSystem::resolve_spawn_pose(const Entity &e, int32_t pose[6]) const {
	const auto &m = e.veh;
	if (!m.spawn_pose_valid)
		return false;
	std::copy_n(m.spawn_pose, 6, pose);
	if (m.spawn_support.valid()) {
		const Entity *support = world_.registry.get(m.spawn_support);
		if (support == nullptr || ((support->flags | support->engine_flags) & 2u) != 0)
			return false;
		int32_t parent[6];
		read_pose(*support, parent);
		const Trig t(parent);
		const int32_t ry = bam_sub(mul22(pose[1], t.c[2]), mul22(pose[2], t.s[2]));
		const int32_t rz = bam_add(mul22(pose[1], t.s[2]), mul22(pose[2], t.c[2]));
		const int32_t px = bam_sub(mul22(pose[0], t.c[1]), mul22(rz, t.s[1]));
		const int32_t pz = bam_add(mul22(pose[0], t.s[1]), mul22(rz, t.c[1]));
		pose[0] = bam_add(parent[0], bam_sub(mul22(px, t.c[0]), mul22(ry, t.s[0])));
		pose[1] = bam_add(parent[1], bam_add(mul22(px, t.s[0]), mul22(ry, t.c[0])));
		pose[2] = bam_add(parent[2], pz);
		pose[3] = bam_add(pose[3], parent[3]);
	}
	return true;
}

// [orig: Game_StartMission @0x525F80..0x526071]
void VehicleSystem::initialize_mission_vehicles() {
	std::vector<EntityHandle> handles;
	world_.registry.for_each_in_pool(1, [&](const Entity &e) { handles.push_back(e.handle); });
	for (EntityHandle handle : handles) {
		Entity *entry = world_.registry.get(handle);
		if (entry == nullptr)
			continue;
		Entity &e = *entry;
		const VehicleTraits *t = traits.get(e.item_id);
		if (t == nullptr || !t->player_control || ((e.flags | e.engine_flags) & 0x80u) != 0)
			continue;
		const int32_t ground = respawn_ground(world_, e);
		if (ground != INT32_MIN) {
			e.position.z = float(from_fixed(
					std::max(world_.env.water_z, bam_add(ground, e.veh.air_probe_z_off))));
			e.veh.ground_cache = to_fixed(e.position.z);
		}
		e.flags |= 0x40u;
		e.engine_flags |= 0x40u;
		if (vehicle_family_uses_direct_air_mover(t->family))
			aircraft_client_tick(e, *t);
		else if (t->family == VehicleFamily::Watercraft)
			tick_watercraft_motor(e, *t);
		else
			tick_motor(e, *t);
		capture_spawn_pose(e);
		mirror_pose(world_, e);
		// The respawn budget seed: a hull with an AI slot stores thinkCooldown
		// (+0x128) = aiRuntime[18] / 31 (the signed magic divide 0x84210843,
		// sar 5, sign fix), which tick_dead consumes one per respawn and holds
		// at 1. Promote seeds slot[18] = 62 * spawns, so the budget is 2 * spawns.
		// [orig: Game_StartMission @0x526071..0x526095]
		if (AiEntity *ai = world_.ai.for_handle(e.handle))
			ai->inf.wait_cooldown = ai->slot.f[18] / 31;
	}
}

// [orig: Entity_RespawnVehicle @0x45FF40]
void VehicleSystem::respawn(Entity &e) {
	// [orig: Entity_RespawnVehicle @0x460186 -> EntityList_ClearParentRef
	// @0x43ED10] Only allocated pool-0 rows whose ground reference matches.
	world_.registry.for_each_in_pool(0, [&](const Entity &row) {
		if (row.item_id == 0 || row.ground_target != e.handle) return;
		Entity &rider = *world_.registry.get(row.handle);
		rider.ground_target = {};
		rider.mount_toggle_fallback = {};
	});
	world_.rotor_wash.release(e);
	auto &m = e.veh;
	// Respawn also retires the husk's authored emitters: retail zeroes the bank
	// handle words in place (+0x138 @0x4600ae, +0x1AC @0x46006e) and releases the
	// two live emitters (CEffectEmitter_ReleaseSafe on +0x1CC @0x460199 and on
	// +0x400 @0x4601b2); it never re-runs Entity_InitFromItemDef. Our banks are
	// event-driven, so the three authored bank families are released explicitly.
	// [orig: Entity_RespawnVehicle @0x45FF40]
	for (uint8_t family = 1; family <= 3; ++family)
		release_death_effect_bank(e, family, world_.out.destruction);
	detail::vehicle_release_damage_effects(world_, e);
	HuskSwapEvent intact;
	intact.net_id = e.net_id;
	intact.wire_handle = e.handle.packed;
	intact.bms_id = e.bms_id;
	intact.spawn_origin = e.spawn_origin;
	intact.item_id = e.item_id;
	intact.pos = e.position;
	intact.restore_intact = true;
	world_.out.destruction.husk_swaps.push_back(intact);
	vehicle_suspension_respawn(m);
	detail::vehicle_clear_chassis(m);
	if ((e.item_attrib & kItemAttribPlayerControl) != 0) {
		e.primary_occupant = {};
		for (auto &seat : e.seats)
			seat.occupant = {};
	}
	e.engine_flags = ((e.engine_flags | e.flags) & 0x400u) | 0x22000u;
	e.flags = e.engine_flags;
	e.health = e.health_max;
	e.alive = e.health > 0;
	e.death_motion = DeathMotionMode::None;
	e.motor_suspended = false;
	e.attach_parent = {};
	e.attach_bone = 0;
	e.death_tick = 0;
	e.section_mask = 0;
	e.spawned_piece_mask = 0;
	m.vel_x = m.vel_y = 0;
	m.wheel_rate_bam = m.air_pitch_rate = m.air_roll_rate = 0;
	m.slide_z = -501;
	m.cmd_lateral_speed = m.cmd_speed = m.net_climb = m.steer_ramp_bam = 0;
	m.steer_state = m.speed = m.speed_accel = 0;
	std::fill_n(m.plat_acc, 4, 0);
	std::fill_n(m.wheel_comp, 6, 0);
	e.net_move_input = 0;
	e.net_stance_bits = 0;
	if (AiEntity *ai = world_.ai.for_handle(e.handle)) {
		for (int reg = 135; reg <= 138; ++reg)
			ai->brain.f[reg] = 0;
		ai->brain.set_pend(ai->brain.f[AiBrain::kCurState] == 15 ? 14 : 22);
	}
	const int32_t ground = respawn_ground(world_, e);
	if (ground != INT32_MIN)
		e.position.z =
				float(from_fixed(std::max(world_.env.water_z, bam_add(ground, m.air_probe_z_off))));
	m.ground_cache = to_fixed(e.position.z);
	stamp_saved_live_pose(e);
	mirror_pose(world_, e);
}

// [orig: AI_TickState_VehicleDead @0x467EA0]
void VehicleSystem::tick_dead(Entity &e, AiEntity &ai) {
	auto &m = e.veh;
	m.stuck_ticks = bam_add(m.stuck_ticks, 1);
	const bool aircraft = ai.brain.f[AiBrain::kCurState] == 15;
	// The aircraft dead tick leaves motion to its installed +1C4 callback.
	// [orig: Entity_UpdateVehicleAIMovement @0x461200 vs ground @0x467EA0]
	if (!aircraft)
		entity_process_falling_death(
				world_, e, world_.tables.terrain, float(from_fixed(world_.env.water_z)));
	if ((e.item_attrib & kItemAttribPlayerControl) == 0 || !world_.ai.is_authority)
		return;
	if (((e.flags | e.engine_flags) & 0x1000u) != 0 || !world_.rules.vehicle_respawns) {
		// The remove-and-notify frees the AI component with the entity; a later
		// pool-1 spawn reusing this slot must not inherit a state-23 brain.
		// [orig: Server_RemoveEntityAndNotify @0x467ede -> Entity_Destroy @0x43e810,
		//  brain memset @0x43e995]
		world_.out.entity_removals.push_back(e.handle.packed);
		world_.commands.remove_ssn(e.handle);
		return;
	}
	if (m.stuck_ticks <= (aircraft ? 30 : 15))
		return;
	const int32_t cooldown = ai.inf.wait_cooldown;
	if (cooldown != 0) {
		ai.inf.wait_cooldown = bam_sub(cooldown, 1);
		if (cooldown == 1) {
			ai.inf.wait_cooldown = 1;
			return;
		}
	}
	if (m.respawn_waiting_for_overlay) {
		int32_t pos[6];
		read_pose(e, pos);
		pos[0] = bam_add(pos[0], 327680000);
		pos[1] = bam_add(pos[1], 327680000);
		if (world_.tables.terrain != nullptr)
			pos[2] = bam_sub(
					calc_average_ground_height(*world_.tables.terrain, pos, 0, GroundClearance{}),
					65536000);
		write_pose(e, pos);
		stamp_saved_live_pose(e);
		mirror_pose(world_, e);
		return;
	}
	int32_t spawn[6];
	if (!resolve_spawn_pose(e, spawn))
		return;
	write_pose(e, spawn);
	e.ground_target = m.spawn_support;
	ai.brain.f[AiBrain::kWpType] = 1;
	ai.brain.f[AiBrain::kWpChannel] = ai.slot.f[37];
	ai.brain.f[AiBrain::kWpNode] = ai.slot.f[38];
	e.team = m.spawn_team;
	respawn(e);
	e.team = m.spawn_team;
	ai.team = e.team;
}
// This is the refNum attachment group, independent of the team byte.
// [orig: Vehicle_CleanupTeamEntitiesOnDestruction @0x547040]
void VehicleSystem::cleanup_destroyed_ref_group(Entity &vehicle) {
	if (!vehicle.has_item_def || vehicle.item_type != 1 || (vehicle.item_attrib & 0x40) == 0)
		return;
	std::vector<EntityHandle> peers;
	world_.registry.for_each([&](const Entity &other) {
		if (other.handle != vehicle.handle && other.ref_num == vehicle.ref_num &&
				other.has_item_def && (other.item_attrib & 0x20) != 0)
			peers.push_back(other.handle);
	});
	for (EntityHandle handle : peers) {
		Entity *other = world_.registry.get(handle);
		if (other == nullptr)
			continue;
		world_.out.scars.clear_entity(handle);
		if (other->item_type != 1) {
			// The two words are zeroed for every matched non-vehicle peer; the
			// clip/reserve split only follows when a weapon row resolves.
			// [orig: +804/+802 = 0 @0x5470f9..0x547100, then the optional
			//  WeaponSlot_SplitAmmoIntoClipAndReserve @0x547107..0x54710e]
			other->primary_weapon_slot.clip = 0;
			other->primary_weapon_slot.reserve = 0;
			const auto *weapon = world_.tables.weapons.by_index(other->primary_weapon_slot_adm);
			if (weapon != nullptr && weapon->startrounds != -1 && weapon->clipsize != -1) {
				other->primary_weapon_slot.clip =
						std::min<int32_t>(weapon->clipsize, weapon->startrounds);
				other->primary_weapon_slot.reserve =
						weapon->startrounds - other->primary_weapon_slot.clip;
			}
		}
		if (world_.rules.logic_authority && other->primary_occupant.valid())
			detach(other->primary_occupant);
	}
}

} // namespace opennova::world
