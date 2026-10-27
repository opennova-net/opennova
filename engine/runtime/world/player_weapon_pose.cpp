#include <runtime/world/player_weapon.h>
#include <runtime/world/angle.h>
#include <runtime/world/ai.h>
#include <runtime/world/local_player.h>
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
    out[0] = carrier.position.x.q16;
    out[1] = carrier.position.y.q16;
    out[2] = carrier.position.z.q16;
    const AiEntity *body = world.ai.for_handle(carrier.handle);
    out[3] = body ? body->heading : carrier.veh.yaw_seeded ? carrier.veh.yaw_bam
        : carrier.heading;
    out[4] = body ? body->pitch : carrier.veh.yaw_seeded ? carrier.veh.air_pitch_bam
        : carrier.pitch_bam;
    out[5] = body ? body->roll : carrier.veh.yaw_seeded ? carrier.veh.air_roll_bam
        : carrier.roll_bam;
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

// Entity_ComputeUserpointWorldTransform handed a record: the slot def picks the
// model and the gate byte as for a shot (a gfx3 def its model and launch byte, else
// the carrier's own model and its barrel's byte in the slot's column), and a gate
// that holds poses the record. The record is `point` of `record_model` (null = the
// carrier's own), posed on that same model; retail poses the carrier model's heat
// record on the gfx3 skeleton when a gfx3 def reaches this through a UseGun seat. The
// raw leg (a zero gate) takes no anchor: retail copies the carrier's origin there and
// leaves the direction local unwritten.
// [orig: Entity_ComputeWeaponFireTransform @0x4011F0 (the record test @0x40123E, the
//  world-space call @0x401299, the position/direction copies @0x4012A9..0x4012D7);
//  Entity_ComputeUserpointWorldTransform @0x545C60 (the model and gate
//  @0x545D06..0x545D55, the gate test @0x545D68, the record arg @0x545D6E, the pose
//  @0x545E08)]
bool heat_glow_point(World &world, const Entity &carrier, const WeaponTableEntry &def,
                     const WeaponFsmEvents &leg, const threedi::Threedi3di3 *record_model,
                     int point, int32_t pos[3], int32_t dir[3]) {
    if (point == 0 || world.pose_provider == nullptr) return false;
    const uint8_t gate = def.third_person_model_asset != nullptr
            ? def.launch_userpoint
            : weapon_userpoint_byte(carrier, int(uint32_t(leg.heat_glow_clip) & 3u),
                                    weapon_userpoint_field(leg.heat_glow_current,
                                                           leg.heat_glow_next));
    int32_t out[6];
    if (gate == 0 || !world.pose_provider->resolve_userpoint_frame(world, carrier.handle,
                record_model, point, out, dir))
        return false;
    std::copy_n(out, 3, pos);
    return true;
}
} // namespace

HeatGlowWalk heat_glow_walk_of(const World &world, const Entity &walked) {
    HeatGlowWalk walk;
    if (walked.has_item_def && (walked.item_attrib & kItemAttribEweap) != 0) {
        walk.gun = &walked;
        return walk;
    }
    walk.carrier = walked.mounted ? world.registry.get(walked.mount_target) : nullptr;
    walk.local = walked.handle == world.cached.local_player;
    walk.first_person = walk.local && world.local_player_state != nullptr &&
            world.local_player_state->view.camera_mode == 0;
    walk.usegun_seat = walked.mounted && walked.mount_type == SeatType::Gunner;
    return walk;
}

// The anchor by the walked entity:
// - an EWeap walked as itself: no gfx3, its own heat point; a gfx3 def, OVERHEATED's
//   gfx3 point (the FP bit, which needs the local player riding the gun, never holds
//   on the pool-1 walk's unoccupied gun);
// - the local player in first person: a gfx1 def takes the camera-view leg; with none,
//   a UseGun seat takes its carrier's heat point, any other seat no anchor;
// - any other body: a gfx3 def takes the camera-view leg, on foot no anchor, else the
//   carrier's heat point.
// The spawn carries the anchor's entity as its tag and the fraction in descriptor
// +40/+44; each later hot tick re-aims and re-intensifies the live emitter.
// [orig: WeaponAction_ProcessFrame @0x540E60 - the FP bit @0x540E8C..0x540ECF; the
//  release @0x541231..0x541241 / @0x54124A..0x54125C; the OVERHEATED row @0x5410A7; the
//  EWeap branch @0x5410BC..0x5410DF; the local first-person branch @0x541113..0x541164;
//  the third-person / other-body branches @0x541166..0x5411B9; the shared carrier leg
//  @0x54119E..0x5411B2; the descriptor +0 / +0x0C / +40 / +44 @0x5411F0..0x541200; the
//  update CEffectEmitter_UpdatePositionAndParams @0x541213, the spawn
//  CEffectWorld_SpawnEmitterAtPosition into the +0x1C handle @0x54122C;
//  Entity_GetWeaponBoneByte @0x545A00 (index 12)]
void weapon_heat_glow_update(World &world, const HeatGlowWalk &walk, const WeaponTableEntry &def,
                             WeaponSlotState &slot, const WeaponFsmEvents &events) {
    if (events.heat_glow_release) {
        slot.heat_glow = {};
        return;
    }
    if (events.heat_glow_fraction == 0) return;
    const threedi::Threedi3di3 *gfx3 = def.third_person_model_asset.get();
    const Entity *anchor = nullptr;
    const threedi::Threedi3di3 *record_model = nullptr;
    int point = 0;
    if (walk.gun != nullptr) {
        anchor = walk.gun;
        if (gfx3 != nullptr) {
            record_model = gfx3;
            point = def.action_fsm.actions[weapon_action::kOverheated].particle_userpoint_3p;
        } else {
            point = weapon_heat_userpoint_byte(*anchor);
        }
    } else if (walk.local && walk.first_person) {
        if (def.has_first_person_model_reference || !walk.usegun_seat || walk.carrier == nullptr)
            return;
        anchor = walk.carrier;
        point = weapon_heat_userpoint_byte(*anchor);
    } else {
        if (gfx3 != nullptr || walk.carrier == nullptr) return;
        anchor = walk.carrier;
        point = weapon_heat_userpoint_byte(*anchor);
    }
    int32_t pos[3], dir[3];
    if (!heat_glow_point(world, *anchor, def, events, record_model, point, pos, dir)) return;
    slot.heat_glow.live = true;
    slot.heat_glow.fraction = events.heat_glow_fraction;
    slot.heat_glow.tag = anchor->handle.packed;
    std::copy_n(pos, 3, slot.heat_glow.pos);
    std::copy_n(dir, 3, slot.heat_glow.dir);
}

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
// [orig: select @0x545D17..0x545D3F; WeaponAction_Fire @0x542B10 makes the
//  Entity_CalcWeaponFirePosition call @0x542BF7 before next = RECOIL @0x542C9E]
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

void controller_fire_pose(World &world, Entity &carrier, const WeaponTableEntry *fired,
                          int32_t clip_before_consume, int32_t out[6]) {
    // The controller helper poses the EWEAP carrier with its view tilt folded
    // in, and asks for the point's direction, which turns the reported euler
    // toward the authored direction.
    // [orig: Entity_ComputeUserpointTransform @0x4DC83A, outDirection
    //  @0x4DC829]
    int32_t direction[3];
    carrier_weapon_local_pose(world, carrier, fired, clip_before_consume, out, direction);
}

// [orig: Entity_CheckWeaponSeatFlags @0x540D00 -- an OnlyScoped weapon held by
//  the local player answers no flag query until g_WeaponScopeActive]
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
        controller_fire_pose(world, *mount, fired, clip_before_consume, out);
        return;
    }
    // On-foot position + CameraOffset and undoubled recoil pitch.
    // [orig: @0x4DC847..0x4DC880]
    const AiEntity *body = world.ai.for_handle(shooter->handle);
    out[0] = io::bam_add(body ? body->self->position.x.q16 : shooter->position.x.q16, body ? body->inf.eye_offset_x : shooter->eye_offset_x);
    out[1] = io::bam_add(body ? body->self->position.y.q16 : shooter->position.y.q16, body ? body->inf.eye_offset_y : shooter->eye_offset_y);
    out[2] = io::bam_add(body ? body->self->position.z.q16 : shooter->position.z.q16, body ? body->inf.eye_offset_z : shooter->eye_offset_z);
    out[3] = body ? body->heading : shooter->heading;
    out[4] = body ? io::bam_add(body->pitch, body->inf.recoil_pitch) : shooter->pitch_bam;
    out[5] = body ? body->roll : shooter->roll_bam;
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
