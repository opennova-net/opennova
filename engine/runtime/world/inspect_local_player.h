// world::inspect::local_player_report — the local player's live facts for
// the F3 Player window (ADR 0042 d5, the one engine function per fact): the
// body (position, orientation, stance, health, the mount), the view (the
// camera mode, third person, the scope), the equipped weapon (its action,
// clip, reserve) and the movement resolver's last capsule readout.
#pragma once

#include <runtime/world/collision.h>
#include <runtime/world/geom.h>

#include <cstdint>
#include <string>

namespace opennova::world {

class World;
class LocalPlayer;

namespace inspect {

struct LocalPlayerReport {
	bool valid = false;
	uint16_t handle = EntityHandle::kInvalid;
	std::string name;
	int32_t team = -1;
	int32_t health = 0;
	int32_t health_max = 0;
	bool alive = false;
	bool hidden = false;
	bool dead = false;
	Vec3 position{};        // mission space
	int32_t yaw_deg = 0;    // Entity::yaw (whole mission degrees)
	int32_t pitch_deg = 0;
	int32_t roll_deg = 0;
	int32_t stance = 0;     // 0 stand, 1 crouch, 2 prone (the sim's latch)
	std::string anim_key;
	int32_t motor_speed_q16 = 0;
	int32_t velocity_q16[3] = {}; // the motor's world velocity (x, y) and vertical slide
	bool mounted = false;
	uint16_t mount_target = EntityHandle::kInvalid;
	int32_t mount_seat = -1;
	// The view.
	int32_t camera_mode = 0;
	bool third_person = false;
	bool scope_engaged = false;
	bool scope_settled = false;
	// The equipped weapon.
	std::string weapon;
	int32_t weapon_action = 0;
	std::string weapon_action_name;
	int32_t clip = 0;
	int32_t clip_capacity = 0;
	int32_t reserve = 0;
	int32_t medic_cooldown_ticks = 0;
	// The movement resolver's last pass for this body (collision.h).
	CollisionWorld::LocalResolveDebug resolve{};
};

// False (and an invalid report) without a local player.
bool local_player_report(const World &world, const LocalPlayer &local, const CollisionWorld *collision,
		LocalPlayerReport &out);

}  // namespace inspect
}  // namespace opennova::world
