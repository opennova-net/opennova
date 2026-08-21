#pragma once

#include <cstdint>

#include "world/entity.h"
#include "world/geom.h"

// The vehicle mount/claim surface: MountedPose + its provider seam, the
// control-occupant claim/release family, attach-heading presnap, occupant
// posing, and SeatSelectionMode. Split from the world.h umbrella (W3-7).

namespace opennova::world {

class World;

// Host-resolved live seat-bone pose. The portable world owns attachment policy and
// fallback geometry; a model-aware host may supply the current articulated bone frame.
// [orig: UseGun Entity_AttachToBoneAndUpdateTransform @0x5463d0; ordinary
// seats Entity_GetBoneTransformAndOrientation @0x4b0c50]
struct MountedPose {
    Vec3 position;
    int16_t yaw = 0;
    int16_t pitch = 0;
    int16_t roll = 0;
};

struct IMountedPoseProvider {
    virtual ~IMountedPoseProvider() = default;
    virtual bool resolve_mounted_pose(World &world, const Entity &carrier,
                                      const Seat &seat, MountedPose &out) = 0;
};

// Seat-type pose/channel predicates (moved beside the mount surface, S7a):
// control seats suppress the on-foot upper-body weapon channel; passengers
// keep it. [orig: the parentSlot {2,3,5} gates @ 0x4b14a7 / @ 0x4dcc44]
inline bool seat_type_blocks_weapon_channel(SeatType type) {
    switch (type) {
        case SeatType::Controller:
        case SeatType::Gunner:
        case SeatType::Driver:
            return true;
        default:
            return false; // passenger seats retain the on-foot upper-body channel
    }
}

// A pilot cannot fire. Retail's local fire gate rejects parentSlot 2
// (Controller) and 5 (Driver) outright -- a GUNNER (3) still fires, which is why
// this is a strictly narrower set than seat_type_blocks_weapon_channel's {2,3,5}
// viewmodel test. Nothing about it is attrib-driven: the `PilotOnly` token in
// items.def is a BHD-lineage leftover retail's parser does not even know
// [orig: no 'PilotOnly' entry in the attrib chain, string pool 0x7c8390..].
// [orig: Player_CanFireWeapon @0x5cf780 -- `if (parentEntity) { seat =
//  parentSlot; if (seat == 2 || seat == 5) return 0; }`]
inline bool mount_blocks_firing(const Entity &occupant) {
    if (!occupant.mounted) return false;
    return occupant.mount_type == SeatType::Controller ||
           occupant.mount_type == SeatType::Driver;
}

inline bool mount_blocks_weapon_channel(const Entity &entity) {
    return entity.mounted && seat_type_blocks_weapon_channel(entity.mount_type);
}

// Does the seat the local player occupies SUPPRESS the first-person weapon?
//
// Retail draws the viewmodel only when the player is NOT in a vehicle, or the
// parent slot is outside {2, 3, 5}, or the carrier is an emplaced weapon that is
// not player-controlled. A helicopter pilot therefore has no weapon in hand at
// all -- the retail Black Hawk cockpit shows a clear screen, while ours drew a
// scoped rifle over the instrument panel.
//
// Our SeatType values ARE retail's slot numbers (Passenger 1, Controller 2,
// Gunner 3, Driver 5), so the {2,3,5} test is exactly the existing
// seat_type_blocks_weapon_channel gate; a Passenger keeps the weapon and can
// still shoot out.
// [orig: Player_RenderFirstPersonViewModel @0x4bd2a0 (kong 179894) —
//  `!vehicle || parentSlot not in {2,3,5} || (attrib & 0x20 && !(attrib & 0x40))`
//  guards the whole draw; attrib 0x20 = EWEAP, 0x40 = PLAYERCONTROL]
inline bool mount_hides_fp_viewmodel(const Entity &occupant,
                                     const Entity *carrier) {
    if (!occupant.mounted || carrier == nullptr) return false;
    if (!seat_type_blocks_weapon_channel(occupant.mount_type)) return false;
    const bool eweap = (carrier->item_attrib & kItemAttribEweap) != 0u;
    const bool player_control =
            (carrier->item_attrib & kItemAttribPlayerControl) != 0u;
    if (eweap && !player_control) return false; // a static gun keeps its weapon
    return true;
}

inline bool mount_collapses_right_hand_row(const Entity &entity) {
    // This terminal skeletal row is stricter than the secondary-channel gate:
    // retail requires a controller/gunner/driver parent slot AND no Flags 0x100.
    // In the port, engine_flags is the authoritative entity+0x24 Flags mirror.
    // [orig: Entity_BuildBoneTransformMatrices special row @ 0x4b1290]
    return entity.mounted &&
            seat_type_blocks_weapon_channel(entity.mount_type) &&
            (entity.engine_flags & kEntityFlagPlayer) == 0;
}

// Host-facing lifecycle for effects that exist only while a vehicle has its single
// tracked primary occupant (the +368 claimant). Payload fields are the target vehicle's
// net_id, bms_id, spawn_origin, and packed runtime wire handle.
// [orig: occupied spawn in entity_update_damage_accumulator_and_shadow @0x48fa70 gate
// @0x48faad (attrib&0x40 && occupantEntity(+368)); release in Entity_DetachFromVehicle
// @0x4355f0 stop leg @0x4356e9..0x435759 — runs ONLY when the detacher IS the claimant.]
void emit_vehicle_control_started(World &world, const Entity &vehicle);
void emit_vehicle_control_stopped(World &world, const Entity &vehicle);
void emit_vehicle_control_stopped(World &world, uint16_t target_net_id,
                                   int32_t target_bms_id, uint32_t target_spawn_origin,
                                   uint16_t target_wire_handle);
// The +368 primary-occupant claim: Controller/Driver seats claim when the slot is empty
// or already theirs; a Gunner claims only when empty (the emplaced-gun UseGun leg);
// Passengers never claim. Emits vehicle_control_started on the empty -> claimed edge.
// Returns true when the occupant holds the claim after the call.
// [orig: Entity_AttachToVehicleSlot @0x4946d0 — +368 writes @0x4947d2 (ctrlx,
// empty-or-same), @0x4948d8 (drvrx, empty-or-same), @0x49495e (UseGun, empty only)]
bool vehicle_claim_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant,
                                    SeatType seat);
// Clears the claim and emits vehicle_control_stopped iff `occupant` IS the claimant —
// a second control-seat occupant staying aboard does NOT keep the engine running.
// [orig: Entity_DetachFromVehicle @0x4355f0 — `occupantEntity == entity` gate @0x4356e9,
// emitter release + engine-stop sound @0x435716..0x435759, +368 clear @0x43577c]
bool vehicle_release_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant);
// Swap a UseGun occupant to the parent's embedded weapon slot, preserving the
// personal equipped AdmDef for detach. Returns true once the parent weapon resolves.
// [orig: Entity_AttachToUseGunSlot @0x546b80..0x546c73]
bool vehicle_bind_use_gun_slot(World &world, Entity &occupant, Entity &vehicle);
// Resolve an entity's authored primary_weapon into its embedded MountSlot and
// seed clip/reserve when the weapon table becomes available. Entity promotion
// can precede armory loading, so callers use this at the first live slot edge.
bool vehicle_prepare_weapon_slot(World &world, Entity &vehicle);
// Resolve the EWeap MountSlot selected by retail's live route bit. A type-1
// vehicle uses its own embedded slot. An attached non-vehicle EWeap uses its
// own slot until redirect_to_parent_slot is set, then follows the exact
// groundEntity relationship to a live type-1 EWeap carrier. Invalid/stale
// routes return null rather than falling back to attachment metadata.
// [orig: shared helper @0x5460e0; NetPacket_WritePlayerState @0x4ffe18]
WeaponSlotState *resolve_mounted_ammo_slot(World &world, Entity &mount);
const WeaponSlotState *resolve_mounted_ammo_slot(
        const World &world, const Entity &mount);
// Clear parent ownership; restore a player's personal EquippedSlot and clear an
// NPC's, matching the post-restore retail player-classifier branch.
// [orig: Entity_DetachFromVehicle restore @0x435671-0x435687,
//  NPC clear @0x435694-0x4356aa]
void vehicle_release_use_gun_slot(Entity &occupant, Entity *vehicle);
// True when at least one Controller/Driver seat has a live, internally consistent
// occupant link. Motor input only — NOT the effect-lifecycle predicate (that is the
// primary-occupant claim above).
bool vehicle_has_valid_control_occupant(const World &world, const Entity &vehicle);

// Entity_RequestVehicleAttach snaps the requester yaw to the chosen seat before
// authority applies the relationship. Keep the registry Entity and our split
// AiEntity/local-player look target coherent so the first mounted tick cannot
// restore the pre-attach look. Pitch is deliberately untouched.
// [orig: Entity_RequestVehicleAttach @0x4364a0; UseGun yaw @0x43656c]
void presnap_vehicle_attach_heading(World &world, Entity &occupant,
                                    const Entity &vehicle, const Seat &seat);

// Snap a mounted occupant onto its seat. A model-aware host resolves retail's live
// seat-bone frame; otherwise the portable fallback is vehicle.position +
// rotate(seat.seat_local, -vehicle.yaw), with Gunner yaw at vehicle.yaw-yaw_offset
// and other seats at vehicle.yaw+yaw_offset. Shared by every attach path and the AI
// tick's per-frame seat follow. [orig: UseGun @0x5463d0; ordinary seats @0x4b0c50]
void pose_mounted_occupant(World &world, Entity &occ, const Entity &vehicle,
                           const Seat &seat);

enum class SeatSelectionMode : uint8_t {
    Any = 0,
    PassengerOnly,     // command 123: only `sitex`
    RejectController,  // command 124: retail gate rejects `ctrlx`, keeps `drvrx` eligible
};

} // namespace opennova::world
