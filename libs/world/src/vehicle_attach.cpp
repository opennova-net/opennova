#include "world/vehicle_attach.h"

#include <cmath>
#include <cstdint>

#include "world/ai.h"
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

// The shared attach write block [orig: Entity_AttachToVehicleSlot @0x4946D0 common tail
// @0x494752-75]. seat_idx < 0 = wire-bone occupancy only (D-NET-157).
void attach_apply(World &world, Entity &occ, Entity &veh, int seat_idx, uint8_t bone) {
    if (seat_idx >= 0) {
        veh.seats[seat_idx].occupant = occ.handle; // [orig: mountHandles[idx] = handle @0x494746]
        occ.mount_type = veh.seats[seat_idx].type;
    } else {
        occ.mount_type = SeatType::Passenger; // wire-bone occupancy only (D-NET-157)
    }
    occ.flags = (occ.flags & 0xFFFF5FBFu) | 0x40u; // clear 0x8000|0x2000, set mounted
    occ.mount_target = veh.handle;                 // [orig: parentEntity(0x16C) = vehicle]
    occ.mount_target_net_id = veh.net_id;
    occ.mount_target_bms_id = veh.bms_id;
    occ.mount_target_spawn_origin = veh.spawn_origin;
    occ.mount_bone = bone;                          // [orig: attachBoneId(0x157) = bone]
    occ.mount_seat = static_cast<int8_t>(seat_idx); // [orig: parentSlot(0x168) = slotType]
    occ.mounted = true;
    // Success clears the movement stance bits [orig: MoveOrder &= ~0x300 @0x435c42 + the
    // prone/crouch latch clears @0x435c54/@0x435c59].
    occ.net_stance_bits = 0;
    vehicle_claim_primary_occupant(world, veh, occ.handle, occ.mount_type); // [orig: +368 @0x4946d0]
}

// Seat world position: the same seat-local rotate the per-tick pose applies
// (pose_mounted_occupant), our stand-in for the posed seat-bone transform
// [orig: build_bone_attachment_matrix @0x56c630 in the scan @0x435fe7].
Vec3 seat_world_pos(const Entity &veh, const Seat &s) {
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double a = static_cast<double>(-veh.yaw) * kDeg2Rad;
    const double ca = std::cos(a), sa = std::sin(a);
    Vec3 p;
    p.x = veh.position.x + static_cast<float>(s.seat_local.x * ca - s.seat_local.y * sa);
    p.y = veh.position.y + static_cast<float>(s.seat_local.x * sa + s.seat_local.y * ca);
    p.z = veh.position.z + s.seat_local.z;
    return p;
}

// Precise-seat attach for the local toggle (the seat is already picked; the wire path keeps
// its bone resolution). Runs the same gate order as entity_process_vehicle_attach.
bool attach_to_seat_index(World &world, EntityHandle player, EntityHandle vehicle,
                          int seat_idx) {
    Entity *occ = world.registry.get(player);
    Entity *veh = world.registry.get(vehicle);
    if (occ == nullptr || veh == nullptr) return false;
    if (occ->health <= 0 || !occ->alive || (occ->flags & 2u) != 0) return false;
    if (veh->health <= 0 || !veh->alive || (veh->flags & 2u) != 0) return false;
    if (seat_idx < 0 || seat_idx >= static_cast<int>(veh->seats.size())) return false;
    if (vehicle_has_enemy_occupant(world, *veh, *occ)) return false;
    const Seat &s = veh->seats[seat_idx];
    if (s.type == SeatType::None) return false;
    if (s.occupant.valid() && s.occupant != player) return false;
    if (occ->mounted) entity_detach_from_vehicle(world, player); // [orig: @0x435bce]
    attach_apply(world, *occ, *veh, seat_idx, s.bone_index);
    return true;
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
    attach_apply(world, *occ, *veh, seat_idx, bone);
    return true;
}

bool entity_detach_from_vehicle(World &world, EntityHandle player) {
    Entity *occ = world.registry.get(player);
    if (occ == nullptr || !occ->mounted) return false;
    const bool claim_capable_seat = occ->mount_type != SeatType::Passenger &&
                                    occ->mount_type != SeatType::None;
    const uint16_t target_net_id = occ->mount_target_net_id;
    const int32_t target_bms_id = occ->mount_target_bms_id;
    const uint32_t target_spawn_origin = occ->mount_target_spawn_origin;
    Entity *veh = world.registry.get(occ->mount_target);
    // [orig: Entity_DetachFromVehicle @0x4355F0] MoveOrder &= ~0x300 (stance clear), then
    // every matching seat handle on the mount target releases (all 10 slots in the
    // original; our seat vector sweeps by occupant), Flags &= ~0x40 and the mount trio
    // clears. The EquippedSlot restore (+0x308) and the ATTR_PlayerControl engine-state 7
    // are unmodeled (no weapon-slot / engine-state model) — tracked in D-NET-157.
    occ->net_stance_bits = 0;
    if (veh != nullptr) {
        for (Seat &s : veh->seats) {
            if (s.occupant == player) s.occupant = EntityHandle{}; // [orig: -> 0xFFFF]
        }
    }
    occ->flags &= ~0x40u;          // [orig: Flags &= ~0x40]
    occ->mount_target = EntityHandle{}; // [orig: +0x16C = 0]
    occ->mount_target_net_id = 0;
    occ->mount_target_bms_id = 0;
    occ->mount_target_spawn_origin = 0;
    occ->mount_bone = 0;           // [orig: +0x157 = 0]
    occ->mount_seat = -1;          // [orig: +0x168 = 0]
    occ->mount_type = SeatType::None;
    occ->mounted = false;
    if (veh != nullptr) {
        // [orig: the +368 leg @0x4356e9..0x43577c — runs only for the claimant]
        vehicle_release_primary_occupant(world, *veh, player);
    } else if (claim_capable_seat) {
        // The vehicle is already gone; the stored identity carries the stop (host
        // cleanup — a spurious stop is idempotent downstream).
        emit_vehicle_control_stopped(world, target_net_id, target_bms_id,
                                     target_spawn_origin);
    }
    return true;
}

bool find_nearest_free_seat(World &world, const Entity &player, NearestSeatHit &out) {
    // Range caps, verbatim 16.16 [orig: @0x435d90 maxDistance = 0x3FFFFFC0, the mounted
    // override @0x435d9a = 0x38E38E0].
    const int32_t max_dist3d = player.mounted ? 59652320 : 1073741760;
    // The player reference point: position + the +0.9 u chest/eye stand-in (CameraOffset
    // unmodeled, D-AI-11) + the witnessed +0.1875 u scan bias [orig: the +12288 term
    // @0x436041].
    const double eye_x = static_cast<double>(player.position.x);
    const double eye_y = static_cast<double>(player.position.y);
    const double eye_z = static_cast<double>(player.position.z) + 0.9;

    int32_t best_score = 0x7FFFFFFF; // [orig: v60 init]
    bool found = false;

    // The original walks the player's proximity list [orig: entity+444/448 @0x435d60]; the
    // registry sweep is the container rebase — behavior-equal inside the 4.0 u gate.
    world.registry.for_each([&](const Entity &cand) {
        if (cand.handle == player.handle) return;
        if (cand.seats.empty()) return;                     // no seat bones
        if (!cand.alive || cand.health <= 0) return;        // [orig: Flags & 2 skip @0x435e28]
        if ((cand.flags & 2u) != 0) return;
        // A live enemy occupant rejects the whole vehicle [orig: Vehicle_HasEnemyOccupant
        // @0x435e58]. (The emplaced-gun carrier legs are unmodeled — D-AI-11.)
        if (vehicle_has_enemy_occupant(world, cand, player)) return;

        for (int i = 0; i < static_cast<int>(cand.seats.size()); ++i) {
            const Seat &s = cand.seats[i];
            if (s.type == SeatType::None) continue;          // [orig: boneIdx == 0 skip]
            if (s.occupant.valid()) continue;                // [orig: mountHandles != 0xFFFF]
            const Vec3 sp = seat_world_pos(cand, s);
            const double dx = static_cast<double>(sp.x) - eye_x;
            const double dy = static_cast<double>(sp.y) - eye_y;
            const double dz = static_cast<double>(sp.z) - eye_z + 0.1875;
            const double horiz = std::sqrt(dx * dx + dy * dy);
            const double d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
            const int32_t horiz_fx = static_cast<int32_t>(horiz * 65536.0);
            const int32_t d3_fx = static_cast<int32_t>(d3 * 65536.0);
            // [orig: @0x436123 — v66 <= 0x40000 && v24 <= maxDistance]
            if (horiz_fx > 0x40000 || d3_fx > max_dist3d) continue;
            // Score = horizontal + 3D/512 [orig: candidateScore = v66 + (v24 >> 9)].
            const int32_t score = horiz_fx + (d3_fx >> 9);
            if (score >= best_score) continue;
            // LOS gate LAST [orig: Entity_CheckLineOfSightTerrainAndEntities @0x436183];
            // excludes the requester and the seat's carrier.
            if (world.ai != nullptr) {
                const int32_t a[3] = {static_cast<int32_t>(player.position.x * 65536.0f),
                                      static_cast<int32_t>(player.position.y * 65536.0f),
                                      static_cast<int32_t>(player.position.z * 65536.0f)};
                const int32_t b[3] = {static_cast<int32_t>(sp.x * 65536.0f),
                                      static_cast<int32_t>(sp.y * 65536.0f),
                                      static_cast<int32_t>(sp.z * 65536.0f)};
                if (!world.ai->line_of_sight_clear(world, a, b, player.handle, cand.handle))
                    continue;
            }
            best_score = score;
            out.vehicle = cand.handle;
            out.seat_index = i;
            out.type = s.type;
            found = true;
        }
    });
    return found;
}

bool player_toggle_vehicle_mount(World &world, EntityHandle player) {
    Entity *p = world.registry.get(player);
    if (p == nullptr || !p->alive || p->health <= 0) return false;

    if (!p->mounted) {
        // Standing ON a seat-bearing carrier -> best free seat on it [orig: the Flags 0x200
        // deck branch @0x4368cf -> Entity_FindBestSeatSlot @0x4351f0; our platform contact
        // is ground_target].
        Entity *g = world.registry.get(p->ground_target);
        if (g != nullptr && !g->seats.empty()) {
            const int si = world.commands.find_best_seat(*g, player);
            if (si >= 0 && attach_to_seat_index(world, player, g->handle, si)) return true;
        }
        NearestSeatHit hit;
        if (find_nearest_free_seat(world, *p, hit))
            return attach_to_seat_index(world, player, hit.vehicle, hit.seat_index);
        return false;
    }

    // Mounted: a seat in scan reach swaps [orig: @0x4369ac -> TryEnterNearestVehicle],
    // else detach [orig: Entity_SendDetachPacket @0x4369c7 — the authority applies
    // directly through the same server leg].
    NearestSeatHit hit;
    if (find_nearest_free_seat(world, *p, hit))
        return attach_to_seat_index(world, player, hit.vehicle, hit.seat_index);
    return entity_detach_from_vehicle(world, player);
}

} // namespace opennova::world
