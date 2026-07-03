#include "world/vehicle_attach.h"

#include "world/world.h"

namespace opennova::world {

namespace {

// A live ENEMY occupies `vehicle` (or one of its carried guns — gun-carrier traversal is
// unmodeled; tracked D-NET-157). Scans pool 0, skipping dead / self / same-team occupants,
// so same-team co-boarding never blocks. [orig: Vehicle_HasEnemyOccupant @0x4359F0 —
// pool-0 scan, dead skip, +0x162 team compare @0x435a5f, parentEntity(0x16C) == root hit]
bool vehicle_has_enemy_occupant(const World &world, const Entity &vehicle,
                                const Entity &requester) {
    bool hit = false;
    world.registry.for_each([&](const Entity &e) {
        if (hit) return;
        if (e.handle.pool() != 0) return;
        if (e.handle == requester.handle) return;      // self [orig: skip requester]
        if (e.health <= 0 || !e.alive) return;         // dead [orig: Flags & 2 skip]
        if (e.team == requester.team) return;          // same team never blocks
        if (e.mounted && e.mount_target == vehicle.handle) hit = true;
    });
    return hit;
}

// Another occupant already holds this wire bone on `vehicle` (the occupancy check for a
// bone with no seat-table match). [orig: the mountHandles[idx] != 0xFFFF reject @0x435ba9,
// keyed by the ItemDef seat-block position of the bone]
bool wire_bone_taken(const World &world, const Entity &vehicle, const Entity &requester,
                     uint8_t bone) {
    bool taken = false;
    world.registry.for_each([&](const Entity &e) {
        if (taken) return;
        if (e.handle.pool() != 0 || e.handle == requester.handle) return;
        if (e.mounted && e.mount_target == vehicle.handle && e.mount_bone == bone) taken = true;
    });
    return taken;
}

} // namespace

bool entity_process_vehicle_attach(World &world, EntityHandle player, EntityHandle vehicle,
                                   uint8_t bone) {
    Entity *occ = world.registry.get(player);
    Entity *veh = world.registry.get(vehicle);
    // 1. Resolve + dead gates [orig: @0x435b01 — null vehicle/itemDef/player or either
    //    Flags & 2 reject]. Our authoritative dead store is health/alive; the flags bit-1
    //    movement/spawn gate also rejects (a mid-spawn player cannot mount).
    if (occ == nullptr || veh == nullptr) return false;
    if (occ->health <= 0 || !occ->alive || (occ->flags & 2u) != 0) return false;
    if (veh->health <= 0 || !veh->alive || (veh->flags & 2u) != 0) return false;

    // 2. Seat classification by the wire bone. Exact bone_index match wins; an unmatched
    //    bone (userpoint-index vs full-bone-table divergence, D-NET-157) falls back to the
    //    best free seat, keeping the wire bone as the occupancy/echo key. A vehicle with no
    //    seat table at all (no model specs fed) accepts on wire-bone occupancy alone.
    //    [orig: Entity_GetBoneSlotType @0x434ED0 -> slotType 0 rejects; seat-block index
    //    @0x435ba9]
    int seat_idx = -1;
    for (int i = 0; i < static_cast<int>(veh->seats.size()); ++i) {
        if (veh->seats[i].type == SeatType::None) continue;
        if (veh->seats[i].bone_index == bone) {
            seat_idx = i;
            break;
        }
    }
    if (seat_idx < 0 && !veh->seats.empty())
        seat_idx = world.commands.find_best_seat(*veh, player);

    // 3. Enemy-occupant gate [orig: @0x4359F0 via the reject @0x435b4b-ish].
    if (vehicle_has_enemy_occupant(world, *veh, *occ)) return false;

    // 4. Occupancy: the matched seat must be free (or already ours); an unmatched wire
    //    bone must not be held by another occupant [orig: @0x435ba9].
    if (seat_idx >= 0) {
        const Seat &s = veh->seats[seat_idx];
        if (s.occupant.valid() && s.occupant != player) return false;
    } else if (wire_bone_taken(world, *veh, *occ, bone)) {
        return false;
    }

    // 5. Already mounted -> detach first [orig: @0x435bce].
    if (occ->mounted) entity_detach_from_vehicle(world, player);

    // 6. Writes [orig: Entity_AttachToVehicleSlot @0x4946D0 common tail @0x494752-75].
    if (seat_idx >= 0) {
        veh->seats[seat_idx].occupant = player; // [orig: mountHandles[idx] = handle @0x494746]
        occ->mount_type = veh->seats[seat_idx].type;
    } else {
        occ->mount_type = SeatType::Passenger; // wire-bone occupancy only (D-NET-157)
    }
    occ->flags = (occ->flags & 0xFFFF5FBFu) | 0x40u; // clear 0x8000|0x2000, set mounted
    occ->mount_target = vehicle;                     // [orig: parentEntity(0x16C) = vehicle]
    occ->mount_bone = bone;                          // [orig: attachBoneId(0x157) = bone]
    occ->mount_seat = static_cast<int8_t>(seat_idx); // [orig: parentSlot(0x168) = slotType]
    occ->mounted = true;
    // Success clears the movement stance bits [orig: MoveOrder &= ~0x300 @0x435c42 + the
    // prone/crouch latch clears @0x435c54/@0x435c59].
    occ->net_stance_bits = 0;
    return true;
}

bool entity_detach_from_vehicle(World &world, EntityHandle player) {
    Entity *occ = world.registry.get(player);
    if (occ == nullptr || !occ->mounted) return false;
    // [orig: Entity_DetachFromVehicle @0x4355F0] MoveOrder &= ~0x300 (stance clear), then
    // every matching seat handle on the mount target releases (all 10 slots in the
    // original; our seat vector sweeps by occupant), Flags &= ~0x40 and the mount trio
    // clears. The EquippedSlot restore (+0x308) and the ATTR_PlayerControl engine-state 7
    // are unmodeled (no weapon-slot / engine-state model) — tracked in D-NET-157.
    occ->net_stance_bits = 0;
    if (Entity *veh = world.registry.get(occ->mount_target)) {
        for (Seat &s : veh->seats) {
            if (s.occupant == player) s.occupant = EntityHandle{}; // [orig: -> 0xFFFF]
        }
    }
    occ->flags &= ~0x40u;          // [orig: Flags &= ~0x40]
    occ->mount_target = EntityHandle{}; // [orig: +0x16C = 0]
    occ->mount_bone = 0;           // [orig: +0x157 = 0]
    occ->mount_seat = -1;          // [orig: +0x168 = 0]
    occ->mount_type = SeatType::None;
    occ->mounted = false;
    return true;
}

} // namespace opennova::world
