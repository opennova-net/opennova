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
    out[4] = body ? body->pitch : carrier.veh.yaw_seeded ? carrier.veh.air_pitch_bam
        : bam_from_degrees_wrapped(carrier.pitch);
    out[5] = body ? body->roll : carrier.veh.yaw_seeded ? carrier.veh.air_roll_bam
        : bam_from_degrees_wrapped(carrier.roll);
}

// The local-space helper the G-redirect arm and the controller branch share:
// the weapon point with an EWEAP carrier's view tilt (+0x45C) folded into its
// Pitch while the matrix is built, only once a point resolves; the raw leg
// copies the unfolded pose. At the fire tick the column select yields 0.
// [orig: Entity_ComputeUserpointTransform @0x545A40 -- Def+0x54 & 0x20 test
//  @0x545BAB..0x545BB7, Pitch += +0x45C @0x545BB9..0x545BBF, restored
//  @0x545C03..0x545C09 behind the same test @0x545BF4..0x545C01; raw copy
//  @0x545C14]
// Only a motorized carrier accumulates the tilt word; its placement pitch is
// the motor's BAM mirror.
void carrier_weapon_local_pose(World &world, Entity &carrier, const WeaponTableEntry *fired,
                               int32_t clip, int32_t out[6], int32_t out_direction[3]) {
    const bool fold = carrier.has_item_def && (carrier.item_attrib & kItemAttribEweap) != 0 &&
            carrier.veh.yaw_seeded;
    const int32_t saved_air_pitch = carrier.veh.air_pitch_bam;
    if (fold) carrier.veh.air_pitch_bam = io::bam_add(saved_air_pitch, carrier.veh.view_tilt_bam);
    const bool posed = carrier_weapon_userpoint(world, carrier, fired, clip, 0, out, out_direction);
    carrier.veh.air_pitch_bam = saved_air_pitch;
    if (!posed) carrier_pose(world, carrier, out);
}
} // namespace

bool carrier_weapon_userpoint(World &world, const Entity &carrier, const WeaponTableEntry *fired,
                              int32_t clip, int column, int32_t out[6],
                              int32_t out_direction[3]) {
    // No slot def: the raw leg. [orig: slot->Def @0x545CC6..0x545CCB /
    //  @0x545AAC..0x545AB1]
    if (fired == nullptr || world.pose_provider == nullptr) return false;
    // A gfx3 def fires from its resolved launch point on that model, posed
    // through the carrier; the slot bytes and the column are never read.
    // [orig: +0x170 @0x545D06..0x545D0E, +0x2D4 @0x545D55, record from the
    //  gfx3 table @0x545D76..0x545D85, modelData = gfx3 @0x545DF8]
    if (fired->third_person_model_asset != nullptr)
        return fired->launch_userpoint != 0 &&
                world.pose_provider->resolve_userpoint_frame(world, carrier.handle,
                        fired->third_person_model_asset.get(), fired->launch_userpoint, out,
                        out_direction);
    // Otherwise the carrier's own byte for the barrel the clip selects.
    // [orig: Entity_GetWeaponSlotByte(entity, slot[+0x10] & 3, col)
    //  @0x545D40..0x545D4B / @0x545B3F..0x545B4B]
    const uint8_t point = weapon_userpoint_byte(carrier, int(uint32_t(clip) & 3u), column);
    return point != 0 && world.pose_provider->resolve_userpoint_frame(world, carrier.handle,
            nullptr, point, out, out_direction);
}

bool carrier_weapon_world_pose(World &world, const Entity &carrier, const WeaponTableEntry *fired,
                               int32_t clip, int column, int32_t out[6],
                               int32_t out_direction[3]) {
    if (carrier_weapon_userpoint(world, carrier, fired, clip, column, out, out_direction))
        return true;
    carrier_pose(world, carrier, out);
    return false;
}

// The fire tick reaches either helper with currentAction FIRE and nextAction
// 0, so the column select yields the FIRE field (0).
// [orig: select @0x545D17..0x545D3F; WeaponAction_Fire calls
//  Entity_CalcWeaponFirePosition @0x542BF7 before next = RECOIL @0x542C9E]
void usegun_fire_pose(World &world, const Entity &gun, const WeaponTableEntry *fired,
                      int32_t clip_before_consume, int32_t out[6]) {
    // A G-attached gun routed to its parent slot fires from the HULL's
    // userpoint table, not the child gun's.
    // [orig: parent+0x326 & 2 @0x4DC7A9, parent+0x312 & 8 @0x4DC7B2,
    //  parent->groundEntity @0x4DC7CD -> Entity_ComputeUserpointTransform
    //  @0x4DC7D9 (no direction); otherwise Entity_ComputeUserpointWorldTransform
    //  @0x4DC7E6..0x4DC7F6, the raw copy @0x545E1F]
    if ((gun.emplacement_attachment_flags & 2u) != 0 &&
            gun.primary_weapon_slot.redirect_to_parent_slot) {
        if (Entity *hull = world.registry.get(gun.ground_target)) {
            carrier_weapon_local_pose(world, *hull, fired, clip_before_consume, out, nullptr);
            return;
        }
    }
    carrier_weapon_world_pose(world, gun, fired, clip_before_consume, 0, out);
}

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
    // The fired slot's def: the weapon the shooter holds (a borrowed UseGun
    // slot's is the gun's own). [orig: MountSlot+0x20 read @0x545CC6 /
    //  @0x545AAC]
    const WeaponTableEntry *fired = world.tables.weapons.by_index(shooter->equipped_adm_index);
    if (mount && shooter->mount_type == SeatType::Gunner) {
        usegun_fire_pose(world, *mount, fired, clip_before_consume, out);
        return;
    }
    if (mount && shooter->mount_type == SeatType::Controller &&
            (mount->item_attrib & kItemAttribEweap) != 0) {
        // The controller helper poses the EWEAP carrier with its view tilt
        // folded in, and asks for the point's direction, which turns the
        // reported euler toward the authored direction.
        // [orig: Entity_ComputeUserpointTransform @0x4DC83A, outDirection
        //  @0x4DC829]
        int32_t direction[3];
        carrier_weapon_local_pose(world, *mount, fired, clip_before_consume, out, direction);
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
