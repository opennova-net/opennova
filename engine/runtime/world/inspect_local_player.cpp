#include <runtime/world/inspect_local_player.h>

#include <runtime/world/local_player.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

namespace opennova::world::inspect {

bool local_player_report(const World &world, const LocalPlayer &local, const CollisionWorld *collision,
		LocalPlayerReport &out) {
	out = LocalPlayerReport{};
	const Entity *e = local.player();
	if (e == nullptr) return false;
	out.valid = true;
	out.handle = e->handle.packed;
	out.name = e->name;
	out.team = static_cast<int32_t>(e->team);
	out.health = e->health;
	out.health_max = e->health_max;
	out.alive = e->alive;
	out.hidden = e->hidden;
	out.dead = local.local_player_dead();
	out.position = e->position;
	out.yaw_deg = e->yaw;
	out.pitch_deg = e->pitch;
	out.roll_deg = e->roll;
	out.stance = local.stance_latch();
	out.anim_key = local.player_anim_key();
	out.motor_speed_q16 = e->veh.speed;
	out.velocity_q16[0] = e->veh.vel_x;
	out.velocity_q16[1] = e->veh.vel_y;
	out.velocity_q16[2] = e->veh.slide_z;
	out.mounted = e->mounted;
	out.mount_target = e->mount_target.packed;
	out.mount_seat = e->mount_seat;
	out.camera_mode = local.view.camera_mode;
	out.third_person = local.view.third_person;
	out.scope_engaged = local.view.scope_engaged;
	out.scope_settled = local.view.scope_settled;
	out.weapon = local.weapon.def_name;
	out.weapon_action = local.weapon.slot.current;
	if (out.weapon_action >= 0 && out.weapon_action < weapon_action::kCount) {
		out.weapon_action_name = kWeaponActionSuffixes[out.weapon_action];
	}
	out.clip = local.weapon.slot.clip;
	out.clip_capacity = local.weapon.def.clip_capacity;
	out.reserve = local.weapon.slot.reserve;
	out.medic_cooldown_ticks = local.medic_request_cooldown_ticks;
	if (collision != nullptr) out.resolve = collision->local_resolve_debug;
	(void)world;
	return true;
}

}  // namespace opennova::world::inspect
