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
