#include <base/io/bam.h>
#include <cmath>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/hud_combat_feed.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/world.h>
namespace opennova::world {
// [orig: draw_weapon_sight_crosshair @0x59ECA0; HUD_draw_crosshair @0x59EA20]
void fill_hud_vehicle_sights(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view) {
	auto &v = view.hud_combat;
	auto &s = v.state;
	const Entity *player = world.registry.get(world.cached.local_player);
	Entity *mount = player ? world.registry.get(player->mount_target) : nullptr;
	if (!player || !mount || !mount->has_item_def || !weapon.active || s.dead)
		return;
	const int32_t root[3] = { to_fixed(mount->position.x), to_fixed(mount->position.y),
		to_fixed(mount->position.z) };
	const int32_t yaw = mount->veh.yaw_seeded ? mount->veh.yaw_bam
											  : bam_heading_from_mission_yaw_deg(mount->yaw);
	const int32_t pitch = bam_from_degrees_wrapped(mount->pitch),
				  roll = bam_from_degrees_wrapped(mount->roll);
	if ((mount->item_attrib & kItemAttribEweap) && player->mount_type == SeatType::Controller &&
			view.camera_mode == 0) {
		const double angle = double(mount->veh.view_tilt_bam) * io::kRadiansPerBam;
		const int32_t local[] = { int32_t((65536000LL * int32_t(std::cos(angle) * 4194304)) >> 22),
			0, int32_t((65536000LL * int32_t(std::sin(angle) * 4194304)) >> 22) };
		collision_matrix_from_euler(yaw, pitch, roll, root)
				.transform_point(local, v.vehicle_fixed.data());
		v.vehicle_fixed_valid = s.vehicle_fixed = true;
	}
	if (mount->item_attrib2 & 0x1000u) {
		const auto *body = world.ai.for_handle(player->handle);
		const int32_t player_yaw =
				body ? body->heading : bam_heading_from_mission_yaw_deg(player->yaw);
		const int32_t player_pitch = body ? body->pitch : bam_from_degrees_wrapped(player->pitch);
		const uint16_t desired_yaw = uint16_t(uint32_t(io::bam_sub(yaw, player_yaw)) >> 16);
		const uint16_t desired_pitch = uint16_t(uint32_t(io::bam_sub(pitch, player_pitch)) >> 16);
		const int16_t dy = int16_t(desired_yaw - uint16_t(mount->emplaced_gun_yaw_word));
		const int16_t dp = int16_t(desired_pitch - uint16_t(mount->emplaced_gun_pitch_word));
		s.vehicle_lag = true;
		s.vehicle_lag_center = std::abs(int32_t(dy)) <= 182 && std::abs(int32_t(dp)) <= 182;
		if (!s.vehicle_lag_center) {
			int32_t pose[6];
			const auto *slot = active_local_weapon_slot(world, weapon);
			// Re-evaluate the entire articulated userpoint, including its
			// position around the part pivot, then restore the simulation pose.
			// [orig: @0x59EA57..0x59EAF3]
			const auto saved_yaw = mount->emplaced_gun_yaw_word;
			const auto saved_pitch = mount->emplaced_gun_pitch_word;
			mount->emplaced_gun_yaw_word = int16_t(desired_yaw);
			mount->emplaced_gun_pitch_word = int16_t(desired_pitch);
			local_weapon_fire_pose(world, weapon, slot->clip, pose);
			mount->emplaced_gun_yaw_word = saved_yaw;
			mount->emplaced_gun_pitch_word = saved_pitch;
			if (weapon.def.scope_zero.max_steps) {
				pose[3] = io::bam_add(pose[3], slot->zero_yaw);
				pose[4] = io::bam_sub(pose[4], slot->zero_pitch);
			}
			const int32_t forward[] = { 65536000, 0, 0 };
			collision_matrix_from_euler(pose[3], pose[4], pose[5], pose)
					.transform_point(forward, v.vehicle_lag.data());
			v.vehicle_lag_valid = true;
		}
	}
}
} // namespace opennova::world
