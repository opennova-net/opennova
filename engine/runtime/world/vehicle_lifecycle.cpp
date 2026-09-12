#include "vehicle_system.h"
#include "world.h"
#include "ai.h"
#include "angle.h"
#include "collision.h"
#include "vehicle_motor_detail.h"
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

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
	// The class init's gunner-attachment setup runs once every pool-1 record is
	// resident (retail's model init walks pool 1 after the whole load) and
	// before the first mover tick below, whose installed +0x1C4 callback already
	// carries the children [orig: Entity_InitAllFromModels @0x40E5B8..0x40E5D8
	//  -> the def callbacks @0x4686C0/@0x4683C0 -> Entity_SetupGunnerAttachments
	//  @0x468964/@0x468692]. The def's `Parent` byte reaches the port through
	// VehicleTraits::attrib_parent, which the items.def traits sweep feeds after
	// promotion, so the setup sits here rather than in promote's brain init.
	for (EntityHandle handle : handles) {
		if (Entity *entry = world_.registry.get(handle))
			setup_gunner_attachments(*entry);
	}
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
	// The +0x1B0 destroy timer re-seeds from def+0x1A4 (the same source as
	// the Entity_InitFromItemDef seed) [orig: Entity_RespawnVehicle `mov eax,
	//  [edx+1A4h]; mov [esi+1B0h], eax` @0x46007C..0x460082].
	if (const auto *traits = world_.tables.item_death_traits.get(e.item_id)) {
		e.destroy_timer = traits->destroy_timing_ticks[0];
		e.destroy_timer_initialized = true;
	}
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
		world_.out.entity_events.push_back(EntityRemoveEvent{e.handle.packed});
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

// ---------------------------------------------------------------------------
// Gunner attachments: the class init's same-refNum pool-1 peer list ridden on
// the vehicle's 'agun' userpoints, and the per-tick follow the installed
// +0x1C4 callback runs after the saved motor. Retail installs the follow as
// the entity's update callback and calls the saved motor from it; the port's
// motor pass calls the follow right after the mover, which is the same order.
// [orig: Entity_SetupGunnerAttachments @0x468100, called by
//  Entity_InitVehicleAIFromDef @0x46895A..0x468964 and
//  Entity_InitHelicopterAIFromDef @0x468688..0x468692 when ItemDef+0x548
//  (the items.def attrib token `Parent`) is set; Entity_UpdateAttachedChildren
//  @0x45D550 (saved callback first @0x45D573, guard gate @0x45D578)]
// ---------------------------------------------------------------------------

namespace {

// The greedy pass's distance: sqrt in double over the Q16 deltas (z first, the
// x87 order), clamped at 2147418100.0, truncated to int64 and compared UNSIGNED
// against the -1 sentinel [orig: @0x468301..0x468369].
uint32_t attachment_distance(const int32_t child[3], const int32_t point[3]) {
	const double dz = double(child[2]) - double(point[2]);
	const double dy = double(child[1]) - double(point[1]);
	const double dx = double(child[0]) - double(point[0]);
	double dist = std::sqrt(dz * dz + dy * dy + dx * dx);
	if (dist > 2147418100.0)
		dist = 2147418100.0;
	return static_cast<uint32_t>(static_cast<int64_t>(dist));
}

EntityHandle attached_child_handle(const AiBrain &b, int slot) {
	const int32_t word = b.f[AiBrain::kAttachSlots + 2 * slot + 1];
	return word > 0 ? EntityHandle{ static_cast<uint16_t>(word - 1) } : EntityHandle{};
}

} // namespace

void VehicleSystem::setup_gunner_attachments(Entity &vehicle) {
	World &world = world_;
	// The class init reaches the setup only for an AI-data item whose def sets
	// `Parent` [orig: @0x46895A..0x468964]; the setup itself bails without the
	// 812-byte brain [orig: @0x46811E].
	const VehicleTraits *t = traits.get(vehicle.item_id);
	AiEntity *ai = world.ai.for_handle(vehicle.handle);
	if (t == nullptr || !t->attrib_parent || ai == nullptr)
		return;
	AiBrain &b = ai->brain;

	// Every OTHER pool-1 entity whose refNum byte equals this entity's nonzero
	// refNum, in pool slot order, sixteen at most [orig: the g_pool_list[1]
	// walk @0x468130..0x468173: self skip @0x468154, refNum nonzero @0x46815E,
	// the +533 compare @0x468166, the slot store @0x468168, the 16 cap @0x468173].
	int32_t count = 0;
	world.registry.for_each_in_pool(1, [&](const Entity &candidate) {
		if (count >= AiBrain::kAttachSlotMax)
			return;
		if (candidate.handle == vehicle.handle)
			return;
		if (vehicle.ref_num == 0 || candidate.ref_num != vehicle.ref_num)
			return;
		b.f[AiBrain::kAttachSlots + 2 * count] = 0;
		b.f[AiBrain::kAttachSlots + 2 * count + 1] =
				static_cast<int32_t>(candidate.handle.packed) + 1;
		++count;
	});
	// No peer: nothing is installed and the count word stays [orig: @0x468177].
	if (count == 0)
		return;
	// The count word (+576) and the callback chain: the saved motor keeps
	// running first, then the follow [orig: @0x468183..0x468193].
	b.f[AiBrain::kAttachCount] = count;

	// The first sixteen 'agun' userpoints, transformed by the entity's affine
	// fixed-point matrix (the scale variant when the entity carries anim data
	// or an item scale — entity_placement_matrix's own select)
	// [orig: @0x4681AA..0x4681D9; the matrix select @0x468200/@0x46823E;
	//  Math_FixedPointTransformPoint22 per point @0x468288].
	const CollisionMatrix matrix = entity_placement_matrix(vehicle);
	const int point_count =
			std::min<int>(static_cast<int>(t->agun_points.size()), AiBrain::kAttachSlotMax);
	int32_t world_points[AiBrain::kAttachSlotMax][3] = {};
	bool available[AiBrain::kAttachSlotMax] = {};
	for (int i = 0; i < point_count; ++i) {
		matrix.transform_point(t->agun_points[static_cast<size_t>(i)].position, world_points[i]);
		available[i] = true;
	}

	// Greedy nearest neighbour, child slot order: each child takes the closest
	// unassigned point and consumes it; a child with no point left keeps bone
	// 0 and stays where it is [orig: @0x4682C1..0x4683AB — best -1/-1
	// @0x4682DD..0x4682E0, the consumed-bone skip @0x4682F4, the unsigned
	// compare @0x468365, the store @0x468389 and the zeroing @0x46838B].
	for (int slot = 0; slot < count; ++slot) {
		const Entity *child = world.registry.get(attached_child_handle(b, slot));
		if (child == nullptr)
			continue;
		const int32_t child_pos[3] = { to_fixed(child->position.x),
			to_fixed(child->position.y), to_fixed(child->position.z) };
		uint32_t best = 0xFFFFFFFFu;
		int best_index = -1;
		for (int j = 0; j < point_count; ++j) {
			if (!available[j])
				continue;
			const uint32_t distance = attachment_distance(child_pos, world_points[j]);
			if (distance < best) {
				best_index = j;
				best = distance;
			}
		}
		if (best_index >= 0) {
			b.f[AiBrain::kAttachSlots + 2 * slot] = best_index + 1;
			available[best_index] = false;
		}
	}
}

void VehicleSystem::update_attached_children(Entity &vehicle) {
	World &world = world_;
	AiEntity *ai = world.ai.for_handle(vehicle.handle);
	if (ai == nullptr)
		return;
	AiBrain &b = ai->brain;
	// The guard gate: no attached children, nothing to follow [orig: @0x45D578].
	const int32_t count =
			std::min<int32_t>(b.f[AiBrain::kAttachCount], AiBrain::kAttachSlotMax);
	if (count <= 0)
		return;
	const VehicleTraits *t = traits.get(vehicle.item_id);
	// The parent matrix, rebuilt every tick from the live position and eulers
	// (the scale variant when the parent carries anim data or an item scale)
	// [orig: @0x45D585..0x45D5D1].
	const CollisionMatrix matrix = entity_placement_matrix(vehicle);
	const int32_t heading = vehicle.veh.yaw_seeded
			? vehicle.veh.yaw_bam
			: bam_heading_from_mission_yaw_deg(static_cast<double>(vehicle.yaw));
	const int32_t pitch = vehicle.veh.yaw_seeded
			? vehicle.veh.air_pitch_bam
			: bam_from_degrees_wrapped(static_cast<double>(vehicle.pitch));
	const int32_t roll = vehicle.veh.yaw_seeded
			? vehicle.veh.air_roll_bam
			: bam_from_degrees_wrapped(static_cast<double>(vehicle.roll));
	for (int slot = 0; slot < count; ++slot) {
		// A child with no assigned point is left alone [orig: @0x45D5F5].
		const int32_t bone = b.f[AiBrain::kAttachSlots + 2 * slot];
		if (bone <= 0)
			continue;
		Entity *child = world.registry.get(attached_child_handle(b, slot));
		if (child == nullptr || t == nullptr ||
				bone > static_cast<int32_t>(t->agun_points.size()))
			continue;
		// child +4..+24 = the parent's position and eulers, then +4/+8/+12 =
		// the point's local position through the parent matrix (translation
		// included) [orig: @0x45D5FF..0x45D61D; @0x45D641..0x45D663].
		int32_t world_point[3];
		matrix.transform_point(t->agun_points[static_cast<size_t>(bone - 1)].position, world_point);
		child->position = { static_cast<float>(from_fixed(world_point[0])),
			static_cast<float>(from_fixed(world_point[1])),
			static_cast<float>(from_fixed(world_point[2])) };
		child->yaw = vehicle.yaw;
		child->pitch = vehicle.pitch;
		child->roll = vehicle.roll;
		child->veh.yaw_seeded = vehicle.veh.yaw_seeded;
		child->veh.yaw_bam = vehicle.veh.yaw_bam;
		child->veh.air_pitch_bam = vehicle.veh.air_pitch_bam;
		child->veh.air_roll_bam = vehicle.veh.air_roll_bam;
		// The six-dword velocity block (+152..+172: velocityX/Y, slideDecay and
		// the modelPtr0..2 rates) is the parent's unless the child is dead
		// (+286 <= 0) or flagged dead/husk (Flags & 6), then zero
		// [orig: @0x45D65B..0x45D6CA].
		const bool dead = child->health <= 0 ||
				((child->flags | child->engine_flags) & (kEntityFlagDead | kEntityFlagHusk)) != 0;
		child->veh.vel_x = dead ? 0 : vehicle.veh.vel_x;
		child->veh.vel_y = dead ? 0 : vehicle.veh.vel_y;
		child->veh.slide_z = dead ? 0 : vehicle.veh.slide_z;
		child->veh.wheel_rate_bam = dead ? 0 : vehicle.veh.wheel_rate_bam;
		child->veh.air_pitch_rate = dead ? 0 : vehicle.veh.air_pitch_rate;
		child->veh.air_roll_rate = dead ? 0 : vehicle.veh.air_roll_rate;
		// One entity struct in retail: a child that carries its own brain reads
		// the same words through the AiEntity mirrors.
		if (AiEntity *child_ai = world.ai.for_handle(child->handle)) {
			child_ai->pos[0] = world_point[0];
			child_ai->pos[1] = world_point[1];
			child_ai->pos[2] = world_point[2];
			child_ai->heading = heading;
			child_ai->pitch = pitch;
			child_ai->roll = roll;
			child_ai->vel_x = child->veh.vel_x; // entity+152
			child_ai->vel_z = child->veh.vel_y; // entity+156
		}
	}
}

} // namespace opennova::world
