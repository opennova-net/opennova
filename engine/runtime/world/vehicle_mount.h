#pragma once

#include <cstdint>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>

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
// [orig: Player_RenderFirstPersonViewModel @0x4DED60, the gate @0x4DED80..0x4DEDA1 —
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

// Clear parent ownership; restore a player's personal EquippedSlot and clear an
// NPC's, matching the post-restore retail player-classifier branch.
// [orig: Entity_DetachFromVehicle restore @0x435671-0x435687,
//  NPC clear @0x435694-0x4356aa]
void vehicle_release_use_gun_slot(Entity &occupant, Entity *vehicle);



// A carrier-local point in world space through the carrier's FULL orientation
// frame — the same Rz(heading)*Ry(-pitch)*Rx(roll) matrix every collision query
// serves (collision_matrix_from_euler), so seats/exit goals and the collision
// shell agree on one frame. Retail has exactly one entity orientation matrix
// serving both. [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 over the
// entity matrix built by Math_BuildFixedPointMatrixFromEulerAngles @0x613f40]
Vec3 entity_local_point_world(const Entity &vehicle, const Vec3 &local);

enum class SeatSelectionMode : uint8_t {
    Any = 0,
    PassengerOnly,     // command 123: only `sitex`
    RejectController,  // command 124: retail gate rejects `ctrlx`, keeps `drvrx` eligible
};

} // namespace opennova::world
