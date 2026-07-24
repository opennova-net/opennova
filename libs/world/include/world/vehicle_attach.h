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
//      [orig: Entity_GetBoneSlotType @0x434ED0 classifies the MODEL USRP row name —
//      48-byte rows, name +32, 1-based index; sitex 1 / ctrlx 2 / drvrx 5 / UseGun 3].
//      Production seat specs preserve that exact enumeration; unknown rows reject;
//   3. enemy-occupant gate: reject when a LIVE ENEMY already occupies the vehicle
//      [orig: Vehicle_HasEnemyOccupant @0x4359F0 — scans pool 0, skips dead/self/
//      same-team; same-team occupants never block multi-seat co-boarding];
//      (the weapon-busy gate on EquippedSlot->currentAction [orig: @0x435b29] is
//      unmodeled — no weapon action state server-side; tracked in D-NET-157)
//   4. seat occupancy: an occupied matching seat / an already-taken wire bone rejects
//      [orig: @0x435ba9 mountHandles[idx] != 0xFFFF];
//   5. an already-mounted requester detaches first [orig: @0x435bce];
//   6. writes: seat occupant + mount_target/mount_bone/mount_type/mount_seat, then
//      stance bits clear [orig: MoveOrder &= ~0x300 @0x435c42 + input latches
//      @0x435c54]. Generic vehicle slots clear 0xA000 and set Flags 0x40
//      [orig: Entity_AttachToVehicleSlot @0x494752]; UseGun clears 0xA000 without
//      setting 0x40 and binds the parent weapon slot
//      [orig: Entity_AttachToUseGunSlot @0x546c42-0x546c7c].
bool entity_process_vehicle_attach(World &world, EntityHandle player, EntityHandle vehicle,
                                   uint8_t bone);

// Apply one C2S 0x27 detach: release every seat this occupant holds on its mount target,
// clear the mount fields + any generic-slot 0x40 flag + the stance bits, and run the
// +368 primary-occupant release (the engine-stop edge fires only for the claimant).
// [orig: Entity_DetachFromVehicle @0x4355F0 — MoveOrder &= ~0x300, EquippedSlot
//  restore for parentSlot 2/3 then non-player clear @0x435671..0x4356aa (ported),
//  the +368/+0x170 claimant leg @0x4356e9..0x43577c,
//  all matching mountHandles -> 0xFFFF, Flags &= ~0x40, +0x16C/+0x157/+0x168 cleared.]
// Returns true iff the entity was mounted.
bool entity_detach_from_vehicle(World &world, EntityHandle player);

// One free seat (or armory point) found by the use-key/label proximity scan.
struct NearestSeatHit {
    EntityHandle vehicle;
    int seat_index = -1; // seat index, or the armory_points index in armory mode
    SeatType type = SeatType::None;
};

// The use-key nearest-seat scan [orig: Entity_FindNearestSeatOrArmory @0x435d50]: for
// every live seat-bearing entity, test each FREE seat's world position against the player
// eye: horizontal distance <= 4.0 u (0x40000 16.16) and 3D distance <= 16384 u unmounted /
// 910.2 u while seat-swapping (0x3FFFFFC0 / 0x38E38E0), LOS-gated, score =
// horiz + dist3d/512, lowest wins. Enemy-occupied vehicles are skipped
// [orig: Vehicle_HasEnemyOccupant reject @0x435e58]. armory_mode = the original's
// searchMode != 0 [orig: @0x435f12]: instead of seats, "armory*" points of Armory-attrib
// items are scanned with the same math (no occupancy), reporting SeatType::ArmoryPoint
// [orig: the attrib 0x80000 walk @0x4361ee, seatType 4 @0x436417] — the label highlight
// pick while the player stands in the armory volume; the mount toggle always scans seats.
// Tracked deviations (D-AI-11): the candidate set is a registry sweep (the original walks
// the player's proximity list), the eye is the +0.9 u chest stand-in + the witnessed
// +0.1875 u bias (CameraOffset unmodeled), the seated requester skips its CURRENT mount
// vehicle outright (the own-hull LOS occlusion stand-in until pool-1 collision lands —
// USE exits, never cycles seats; j), and the emplaced-gun carrier LOS/reject legs
// (def attrib 0x20 -> groundEntity) are unmodeled.
bool find_nearest_free_seat(World &world, const Entity &player, NearestSeatHit &out,
                            bool armory_mode);

// One floating attach label [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — the
// selection half; projection and drawing stay host-side]. world_pos carries the witnessed
// +0.1875 u label lift [orig: point.z = boneZ + 12288 @0x5a3585].
struct AttachLabel {
    EntityHandle entity;
    int seat_index = -1;              // seat index; armory-point index when armory
    SeatType type = SeatType::None;   // Passenger/Controller/Driver -> Sit/Control text,
                                      // Gunner -> the weapon attachtextid, ArmoryPoint -> Armory
    bool armory = false;
    bool nearest = false;             // the full-bright highlight; others draw half-bright
    Vec3 world_pos;
};

// Optional deterministic work counters for attach-label performance tests and probes.
// The label gather is called every render frame; one gather may visit many proximity
// candidates, but it must not rescan the whole entity registry for every candidate.
struct AttachLabelScanStats {
    uint32_t enemy_occupancy_registry_passes = 0;
};

// The floating seat/armory label list for the local player, a structural translation of
// the selection half of [orig: draw_vehicle_seat_and_armory_labels @0x5a3290]:
//  - no nearest scan hit -> no labels at all [orig: the Entity_FindNearestSeatOrArmory
//    gate @0x5a32e2];
//  - can_fire limits labels to the nearest entity; when the player cannot fire, every
//    in-range candidate labels [orig: !Player_CanFireWeapon() || entity == nearest
//    @0x5a3354];
//  - per entity: dead/destroyed skip, enemy-occupant reject [orig: @0x5a3373/@0x5a3395];
//  - armory_mode false: every FREE seat within 4.0 u 3D of the player position
//    (point lifted +0.1875 u) with clear LOS labels [orig: the seat loop @0x5a3464,
//    occupancy skip @0x5a348f, distance @0x5a35f0, raycast @0x5a3609];
//  - armory_mode true: the "armory*" points of Armory-attrib items label instead
//    [orig: @0x5a36f5..@0x5a38e2].
// Labels append to out in scan order; nearest marks the scan winner's own label.
void collect_attach_labels(World &world, const Entity &player, bool armory_mode,
                           bool can_fire, std::vector<AttachLabel> &out,
                           AttachLabelScanStats *stats = nullptr);

// The use-key mount toggle [orig: Entity_ToggleVehicleMount @0x436950 +
// Entity_TryEnterNearestVehicle @0x4368c0]:
//  - unmounted, standing ON a seat-bearing carrier (our generic ground_target
//    stands in for the Flags 0x200 deck latch; this is unrelated to CL) -> best free seat
//    on the carrier
//    [orig: Entity_FindBestSeatSlot @0x4351f0];
//  - unmounted otherwise -> the nearest-seat scan;
//  - mounted -> a seat in scan reach swaps [orig: @0x4369ac], else detach.
// The weapon-busy gate (EquippedSlot currentAction @0x436958) and the WAC no-dismount
// global (dword_C6EADC @0x43698b) are the caller's/session's concern (D-AI-11).
// Returns true iff a mount/swap/detach was applied.
bool player_toggle_vehicle_mount(World &world, EntityHandle player);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VEHICLE_ATTACH_H
