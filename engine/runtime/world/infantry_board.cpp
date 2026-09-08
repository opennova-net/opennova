// Reserved infantry commands and the authored carrier entry walk.
// [orig: Entity_UpdateInfantryAI @0x4B9910, Entity_FindBestSeatSlot @0x4351F0,
// Entity_GetBoneTransformAndOrientation @0x4B0C50, Entity_CanEnterVehicle @0x435480]
#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace opennova::world {
namespace {
int32_t board_bearing_to(int32_t dx, int32_t dy) {
	return static_cast<int32_t>(std::atan2(double(dy), double(dx)) * io::kBamPerRadian);
}
int32_t board_to_fixed(float v) { return static_cast<int32_t>(v * 65536.0f); }
int32_t board_dist(const int32_t pos[3], const int32_t tgt[3]) {
	const double dx = double(tgt[0]) - pos[0], dy = double(tgt[1]) - pos[1];
	const double dz = std::max(0.0, std::abs(double(tgt[2]) - pos[2]) - 65536.0);
	return static_cast<int32_t>(std::min(2147483647.0, std::sqrt(dx * dx + dy * dy + dz * dz)));
}
SeatSelectionMode seat_mode_for_command(int32_t command) {
	if (command == 123)
		return SeatSelectionMode::PassengerOnly;
	if (command == 124)
		return SeatSelectionMode::RejectController;
	return SeatSelectionMode::Any;
}
// [orig: Entity_GetBoneWorldPosition_0 @0x434DF0]
void seat_world_position(World &world, const Entity &carrier, const Seat &seat, int32_t out[3]) {
	MountedPose pose;
	const Vec3 p = world.pose_provider &&
					world.pose_provider->resolve_mounted_pose(world, carrier, seat, pose)
			? pose.position
			: entity_local_point_world(carrier, seat.seat_local);
	out[0] = board_to_fixed(p.x);
	out[1] = board_to_fixed(p.y);
	out[2] = board_to_fixed(p.z);
}
bool named_point(World &world, const Entity &carrier, const char *name, int32_t out[6]) {
	if (world.pose_provider &&
			world.pose_provider->resolve_named_transform(world, carrier.handle, name, out))
		return true;
	for (const Seat &seat : carrier.seats) {
		if (!strutil::iequals(seat.source_name, name))
			continue;
		seat_world_position(world, carrier, seat, out);
		out[3] = out[4] = out[5] = 0;
		return true;
	}
	return false;
}
// Claim the lowest unclaimed E1..En when resolving a new command target.
// The current command's 123/124 terms are intentional in the retail predicate.
// [orig: Entity_UpdateInfantryAI @0x4BB0BB..0x4BB269]
void claim_entry(AiEntity &e, World &world, const Entity &target, int32_t command) {
	int count = 0, point[6] = {};
	for (int i = 8; i > 0; --i) {
		const char name[] = { 'E', char('0' + i), 0 };
		if (named_point(world, target, name, point)) {
			count = i;
			break;
		}
	}
	bool claimed[32] = {};
	world.registry.for_each_in_pool(0, [&](const Entity &other) {
		if (other.handle == e.handle || !other.alive || other.health <= 0 ||
				(other.flags & kEntityFlagDead) != 0)
			return;
		const AiEntity *brain = world.ai.for_handle(other.handle);
		if (brain == nullptr)
			return;
		if ((brain->slot.f[38] == e.slot.f[38] &&
					(brain->slot.f[37] == 125 || command == 124 || command == 123)) ||
				other.ground_target == target.handle) {
			if (brain->inf.board_entry_slot < 32)
				claimed[brain->inf.board_entry_slot] = true;
		}
	});
	e.inf.board_entry_slot = 1;
	for (int i = count; i > 0; --i)
		if (!claimed[i])
			e.inf.board_entry_slot = static_cast<uint8_t>(i);
}
// Authored E/G/S/H points for non-PlayerControl targets. UseGun bypasses the
// staged walk. Radii are the original Q16 constants, including 57344/102400.
// [orig: Entity_UpdateInfantryAI @0x4BB373..0x4BB849]
void entry_goal(AiEntity &e, World &world, Entity &self, const Entity &target, int32_t goal[3],
		int32_t &radius) {
	auto &inf = e.inf;
	int32_t point[6] = {};
	const auto copy = [&] { std::copy_n(point, 3, goal); };
	if (named_point(world, target, "UseGun", point)) {
		copy();
		radius = target.primary_weapon_owner.valid() ? 0x30000 : 0x10000;
		return;
	}
	char name[] = { 'E', char('0' + inf.board_entry_slot), 0 };
	const auto get = [&](char prefix) {
		name[0] = prefix;
		return named_point(world, target, name, point);
	};
	const int stage = inf.board_entry_stage;
	if (stage > 6) {
		std::copy_n(e.pos, 3, goal);
		radius = 0x10000;
		--inf.board_entry_stage;
	} else if (stage > 4) {
		if (get('H')) {
			copy();
			radius = 0x10000;
			inf.board_entry_stage = 6;
		}
	} else if (stage > 1) {
		if (get('G')) {
			copy();
			radius = 0x10000;
			inf.board_entry_stage = 4;
			if (stage == 2)
				inf.board_anim = anim_state::kStop;
		} else if (get('S')) {
			const int32_t sx = point[0], sy = point[1], sz = point[2], yaw = point[3];
			inf.board_anim = anim_state::kStop;
			if (stage == 2 || stage == 3) {
				copy();
				radius = 102400;
				if (stage == 2) {
					inf.board_entry_stage = 3;
					if (get('E')) {
						e.pos[0] += (point[0] - e.pos[0]) >> 3;
						e.pos[1] += (point[1] - e.pos[1]) >> 3;
					}
				}
			} else {
				std::copy_n(e.pos, 3, goal);
				radius = 90112;
				inf.board_entry_stage = 3;
				inf.target_heading = yaw;
				e.heading = yaw;
				const int32_t diff =
						static_cast<int32_t>(uint32_t(yaw) - uint32_t(inf.body_heading));
				if (std::abs(int64_t(diff)) < 1073741760) {
					self.flags |= kEntityFlagMounted;
					// The guard clip (0x8C) is selected only when the body's
					// anim table maps state 140 to a clip of its own: the record
					// behind [[entity+0x188]+0x48] is a dword per state and the
					// test is [rec+0x230] != [rec+0] (0x230 = 4 * 0x8C), which is
					// the root-motion source's has_clip(kGuard). Retail also
					// stamps attachParent (+0x184) = self and the S position into
					// +0x2FC..+0x304 here; no consumer of either is witnessed
					// (the IDB tracks no reader), so they are not modeled.
					// [orig: Entity_UpdateInfantryAI @0x4BB818..0x4BB858]
					const IRootMotionSource *rm = world.ai.root_motion;
					if (rm == nullptr || rm->has_clip(inf.adm_id, anim_state::kGuard))
						inf.board_anim = anim_state::kGuard;
				}
				if (std::abs(int64_t(diff)) < 0x2D82D80) {
					e.pos[0] = sx;
					e.pos[1] = sy;
					e.pos[2] = std::max(e.pos[2], sz);
				}
			}
		}
	} else if (get('E')) {
		copy();
		radius = 57344;
		inf.board_entry_stage = 1;
		inf.board_anim = anim_state::kStop;
		const double dx = target.position.x * 65536.0 - e.pos[0];
		const double dy = target.position.y * 65536.0 - e.pos[1];
		const int32_t bound = board_to_fixed(target.bound_radius) + 0x10000;
		if (std::hypot(dx, dy) < bound && world.collision &&
				!world.collision->entity_los_clear(
						world, self.handle, self.handle, e.pos, goal, 0x4000, true)) {
			// Walk around the near side of the target's bound when the E point
			// is hidden by its hull. [orig: @0x4BB4A2..0x4BB591]
			const int32_t yaw =
					board_bearing_to(-static_cast<int32_t>(dx), -static_cast<int32_t>(dy));
			const double angle = yaw / io::kBamPerRadian;
			const int64_t c = static_cast<int32_t>(std::cos(angle) * 4194304.0);
			const int64_t sn = static_cast<int32_t>(std::sin(angle) * 4194304.0);
			goal[0] = board_to_fixed(target.position.x) + int32_t((bound * c) >> 22) +
					int32_t(((bound >> 1) * sn) >> 22);
			goal[1] = board_to_fixed(target.position.y) + int32_t((bound * sn) >> 22) -
					int32_t(((bound >> 1) * c) >> 22);
			goal[2] = board_to_fixed(target.position.z);
			radius = 0;
		}
	}
	self.position = { e.pos[0] / 65536.0f, e.pos[1] / 65536.0f, e.pos[2] / 65536.0f };
}
} // namespace

void AiSystem::infantry_command_think(AiEntity &e, World &world) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;
    const int32_t command = slot.f[37];

    if (command == 126) {
        // GOTO GROUP -> a stationary guard. Retail parks the shared gait select
        // at moveMode 3 with targetDist == arrivalRadius == 10.0u, which resolves
        // to no motion; our think contract encodes that outcome as the move_mode 0
        // the caller's per-think reset already left in place.
        // [orig: the ==126 leg @0x4baabd..0x4baacf — moveMode=3, dist=radius=0xA0000]
        return;
    }

    if (command == 127) {
        // FOLLOW THE LOCAL PLAYER. Arrive at 4.0u; when the AI's combat focus IS
        // the player (a guard order), the ring widens to max(slot[16], 4.0u);
        // inside the ring the move clears. [orig: the ==127 leg @0x4baad4.. —
        // target = g_local_player position @0x4baae5, radius 0x40000
        // @0x4bab60, focus compare aiComp[3] == g_local_player, radius
        // max(aiComp[16], 0x40000) @0x4bab77..0x4bab83]
        const Entity *player = world.registry.get(world.cached.local_player);
        if (player == nullptr) return;
        int32_t tgt[3] = {board_to_fixed(player->position.x),
                          board_to_fixed(player->position.y),
                          board_to_fixed(player->position.z)};
        const int32_t dist = board_dist(e.pos, tgt);
        int32_t radius = 0x40000;
        if (inf.combat_target.valid() &&
            inf.combat_target == world.cached.local_player)
            radius = std::max(slot.f[16], radius);
        if (dist < radius) return;
        inf.move_mode = 3;
        inf.target_dist = dist;
        inf.arrival_radius = radius;
        inf.move_target[0] = tgt[0];
        inf.move_target[1] = tgt[1];
        inf.move_target[2] = tgt[2];
        inf.target_heading = board_bearing_to(tgt[0] - e.pos[0], tgt[1] - e.pos[1]);
        return;
    }

    infantry_board_think(e, world, command);
}

void AiSystem::infantry_board_think(AiEntity &e, World &world, int32_t command) {
	auto &inf = e.inf;
	auto &slot = e.slot;
	// Cached board target and command dispatch [orig: @0x4BEE93..0x4BEEC6,
	// @0x4BEEA5..0x4BEEAF, @0x4BEEBA].
	const uint16_t ssn = static_cast<uint16_t>(slot.f[38]);
	const auto th = world.registry.find_by_net_id(ssn);
	Entity *target = world.registry.get(th);
	Entity *self = world.registry.get(e.handle);
	if (target == nullptr || self == nullptr) {
		slot.f[36] = 0;
		inf.board_blocked = false;
		return;
	}
	if (slot.f[36] != int32_t(th.packed) + 1)
		claim_entry(e, world, *target, command);
	slot.f[36] = int32_t(th.packed) + 1;
	if (self->mounted || !target->has_item_def)
		return;
	const bool pc = (target->item_attrib & kItemAttribPlayerControl) != 0;
	const bool entry_type = target->item_type == 1 || target->item_type == 6;
	if (entry_type && pc && !vehicle_can_enter(world, self, *target)) {
		if ((target->flags & kEntityFlagDead) != 0 || !target->alive || target->health <= 0) {
			const double dx = board_to_fixed(self->spawn_position.x) - int64_t(e.pos[0]);
			const double dy = board_to_fixed(self->spawn_position.y) - int64_t(e.pos[1]);
			const double dz = (board_to_fixed(self->spawn_position.z) - int64_t(e.pos[2])) >> 1;
			if (std::trunc(std::sqrt(dx * dx + dy * dy + dz * dz)) > 0x80000) {
				self->health = e.health = 0;
				self->last_attacker = target->last_attacker;
			}
		}
		// The live can't-enter arm still runs the common arrival tail: the goal
		// is the entity's own position with a 125 u ring (0x7D0000), so the
		// distance is zero and the arrival branch increments a nonzero entry
		// stage; the attach is skipped by the ring's >= 100 u (0x640000) gate.
		// [orig: goal = self @0x4BB2CE..0x4BB2E4, radius @0x4BB2D7 -> loc_4BB5A1;
		//  arrival @0x4BBD87..0x4BBDA0; attach gate @0x4BBDAF; flag clear
		//  @0x4BBDFA..0x4BBE07]
		if (inf.board_entry_stage)
			++inf.board_entry_stage;
		if (!self->mounted)
			self->flags &= ~kEntityFlagMounted;
		return;
	}
	int32_t goal[3] = { board_to_fixed(target->position.x), board_to_fixed(target->position.y),
		board_to_fixed(target->position.z) };
	// Arrival radius [orig: @0x4BB325..0x4BB34A, 2-unit arm @0x4BB32E].
	int32_t radius = board_to_fixed(target->bound_radius) + 0x10000;
	if (entry_type && pc) {
		VehicleSeatSelection best;
		if (find_best_vehicle_seat(world, th, e.handle, best, seat_mode_for_command(command))) {
			const Entity *carrier = world.registry.get(best.vehicle);
			if (carrier)
				seat_world_position(world, *carrier, carrier->seats[best.seat_index], goal);
		}
		// The latch is armed by the terrain-gradient shove, not a stalled walk.
		// Recover the exact integer neighbour differences from the shared normal
		// kernel. [orig: Terrain_GetHeightGradient @0x606330; @0x4BA85B]
		if (world.tables.terrain && ((self->flags | self->engine_flags) & 0x90A000u) == 0) {
			const auto n = terrain::height_field_surface_normal_world(
					*world.tables.terrain, e.pos[0] / 65536.0f, -e.pos[1] / 65536.0f);
			const double gx = std::round(-256.0 * n.x / n.up), gy = std::round(-256.0 * n.z / n.up);
			if (std::trunc(std::hypot(gx, gy)) >= 768)
				inf.board_blocked = true;
		}
		if (!inf.board_blocked)
			radius = 0x20000;
	} else if (entry_type) {
		entry_goal(e, world, *self, *target, goal, radius);
	}
	const int32_t dist = board_dist(e.pos, goal);
	// Attach gate and calls [orig: @0x4BBDA6..0x4BBE07; @0x4BBDAF,
	// @0x4BBDC4, @0x4BBDD4, @0x4BBDF2]; common move tail @0x4BBE11.
	if (dist < radius) {
		if (inf.board_entry_stage)
			++inf.board_entry_stage;
		// The arrival clears a frame LOCAL (var_1169 @0x4BBD8F), not the +0x369
		// path-state byte, so board_blocked (its model) stays set here; only the
		// unported cover/path consumer (ai_find_cover_position @0x4afab0, clear
		// within 1 u @0x4aff06) returns that byte to 0.
		if (radius < 0x640000 &&
				(target->item_attrib & (kItemAttribPlayerControl | kItemAttribEweap)) != 0)
			world.commands.mount_boarding_command(self->net_id, ssn, static_cast<uint8_t>(command));
		if (!self->mounted)
			self->flags &= ~kEntityFlagMounted;
		return;
	}
	inf.move_mode = 3;
	inf.target_dist = dist;
	inf.arrival_radius = radius;
	std::copy_n(goal, 3, inf.move_target);
	inf.at_final_oneshot = true;
	inf.target_heading = board_bearing_to(goal[0] - e.pos[0], goal[1] - e.pos[1]);
}

} // namespace opennova::world
