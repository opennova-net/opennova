#include <runtime/world/player_weapon.h>
#include <runtime/world/angle.h>
#include <runtime/world/ai.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <algorithm>

namespace opennova::world {
namespace {
// No userpoint/model: copy the carrier pose, never the gunner's eye.
// [orig: Entity_ComputeUserpointWorldTransform @0x545E1F..0x545E4C;
//  Entity_ComputeUserpointTransform @0x545C1B..0x545C41]
void carrier_pose(const World &world, const Entity &carrier, int32_t out[6]) {
    out[0] = to_fixed(carrier.position.x);
    out[1] = to_fixed(carrier.position.y);
    out[2] = to_fixed(carrier.position.z);
    const AiEntity *body = world.ai.for_handle(carrier.handle);
    out[3] = body ? body->heading : carrier.veh.yaw_seeded ? carrier.veh.yaw_bam
        : bam_heading_from_mission_yaw_deg(carrier.yaw);
    out[4] = body ? body->pitch : bam_from_degrees_wrapped(carrier.pitch);
    out[5] = body ? body->roll : bam_from_degrees_wrapped(carrier.roll);
}

// The weapon userpoint of `carrier` for the barrel the pre-consumption clip
// selects, column 0. [orig: Entity_GetWeaponSlotByte(entity, slot[4] & 3, col)
//  @0x545D4B / @0x545B4B]
bool carrier_weapon_pose(World &world, const Entity &carrier,
                         int32_t clip_before_consume, int32_t out[6]) {
    const uint8_t point = weapon_userpoint_byte(carrier, uint32_t(clip_before_consume) & 3u, 0);
    if (point && world.pose_provider && world.pose_provider->resolve_userpoint_transform(
            world, carrier.handle, point, out)) return true;
    carrier_pose(world, carrier, out);
    return false;
}
} // namespace

// [orig: Entity_CheckWeaponSeatFlags @0x540D00 -- an OnlyScoped weapon held by
//  the local player answers no flag query until g_weaponScopeActive]
bool local_weapon_seat_flag(const LocalPlayerWeapon &weapon, bool scope_settled, uint32_t mask) {
    if (!weapon.active) return false;
    if ((weapon.def.flags & def::DEF_WEAPON_FLAG_ONLYSCOPED) != 0 && !scope_settled) return false;
    return (weapon.def.flags & mask) != 0;
}

// [orig: Entity_CalcWeaponFirePosition @0x4DC750 -- gunner branch
//  @0x4DC7A0..0x4DC802 (G-redirect arm @0x4DC7A9..0x4DC7E5), controller branch
//  @0x4DC803..0x4DC846, on-foot leg @0x4DC847..0x4DC937. The slot-flag & 1
//  replay arm @0x4DC75C..0x4DC789 returns the slot's stored position; the wire
//  replay sets that bit only transiently, so the local pump never takes it.]
void local_weapon_fire_pose(World &world, const LocalPlayerWeapon &weapon,
                           int32_t clip_before_consume, bool scope_settled, int32_t out[6]) {
    std::fill_n(out, 6, 0);
    const Entity *shooter = world.registry.get(world.cached.local_player);
    if (!shooter) return;
    Entity *mount = world.registry.get(shooter->mount_target);
    if (mount && shooter->mount_type == SeatType::Gunner) {
        // A G-attached gun routed to its parent slot fires from the HULL's
        // userpoint table, not the child gun's.
        // [orig: parent+0x326 & 2 @0x4DC7A9, parent+0x312 & 8 @0x4DC7B2,
        //  parent->groundEntity @0x4DC7CD -> Entity_ComputeUserpointTransform
        //  @0x545A40; otherwise Entity_ComputeUserpointWorldTransform @0x4DC7F6]
        if ((mount->emplacement_attachment_flags & 2u) != 0 &&
                mount->primary_weapon_slot.redirect_to_parent_slot) {
            if (const Entity *hull = world.registry.get(mount->ground_target)) {
                carrier_weapon_pose(world, *hull, clip_before_consume, out);
                return;
            }
        }
        carrier_weapon_pose(world, *mount, clip_before_consume, out);
        return;
    }
    if (mount && shooter->mount_type == SeatType::Controller &&
            (mount->item_attrib & kItemAttribEweap) != 0) {
        // The controller helper poses the EWEAP carrier with its view tilt
        // folded into Pitch for the duration of the matrix build.
        // [orig: Entity_ComputeUserpointTransform @0x545A40 -- Pitch += +0x45C
        //  @0x545BB9..0x545BBF, restored @0x545C03..0x545C09]
        // Only a motorized carrier accumulates the tilt word; its placement
        // pitch is the motor's BAM mirror.
        const int32_t saved_air_pitch = mount->veh.air_pitch_bam;
        if (mount->veh.yaw_seeded)
            mount->veh.air_pitch_bam = io::bam_add(saved_air_pitch, mount->veh.view_tilt_bam);
        carrier_weapon_pose(world, *mount, clip_before_consume, out);
        mount->veh.air_pitch_bam = saved_air_pitch;
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
    // Add the minimum + local elevation offset, then the banded limit clamp.
    // [orig: seat-flag gate @0x4DC8A3; add @0x4DC8DE..0x4DC8EC;
    //  Math_ClampAngleToBounds @0x4DC8EF]
    if (local_weapon_seat_flag(weapon, scope_settled, def::DEF_WEAPON_FLAG_ABSORBPITCH)) {
        out[4] = io::bam_add(out[4], io::bam_add(weapon.pitch_min_bam, weapon.pitch_offset_bam));
        emplaced_clamp_turret_bam(out[4], weapon.pitch_max_bam, weapon.pitch_min_bam);
    }
    // Designator rounds originate at the measured aim point only while firing.
    // [orig: ammo flag @0x4DC90D, action gate @0x4DC916..0x4DC91A, copy
    //  @0x4DC91C..0x4DC937 (first store @0x4DC92C)]
    const auto *slot = active_local_weapon_slot(world, weapon);
    const auto *entry = world.tables.weapons.by_index(shooter->equipped_adm_index);
    const auto *ammo = entry ? world.tables.ammo.by_index(entry->ammo_index) : nullptr;
    if (body && ammo && (ammo->flags & def::DEF_AMMO_FLAG_DESIGNATETARGET) && slot->current == weapon_action::kFire)
        std::copy_n(body->inf.aim_point, 3, out);
}
} // namespace opennova::world
