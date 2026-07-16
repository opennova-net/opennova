// Wire-side vehicle attach/detach — the server acceptors behind the C2S 0x26/0x27
// vehicle messages (net-re §5.61 round 14). Handle-keyed and validation-ordered per the
// witnessed originals; the WAC/BMS mount commands (EntityCommands::mount, SSN-keyed,
// script semantics) stay separate.
//
// [orig: NapiNPServerMsg_HandleVehicleAttach @0x502390 -> Entity_ProcessVehicleAttach
//  @0x435AA0 -> Entity_AttachToVehicleSlot @0x4946D0 / Entity_AttachToUseGunSlot @0x546B80;
//  NapiNPServerMsg_HandleVehicleDetach @0x4FC980 -> Entity_DetachFromVehicle @0x4355F0.
//  There is NO reply message: the 0x0A compact player record's mounted branch is the
//  confirmation for everyone including the requester.]
#ifndef OPENNOVA_WORLD_VEHICLE_ATTACH_H
#define OPENNOVA_WORLD_VEHICLE_ATTACH_H

#include <cstdint>

#include "world/entity.h"

namespace opennova::world {

class World;

// Validate + apply one C2S 0x26 attach request: `player` mounts `vehicle` at model-bone
// `bone` (1-based, the wire byte — the occupancy/echo key). Returns true iff attached.
// Ported validation order [orig: Entity_ProcessVehicleAttach @0x435AA0]:
//   1. resolve both handles; reject a missing entity or either side dead
//      (Flags & 2 / health <= 0) [orig: @0x435b01];
//   2. seat classification: the vehicle seat whose bone_index matches the wire bone
//      [orig: Entity_GetBoneSlotType @0x434ED0 classifies the MODEL bone name —
//      sitex 1 / ctrlx 2 / drvrx 5 / UseGun 3]. Our seat table is built from the same
//      model's userpoints, but its bone_index is the USERPOINT enumeration index, which
//      can diverge from the retail full-bone-table index — an unmatched wire bone
//      therefore falls back to the best free seat with the wire bone kept as the
//      occupancy/echo key (tracked divergence, D-NET-157);
//   3. enemy-occupant gate: reject when a LIVE ENEMY already occupies the vehicle
//      [orig: Vehicle_HasEnemyOccupant @0x4359F0 — scans pool 0, skips dead/self/
//      same-team; same-team occupants never block multi-seat co-boarding];
//      (the weapon-busy gate on EquippedSlot->currentAction [orig: @0x435b29] is
//      unmodeled — no weapon action state server-side; tracked in D-NET-157)
//   4. seat occupancy: an occupied matching seat / an already-taken wire bone rejects
//      [orig: @0x435ba9 mountHandles[idx] != 0xFFFF];
//   5. an already-mounted requester detaches first [orig: @0x435bce];
//   6. writes [orig: Entity_AttachToVehicleSlot @0x4946D0]: seat occupant = player,
//      player Flags = (Flags & 0xFFFF5FBF) | 0x40 (clear 0x8000|0x2000, set mounted),
//      mount_target/mount_bone/mount_type/mount_seat set, stance bits cleared
//      [orig: MoveOrder &= ~0x300 @0x435c42 + the input-latch clears @0x435c54].
bool entity_process_vehicle_attach(World &world, EntityHandle player, EntityHandle vehicle,
                                   uint8_t bone);

// Apply one C2S 0x27 detach: release every seat this occupant holds on its mount target,
// clear the mount fields + the 0x40 mounted flag + the stance bits, and run the
// +368 primary-occupant release (the engine-stop edge fires only for the claimant).
// [orig: Entity_DetachFromVehicle @0x4355F0 — MoveOrder &= ~0x300, EquippedSlot restore
//  (unmodeled), the +368/+0x170 claimant leg @0x4356e9..0x43577c,
//  all matching mountHandles -> 0xFFFF, Flags &= ~0x40, +0x16C/+0x157/+0x168 cleared.]
// Returns true iff the entity was mounted.
bool entity_detach_from_vehicle(World &world, EntityHandle player);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_ATTACH_H
