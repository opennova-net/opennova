#include <runtime/world/player_weapon.h>
#include <runtime/world/angle.h>
#include <runtime/world/ai.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <algorithm>

namespace opennova::world {
// [orig: Entity_CalcWeaponFirePosition @0x4DC750; mounted helper @0x545C60;
// gunner branch @0x4DC789, controller branch @0x4DC7E5. Aim acquisition
// reaches the same mounted seed @0x4B4F35.]
void local_weapon_fire_pose(World &world, const LocalPlayerWeapon &weapon,
                           int32_t clip_before_consume, int32_t out[6]) {
    std::fill_n(out, 6, 0);
    const Entity *shooter = world.registry.get(world.cached.local_player);
    if (!shooter) return;
    const Entity *mount = world.registry.get(shooter->mount_target);
    if (mount && (shooter->mount_type == SeatType::Gunner ||
            (shooter->mount_type == SeatType::Controller && (mount->item_attrib & kItemAttribEweap)))) {
        const uint8_t point = weapon_userpoint_byte(*mount, uint32_t(clip_before_consume) & 3u, 0);
        if (point && world.pose_provider && world.pose_provider->resolve_userpoint_transform(
                world, mount->handle, point, out)) return;
        // No userpoint/model: copy the carrier pose, never the gunner's eye.
        // [orig: Entity_ComputeUserpointWorldTransform @0x545E1F]
        out[0] = to_fixed(mount->position.x); out[1] = to_fixed(mount->position.y);
        out[2] = to_fixed(mount->position.z);
        const AiEntity *body = world.ai.for_handle(mount->handle);
        out[3] = body ? body->heading : mount->veh.yaw_seeded ? mount->veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(mount->yaw);
        out[4] = body ? body->pitch : bam_from_degrees_wrapped(mount->pitch);
        out[5] = body ? body->roll : bam_from_degrees_wrapped(mount->roll);
        return;
    }
    // On-foot position + CameraOffset and undoubled recoil pitch.
    // [orig: @0x4DC847..0x4DC880]
    const AiEntity *body = world.ai.for_handle(shooter->handle);
    out[0] = io::bam_add(body ? body->pos[0] : to_fixed(shooter->position.x), body ? body->inf.eye_offset_x : shooter->eye_offset_x);
    out[1] = io::bam_add(body ? body->pos[1] : to_fixed(shooter->position.y), body ? body->inf.eye_offset_y : shooter->eye_offset_y);
    out[2] = io::bam_add(body ? body->pos[2] : to_fixed(shooter->position.z), body ? body->inf.eye_offset_z : shooter->eye_offset_z);
    out[3] = body ? body->heading : bam_heading_from_mission_yaw_deg(shooter->yaw);
    out[4] = body ? io::bam_add(body->pitch, body->inf.recoil_pitch) : bam_from_degrees_wrapped(shooter->pitch);
    out[5] = body ? body->roll : bam_from_degrees_wrapped(shooter->roll);
    // Add the minimum + local elevation offset, then clamp to authored limits.
    // [orig: @0x4DC8A3..0x4DC8EF]
    if (weapon.active && (weapon.def.flags & def::DEF_WEAPON_FLAG_ABSORBPITCH)) {
        out[4] = std::max(weapon.pitch_min_bam, std::min(weapon.pitch_max_bam,
            io::bam_add(out[4], io::bam_add(weapon.pitch_min_bam, weapon.pitch_offset_bam))));
    }
    // Designator rounds originate at the measured aim point only while firing.
    // [orig: @0x4DC8F7..0x4DC939; action gate @0x4DC91A, copy @0x4DC92C]
    const auto *slot = active_local_weapon_slot(world, weapon);
    const auto *entry = world.tables.weapons.by_index(shooter->equipped_adm_index);
    const auto *ammo = entry ? world.tables.ammo.by_index(entry->ammo_index) : nullptr;
    if (body && ammo && (ammo->flags & def::DEF_AMMO_FLAG_DESIGNATETARGET) && slot->current == weapon_action::kFire)
        std::copy_n(body->inf.aim_point, 3, out);
}
} // namespace opennova::world
