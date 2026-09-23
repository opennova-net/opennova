#include <runtime/world/vehicle_system.h>

#include <runtime/world/world.h>
#include <runtime/devtools/tick_profile.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/mount_controls.h> // emplaced_gun_frame_heading, emplaced_word_bam
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_inventory.h> // weapon_slot_initial_zoom

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

namespace opennova::world {

static void emit_vehicle_control(World &world, const char *kind, uint16_t target_net_id,
                                 int32_t target_bms_id, uint32_t target_spawn_origin,
                                 uint16_t target_wire_handle) {
    Effect effect;
    effect.kind = kind;
    effect.a = static_cast<int32_t>(target_net_id);
    effect.b = target_bms_id;
    effect.c = static_cast<int32_t>(target_spawn_origin);
    effect.d = static_cast<int32_t>(target_wire_handle);
    world.out.effects.push(std::move(effect));
}

void VehicleSystem::emit_control_started(const Entity &vehicle) {
    World &world = world_;
    emit_vehicle_control(world, "vehicle_control_started", vehicle.net_id, vehicle.bms_id,
                         vehicle.spawn_origin, vehicle.handle.packed);
}

void VehicleSystem::emit_control_stopped(const Entity &vehicle) {
    World &world = world_;
    world.vehicles.emit_control_stopped(vehicle.net_id, vehicle.bms_id, vehicle.spawn_origin,
                                 vehicle.handle.packed);
}

void VehicleSystem::emit_control_stopped(uint16_t target_net_id, int32_t target_bms_id, uint32_t target_spawn_origin, uint16_t target_wire_handle) {
    World &world = world_;
    emit_vehicle_control(world, "vehicle_control_stopped", target_net_id, target_bms_id,
                         target_spawn_origin, target_wire_handle);
}

bool VehicleSystem::claim_primary_occupant(Entity &vehicle, EntityHandle occupant, SeatType seat) {
    World &world = world_;
    // [orig: Entity_AttachToVehicleSlot @0x4946d0] ctrlx(2)/drvrx(5) claim +368 when it is
    // empty or already theirs (@0x4947b3..0x4947d2 / @0x4948b9..0x4948d8); UseGun(3) claims
    // only when empty (@0x494944..0x49495e); sitex passengers never touch +368.
    const bool was_empty = !vehicle.primary_occupant.valid();
    switch (seat) {
        case SeatType::Controller:
        case SeatType::Driver:
            if (!was_empty && vehicle.primary_occupant != occupant) return false;
            break;
        case SeatType::Gunner:
            if (!was_empty) return vehicle.primary_occupant == occupant;
            break;
        default:
            return false;
    }
    vehicle.primary_occupant = occupant;
    // The empty -> claimed edge is the retail engine-start edge (the per-tick spawner
    // fires once its latch sees +368 set) [orig: @0x48faad..0x48fb0c].
    if (was_empty) world.vehicles.emit_control_started(vehicle);
    return true;
}

bool VehicleSystem::release_primary_occupant(Entity &vehicle, EntityHandle occupant) {
    World &world = world_;
    // [orig: Entity_DetachFromVehicle @0x4355f0] the stop leg runs ONLY when the detaching
    // entity IS the claimant (@0x4356e9); anyone else leaving — including a second control
    // occupant — leaves the latch untouched.
    if (!vehicle.primary_occupant.valid() || vehicle.primary_occupant != occupant)
        return false;
    // A PlayerControl vehicle's claimant leaving cuts the running action of
    // its vehicle weapon slot short: a nonzero MountSlot counter drops to 7.
    // [orig: Def gate @0x4356D0, attrib 0x40 @0x4356EF..0x4356F4, +0x474
    //  counter @0x4356F6..0x4356FF; the slot is Entity_GetWeaponSlots
    //  @0x5460FA's vehicle MountSlot]
    if (vehicle.has_item_def && (vehicle.item_attrib & kItemAttribPlayerControl) != 0 &&
            vehicle.primary_weapon_slot.counter != 0)
        vehicle.primary_weapon_slot.counter = 7;
    // The PlayerControl leg: the all-zero fold and the stop on the departing
    // occupant, then the +0x1CC smoke emitter release; the next mover tick
    // re-arms the smoke while the damage band still holds.
    // [orig: Entity_DetachFromVehicle @0x4356EF..0x435759]
    world.vehicles.play_claimant_detach_sound(vehicle, world.registry.get(occupant));
    if (const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
            traits != nullptr && traits->player_control)
        detail::vehicle_smoke_effect(world, vehicle, /*release=*/true);
    vehicle.primary_occupant = EntityHandle{};
    world.vehicles.emit_control_stopped(vehicle);
    return true;
}

bool VehicleSystem::prepare_weapon_slot(Entity &vehicle) {
    World &world = world_;
    const int weapon_index = world.tables.weapons.index_of(vehicle.primary_weapon.c_str());
    if (weapon_index < 0 || weapon_index > 0xFF) return false;
    const uint8_t adm = static_cast<uint8_t>(weapon_index);
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(adm);
    if (weapon == nullptr) return false;
    if (vehicle.primary_weapon_slot_adm != adm) {
        vehicle.primary_weapon_slot = WeaponSlotState{};
        vehicle.primary_weapon_slot_adm = adm;
        // Scope zero seed and elevation under flags & 3 [orig:
        //  WeaponSlot_InitFromEntityDef @0x5466C0 -> WeaponSlot_InitFromDef
        //  @0x53EE70 (call @0x54670A): +0x60 = def+0xA0 / def+0x9C
        //  @0x53EEB2..0x53EECB; +0x04 = def[+0x3B0 + 4*step] under def+8 & 3
        //  @0x53EEDA..0x53EEEC; +0x08 = atan2(def+0x8C, step * def+0x9C << 16
        //  floored at 100 m) outside that gate, never negated here
        //  @0x53EF4F..0x53EF8B]; the clip/reserve words from def+0x58/+0x5C
        //  @0x54670F..0x546733.
        vehicle.primary_weapon_slot.scope_zero = weapon_scope_zero_initial(weapon->action_fsm.scope_zero);
        if ((weapon->flags & 3) != 0)
            vehicle.primary_weapon_slot.zero_pitch = weapon_scope_zero_pitch(
                weapon->action_fsm.scope_zero, vehicle.primary_weapon_slot.scope_zero);
        vehicle.primary_weapon_slot.zero_yaw = weapon_scope_zero_yaw(
            weapon->action_fsm.scope_zero, vehicle.primary_weapon_slot.scope_zero);
        // The zoom seed (MountSlot+0xC). The emplacement's own slot has no
        // owner entity, so the class-6 sniper lock never applies here: a
        // tank cannon starts at its floor, a JOTAC roof sight at 1x
        // [orig: WeaponSlot_InitFromEntityDef passes entityPtr 0 @0x546706;
        //  WeaponSlot_InitFromDef skips the lock @0x53EEF7, seed
        //  @0x53EF2D..0x53EF44].
        vehicle.primary_weapon_slot.scope_zoom = weapon_slot_initial_zoom(weapon->scope_max_mag,
                weapon->scope_initial_mag, weapon->scope_min_mag, /*sniper_lock=*/false);
        if (weapon->clipsize < 0) {
            vehicle.primary_weapon_slot.clip = -1;
        } else {
            vehicle.primary_weapon_slot.clip = weapon->clipsize;
            vehicle.primary_weapon_slot.reserve = std::max<int32_t>(
                    0, static_cast<int32_t>(weapon->startrounds) - weapon->clipsize);
        }
    }
    return true;
}

bool VehicleSystem::bind_use_gun_slot(Entity &occupant, Entity &vehicle) {
    World &world = world_;
    if (!occupant.use_gun_slot_swapped) {
        occupant.pre_use_gun_equipped_adm_index = occupant.equipped_adm_index;
        occupant.use_gun_slot_swapped = true;
    }
    vehicle.primary_weapon_owner = occupant.handle;
    if (!world.vehicles.prepare_weapon_slot(vehicle)) {
        occupant.equipped_adm_index = 0xFF;
        return false;
    }
    occupant.equipped_adm_index = vehicle.primary_weapon_slot_adm;
    return true;
}

const WeaponSlotState *VehicleSystem::resolve_mounted_ammo_slot(const Entity &mount) const {
    const World &world = world_;
    // Retail proves the item definition and the EWeap attrib before resolving
    // ANY slot: the shared helper bails to NULL and the phase-8 writer emits
    // the zero-word form when either is missing [orig: shared helper @0x5460E0
    // (!itemDef -> 0; !(attrib & 0x20) -> 0); writer gate @0x4FFE0B]. A tool
    // world that installs authored seat specs before the item database
    // therefore resolves no slot until traits arrive.
    if (!mount.has_item_def ||
        (mount.item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    // The unredirected route is the entity's already-bound embedded MountSlot.
    // Only following the mutable route bit to another entity needs the
    // cross-entity relationship proof below.
    if (!mount.primary_weapon_slot.redirect_to_parent_slot)
        return &mount.primary_weapon_slot;
    // Stand-in note: the shared helper routes vehicles via the def+84 attrib
    // bit 0x40 [orig: Entity_GetWeaponSlots @ 0x5460E0] while the phase-8
    // writer keys def+92 type==1 [orig: @ 0x4FFE3F]; the shipped corpus stamps
    // both together on every EWeap vehicle, so type==1 serves both sites.
    if (mount.item_type == 1u)
        return &mount.primary_weapon_slot;

    // Retail follows entity+0x28 (groundEntity), not the addeweap metadata
    // pointer. Promotion/materialization capture the same relationship's live
    // generation so packed-handle reuse cannot redirect into an unrelated row.
    if (!mount.ground_target.valid() ||
        mount.emplacement_parent != mount.ground_target ||
        mount.emplacement_parent_spawn_id == 0)
        return nullptr;
    const Entity *parent = world.registry.get(mount.ground_target);
    if (parent == nullptr ||
        parent->registry_spawn_id != mount.emplacement_parent_spawn_id ||
        !parent->has_item_def || parent->item_type != 1u ||
        (parent->item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    return &parent->primary_weapon_slot;
}

WeaponSlotState *VehicleSystem::resolve_mounted_ammo_slot(Entity &mount) {
    World &world = world_;
    (void)world;
    return const_cast<WeaponSlotState *>(
            static_cast<const VehicleSystem *>(this)->resolve_mounted_ammo_slot(
                    static_cast<const Entity &>(mount)));
}

void vehicle_release_equipped_slot(Entity &occupant, Entity *vehicle) {
    if (vehicle != nullptr && vehicle->primary_weapon_owner == occupant.handle)
        vehicle->primary_weapon_owner = EntityHandle{};
    const bool is_player =
            ((occupant.flags | occupant.engine_flags) & 0x100u) != 0;
    if (occupant.use_gun_slot_swapped) {
        if (is_player)
            occupant.equipped_adm_index = occupant.pre_use_gun_equipped_adm_index;
        occupant.pre_use_gun_equipped_adm_index = 0xFF;
        occupant.use_gun_slot_swapped = false;
    }
    // Then any non-player leaving its parent, whatever the seat, drops its
    // EquippedSlot and zeroes its AdmDef byte.
    // [orig: Entity_DetachFromVehicle Flags 0x100 test @0x43568D, EquippedSlot
    //  @0x435696, the byte @0x43569C]
    if (!is_player) occupant.equipped_adm_index = 0;
}

Vec3 entity_local_point_world(const Entity &vehicle, const Vec3 &local) {
    // Seat points use the carrier's live BAM frame, as collision/render do;
    // the mission-degree mirrors discard sub-degree flight motion. An unmoved
    // carrier's frame is its placement angles in the spawn form.
    // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50, point @0x4b0d42;
    //  Math_BuildFixedPointMatrixFromEulerAngles @0x613f40; Entity_SpawnFromBMSRecord
    //  @0x40EB42..0x40EBA6]
    const Vec3 &L = local;
    const int32_t heading = vehicle.veh.yaw_seeded ? vehicle.veh.yaw_bam
            : spawn_angle_bam(90 - vehicle.yaw);
    const int32_t origin[3] = {0, 0, 0};
    const CollisionMatrix m = collision_matrix_from_euler(
            heading,
            vehicle.veh.yaw_seeded ? vehicle.veh.air_pitch_bam
                    : spawn_angle_bam(vehicle.pitch),
            vehicle.veh.yaw_seeded ? vehicle.veh.air_roll_bam
                    : spawn_angle_bam(vehicle.roll), origin);
    // seat_local is pre-swizzled ((-y, x, z) over the raw authored ints — a
    // baked-in Rz(90)), while the collision euler matrix with heading
    // bam(90 - yaw) expects RAW model coordinates: un-swizzle first.
    const int32_t lf[3] = {static_cast<int32_t>(L.y * 65536.0f),
                           static_cast<int32_t>(-L.x * 65536.0f),
                           static_cast<int32_t>(L.z * 65536.0f)};
    int32_t wf[3];
    m.rotate_point(lf, wf);
    Vec3 p;
    p.x = vehicle.position.x + static_cast<float>(wf[0]) / 65536.0f;
    p.y = vehicle.position.y + static_cast<float>(wf[1]) / 65536.0f;
    p.z = vehicle.position.z + static_cast<float>(wf[2]) / 65536.0f;
    return p;
}

void VehicleSystem::presnap_attach_heading(Entity &occupant, const Entity &vehicle, const Seat &seat) {
    World &world = world_;
    // A UseGun requester faces along the gun: the carrier's own Yaw less its
    // stored gun yaw word (+0x322) shifted up. Every other seat takes the seat
    // bone's yaw, the carrier frame turned by the seat offset as
    // pose_mounted_occupant composes it.
    // [orig: Entity_RequestVehicleAttach @0x43655F..0x43656E (UseGun),
    //  @0x4365BF..0x4365C3 (the Entity_GetBoneTransformAndOrientation yaw)]
    const int32_t carrier = emplaced_gun_frame_heading(vehicle);
    const int32_t seat_heading = seat.type == SeatType::Gunner && !seat.attachment_frame
            ? io::bam_sub(carrier, emplaced_word_bam(vehicle.emplaced_gun_yaw_word))
            : io::bam_sub(carrier, bam_from_degrees_wrapped(seat.yaw_offset));
    occupant.yaw = static_cast<int16_t>(
            std::lround(mission_yaw_deg_from_bam_heading(seat_heading)));
    AiEntity *body = world.ai.for_handle(occupant.handle);
    if (body == nullptr) return;

    body->heading = seat_heading;
    // Retail has one entity Yaw. OpenNova separates the local input-owned look
    // target from the render heading, so both must receive the same attach snap.
    if (body->inf.is_local_player)
        body->inf.target_heading = seat_heading;
}

MountedPose VehicleSystem::pose_mounted_occupant(Entity &occ, const Entity &vehicle, const Seat &seat) {
    World &world = world_;
    MountedPose live;
    if (world.pose_provider == nullptr ||
        !world.pose_provider->resolve_mounted_pose(world, vehicle, seat, live)) {
        // The root/local fallback uses the same complete carrier frame as
        // collision. Only an unseeded mission row needs degree-to-BAM conversion.
        // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 over @0x613f40]
        live.position = entity_local_point_world(vehicle, seat.seat_local);
        const int offset = !seat.attachment_frame && seat.type == SeatType::Gunner
                ? -seat.yaw_offset : seat.yaw_offset;
        live.heading = vehicle.veh.yaw_seeded
                ? io::bam_sub(vehicle.veh.yaw_bam, bam_from_degrees_wrapped(offset))
                : io::bam_sub(spawn_angle_bam(90 - vehicle.yaw), bam_from_degrees_wrapped(offset));
        live.pitch = vehicle.veh.yaw_seeded ? vehicle.veh.air_pitch_bam
                : spawn_angle_bam(vehicle.pitch);
        live.roll = vehicle.veh.yaw_seeded ? vehicle.veh.air_roll_bam
                : spawn_angle_bam(vehicle.roll);
    }
    occ.position = live.position;
    occ.yaw = static_cast<int16_t>(std::lround(mission_yaw_deg_from_bam_heading(live.heading)));
    occ.pitch = static_cast<int16_t>(std::lround(live.pitch * kDegreesPerBam));
    occ.roll = static_cast<int16_t>(std::lround(live.roll * kDegreesPerBam));
    if (seat.attachment_frame) {
        // An addeweap child carries the full bone attitude into its own model,
        // collision, and camera. Ordinary infantry retain independent LOOK.
        // [orig: Entity_UpdateTransformAndTurret @0x440ca0 ->
        //  build_bone_attachment_matrix @0x56c630]
        occ.veh.yaw_seeded = true;
        occ.veh.yaw_bam = live.heading;
        occ.veh.air_pitch_bam = live.pitch;
        occ.veh.air_roll_bam = live.roll;
    }
    return live;
}

} // namespace opennova::world
