#include "world/vehicle_attach.h"

#include <bitset>
#include <cmath>
#include <cstdint>

#include "world/ai.h"
#include "world/collision.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr std::size_t kEntityHandleDomain =
        static_cast<std::size_t>(EntityRegistry::kPoolCount) << 12;

bool is_blocking_enemy_rider(const Entity &entity, const Entity &requester) {
    if (entity.handle == requester.handle) return false;
    if (entity.health <= 0 || !entity.alive) return false;
    if (entity.team == requester.team) return false;
    return entity.mounted;
}

// A live ENEMY occupies `vehicle` (or one of its carried guns — gun-carrier traversal is
// unmodeled; tracked D-NET-157). Scans pool 0, skipping dead / self / same-team occupants,
// so same-team co-boarding never blocks. [orig: Vehicle_HasEnemyOccupant @0x4359F0 —
// pool-0 scan, dead skip, +0x162 team compare @0x435a5f, parentEntity(0x16C) == root hit]
bool vehicle_has_enemy_occupant(const World &world, const Entity &vehicle,
                                const Entity &requester) {
    const std::size_t pool_capacity = world.registry.pool_capacity(0);
    for (std::size_t slot = 0; slot < pool_capacity; ++slot) {
        const Entity *entity =
                world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
        if (entity != nullptr && is_blocking_enemy_rider(*entity, requester) &&
            entity->mount_target == vehicle.handle)
            return true;
    }
    return false;
}

// Requester-relative hostile mount targets for one synchronous attach query. The
// source-of-truth remains the witnessed pool-0 rider parent link; this is only a
// per-call index, so attach/detach, death, team changes, restore, and handle reuse
// need no cross-frame invalidation.
class HostileMountIndex {
public:
    HostileMountIndex(const World &world, const Entity &requester,
                      AttachLabelScanStats *stats) {
        if (stats != nullptr) ++stats->enemy_occupancy_registry_passes;
        const std::size_t pool_capacity = world.registry.pool_capacity(0);
        for (std::size_t slot = 0; slot < pool_capacity; ++slot) {
            const Entity *entity =
                    world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
            if (entity == nullptr || !is_blocking_enemy_rider(*entity, requester) ||
                !entity->mount_target.valid())
                continue;
            const std::size_t target = entity->mount_target.packed;
            if (target < blocked_.size()) blocked_.set(target);
        }
    }

    bool blocks(EntityHandle vehicle) const {
        return vehicle.valid() && vehicle.packed < blocked_.size() &&
               blocked_.test(vehicle.packed);
    }

private:
    std::bitset<kEntityHandleDomain> blocked_;
};

bool candidate_relevant_for_mode(const Entity &candidate, bool armory_mode) {
    return armory_mode ? !candidate.armory_points.empty() : !candidate.seats.empty();
}

// Shared host attach write block. Retail splits UseGun from ordinary vehicle slots at
// the flags write; the remaining relationship fields are common.
void attach_apply(World &world, Entity &occ, Entity &veh, int seat_idx, uint8_t bone) {
    presnap_vehicle_attach_heading(world, occ, veh, veh.seats[seat_idx]);
    veh.seats[seat_idx].occupant = occ.handle; // [orig: mountHandles[idx] = handle @0x494746]
    occ.mount_type = veh.seats[seat_idx].type;
    static_assert((kEntityFlagDrowning | kEntityFlagInAir) == 0xA000u,
                  "the witnessed gunner-mount scrub mask");
    static_assert((kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted) == 0xA040u,
                  "the witnessed vehicle-mount scrub+set mask (~mask == 0xFFFF5FBF)");
    if (occ.mount_type == SeatType::Gunner) {
        occ.flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
        occ.engine_flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
        // [orig: Entity_AttachToUseGunSlot @0x546c56-0x546c7c]
    } else {
        occ.flags = (occ.flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                    kEntityFlagMounted;
        occ.engine_flags =
                (occ.engine_flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                kEntityFlagMounted;
        // [orig: Entity_AttachToVehicleSlot @0x494752-0x494775]
    }
    occ.mount_target = veh.handle;                 // [orig: parentEntity(0x16C) = vehicle]
    occ.mount_target_net_id = veh.net_id;
    occ.mount_target_bms_id = veh.bms_id;
    occ.mount_target_spawn_origin = veh.spawn_origin;
    occ.mount_bone = bone;                          // [orig: attachBoneId(0x157) = bone]
    occ.mount_seat = static_cast<int8_t>(seat_idx); // [orig: parentSlot(0x168) = slotType]
    occ.mounted = true;
    occ.mounted_config_valid = veh.emplaced_config_valid;
    occ.mounted_config = veh.emplaced_config_valid ? veh.emplaced_config : 0;
    if (occ.mount_type == SeatType::Gunner)
        vehicle_bind_use_gun_slot(world, occ, veh);
    pose_mounted_occupant(world, occ, veh, veh.seats[seat_idx]);
    // Success clears the movement stance bits [orig: MoveOrder &= ~0x300 @0x435c42 + the
    // prone/crouch latch clears @0x435c54/@0x435c59].
    occ.net_stance_bits = 0;
    vehicle_claim_primary_occupant(world, veh, occ.handle, occ.mount_type); // [orig: +368 @0x4946d0]
}

// Local-point world position: the same local rotate the per-tick pose applies
// (pose_mounted_occupant), our stand-in for the posed bone transform
// [orig: build_bone_attachment_matrix @0x56c630 in the scan @0x435fe7].
Vec3 local_point_world_pos(const Entity &veh, const Vec3 &local) {
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double a = static_cast<double>(-veh.yaw) * kDeg2Rad;
    const double ca = std::cos(a), sa = std::sin(a);
    Vec3 p;
    p.x = veh.position.x + static_cast<float>(local.x * ca - local.y * sa);
    p.y = veh.position.y + static_cast<float>(local.x * sa + local.y * ca);
    p.z = veh.position.z + local.z;
    return p;
}

Vec3 seat_world_pos(const Entity &veh, const Seat &s) {
    return local_point_world_pos(veh, s.seat_local);
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

    // 2. Seat classification by the exact wire bone. The byte is a 1-based index into
    //    the model USRP table (48-byte rows, name at +32), which is exactly how production
    //    seat specs assign bone_index. Unknown/unrecognized rows reject; retail never
    //    substitutes another free seat.
    //    [orig: Entity_GetBoneSlotType @0x434ED0 -> slotType 0 reject @0x435B14;
    //    seat-block index @0x435BA9]
    int seat_idx = -1;
    for (int i = 0; i < static_cast<int>(veh->seats.size()); ++i) {
        if (veh->seats[i].type == SeatType::None) continue;
        if (veh->seats[i].bone_index == bone) {
            seat_idx = i;
            break;
        }
    }
    if (seat_idx < 0) return false;

    // 3. Enemy-occupant gate [orig: @0x4359F0 via the reject @0x435b4b-ish].
    if (vehicle_has_enemy_occupant(world, *veh, *occ)) return false;

    // 4. Occupancy: the matched seat must be free (or already ours) [orig: @0x435BA9].
    const Seat &s = veh->seats[seat_idx];
    if (s.occupant.valid() && s.occupant != player) return false;

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
    const uint16_t target_wire_handle = occ->mount_target.packed;
    Entity *veh = world.registry.get(occ->mount_target);
    // [orig: Entity_DetachFromVehicle @0x4355F0] MoveOrder &= ~0x300 (stance clear), then
    // every matching seat handle on the mount target releases (all 10 slots in the
    // original; our seat vector sweeps by occupant), Flags &= ~0x40 and the mount trio
    // clears. The EquippedSlot backup is restored for a player and cleared for an
    // NPC below. The claimant-only ground sound clear/stop now rides
    // vehicle_release_primary_occupant; the engine-state 7 / attached-effect release
    // remains unmodeled (D-NET-157).
    occ->net_stance_bits = 0;
    if (veh != nullptr) {
        for (Seat &s : veh->seats) {
            if (s.occupant == player) s.occupant = EntityHandle{}; // [orig: -> 0xFFFF]
        }
    }
    vehicle_release_use_gun_slot(*occ, veh);
    occ->flags &= ~kEntityFlagMounted;          // [orig: Flags &= ~0x40]
    occ->engine_flags &= ~kEntityFlagMounted;
    occ->mount_target = EntityHandle{}; // [orig: +0x16C = 0]
    occ->mount_target_net_id = 0;
    occ->mount_target_bms_id = 0;
    occ->mount_target_spawn_origin = 0;
    occ->mount_bone = 0;           // [orig: +0x157 = 0]
    occ->mount_seat = -1;          // [orig: +0x168 = 0]
    occ->mount_type = SeatType::None;
    occ->mounted = false;
    occ->mounted_config_valid = false;
    occ->mounted_config = 0;
    if (veh != nullptr) {
        // [orig: the +368 leg @0x4356e9..0x43577c — runs only for the claimant]
        vehicle_release_primary_occupant(world, *veh, player);
    } else if (claim_capable_seat) {
        // The vehicle is already gone; the stored identity carries the stop (host
        // cleanup — a spurious stop is idempotent downstream).
        emit_vehicle_control_stopped(world, target_net_id, target_bms_id,
                                     target_spawn_origin, target_wire_handle);
    }
    return true;
}

namespace {

// LOS between the player position and a candidate point, excluding both entities
// [orig: Entity_CheckLineOfSightTerrainAndEntities @0x436183 in the scan; the label draw's
// Physics_RaycastTerrainAndSectors @0x5a3609 — both cast from the player POSITION].
bool point_los_clear(World &world, const Entity &player, const Entity &cand, const Vec3 &sp) {
    if (world.ai == nullptr) return true;
    const int32_t a[3] = {static_cast<int32_t>(player.position.x * 65536.0f),
                          static_cast<int32_t>(player.position.y * 65536.0f),
                          static_cast<int32_t>(player.position.z * 65536.0f)};
    const int32_t b[3] = {static_cast<int32_t>(sp.x * 65536.0f),
                          static_cast<int32_t>(sp.y * 65536.0f),
                          static_cast<int32_t>(sp.z * 65536.0f)};
    return world.ai->line_of_sight_clear(world, a, b, player.handle, cand.handle);
}

// The shared per-entity reject set of the scan and the label pass
// [orig: @0x435e28..0x435eae / @0x5a335a..0x5a3395 — dead/destroyed skip, itemDef/model
// presence, enemy-occupant reject; the carrier legs are unmodeled (D-AI-11)].
bool scan_entity_rejected(const Entity &cand, const Entity &player,
                          const HostileMountIndex &hostile_mounts) {
    if (cand.handle == player.handle) return true;
    if (!cand.alive || cand.health <= 0) return true; // [orig: Flags & 2 skip]
    if ((cand.flags & 2u) != 0) return true;
    return hostile_mounts.blocks(cand.handle);
}

// The scan container: the player's proximity slice when the per-tick tables are
// live [orig: entity+444/448 @0x435d60], the registry sweep only for sliceless
// worlds (headless callers that never ran the table build). An absent slice on
// a live world scans nothing — retail's BSS-zero start behaves the same way.
template <typename Fn>
void for_each_scan_candidate(World &world, const Entity &player, Fn &&fn) {
    CollisionWorld *cw = world.collision;
    if (cw != nullptr && cw->attach_candidate_slices_authoritative()) {
        int32_t n = 0;
        const EntityHandle *slice = cw->candidate_slice(player.handle, n);
        for (int32_t i = 0; i < n; ++i) {
            const Entity *c = world.registry.get(slice[i]);
            if (c != nullptr) fn(*c);
        }
        return;
    }
    world.registry.for_each(fn);
}

} // namespace

static bool find_nearest_free_seat_impl(World &world, const Entity &player,
                                        NearestSeatHit &out, bool armory_mode,
                                        const HostileMountIndex &hostile_mounts) {
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

    // One candidate point [orig: the shared score/gate block @0x435fe7..0x4361c2 (seats) =
    // @0x43624d..0x436417 (armory points)]. Returns true when it becomes the best hit.
    const auto consider = [&](const Entity &cand, const Vec3 &sp, int index, SeatType type) {
        const double dx = static_cast<double>(sp.x) - eye_x;
        const double dy = static_cast<double>(sp.y) - eye_y;
        const double dz = static_cast<double>(sp.z) - eye_z + 0.1875;
        const double horiz = std::sqrt(dx * dx + dy * dy);
        const double d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int32_t horiz_fx = static_cast<int32_t>(horiz * 65536.0);
        const int32_t d3_fx = static_cast<int32_t>(d3 * 65536.0);
        // [orig: @0x436123 — v66 <= 0x40000 && v24 <= maxDistance]
        if (horiz_fx > 0x40000 || d3_fx > max_dist3d) return;
        // Score = horizontal + 3D/512 [orig: candidateScore = v66 + (v24 >> 9)].
        const int32_t score = horiz_fx + (d3_fx >> 9);
        if (score >= best_score) return;
        // LOS gate LAST [orig: @0x436183].
        if (!point_los_clear(world, player, cand, sp)) return;
        best_score = score;
        out.vehicle = cand.handle;
        out.seat_index = index;
        out.type = type;
        found = true;
    };

    // The original walks the player's proximity list [orig: entity+444/448
    // @0x435d60 — the slice Entity_BuildProximityListsFromPools fills @0x4b8eb0].
    // for_each_scan_candidate below walks that same slice; the former
    // whole-registry sweep (the container rebase) was behavior-equal inside the
    // 4.0 u gate but ran O(world) per frame. Hostile occupancy is indexed once
    // per query above so its witnessed pool-0 scan is not multiplied here.
    for_each_scan_candidate(world, player, [&](const Entity &cand) {
        if (!candidate_relevant_for_mode(cand, armory_mode)) return;
        // While seated, the OWN vehicle's other seats are LOS-blocked by its hull in
        // retail (the ray walks pool-1 collision models) — that is why USE exits
        // instead of cycling seats. Pool-1 hulls are unbuilt (D-AI-11 j), so the hull
        // occlusion is modeled as this candidate skip (D-AI-11 j).
        if (player.mounted && cand.handle == player.mount_target) return;
        if (scan_entity_rejected(cand, player, hostile_mounts)) return;
        if (!armory_mode) {
            // [orig: the searchMode-0 seat loop @0x435f1e]
            for (int i = 0; i < static_cast<int>(cand.seats.size()); ++i) {
                const Seat &s = cand.seats[i];
                if (s.type == SeatType::None) continue; // [orig: boneIdx == 0 skip]
                if (s.occupant.valid()) continue;       // [orig: mountHandles != 0xFFFF]
                consider(cand, seat_world_pos(cand, s), i, s.type);
            }
            return;
        }
        // [orig: the armory leg @0x4361ee — attrib 0x80000 + "armory*" points, no
        // occupancy, seatType 4]. armory_points is non-empty only for Armory-attrib items.
        for (int i = 0; i < static_cast<int>(cand.armory_points.size()); ++i)
            consider(cand, local_point_world_pos(cand, cand.armory_points[i]), i,
                     SeatType::ArmoryPoint);
    });
    return found;
}

bool find_nearest_free_seat(World &world, const Entity &player, NearestSeatHit &out,
                            bool armory_mode) {
    const HostileMountIndex hostile_mounts(world, player, nullptr);
    return find_nearest_free_seat_impl(
            world, player, out, armory_mode, hostile_mounts);
}

void collect_attach_labels(World &world, const Entity &player, bool armory_mode,
                           bool can_fire, std::vector<AttachLabel> &out,
                           AttachLabelScanStats *stats) {
    const HostileMountIndex hostile_mounts(world, player, stats);
    // No nearest hit -> no labels at all [orig: the Entity_FindNearestSeatOrArmory gate
    // @0x5a32e2 brackets the whole pass].
    NearestSeatHit nearest;
    if (!find_nearest_free_seat_impl(
                world, player, nearest, armory_mode, hostile_mounts))
        return;

    // One label point [orig: the shared draw block @0x5a3553..0x5a36c9 — the +0.1875 u
    // lift, the 4.0 u 3D gate from the player POSITION, LOS, then the draw].
    const auto emit = [&](const Entity &cand, const Vec3 &point, int index, SeatType type,
                          bool armory) {
        Vec3 lifted = point;
        lifted.z += 0.1875f; // [orig: point.z = boneZ + 12288 @0x5a3585]
        const double dx = static_cast<double>(lifted.x) - static_cast<double>(player.position.x);
        const double dy = static_cast<double>(lifted.y) - static_cast<double>(player.position.y);
        const double dz = static_cast<double>(lifted.z) - static_cast<double>(player.position.z);
        const double d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
        // [orig: the label radius @0x5a35f0 — dist < 0x40000 (4.0 u), FULL 3D, from the
        // entity position (not the eye)]
        if (static_cast<int32_t>(d3 * 65536.0) >= 0x40000) return;
        if (!point_los_clear(world, player, cand, lifted)) return;
        AttachLabel label;
        label.entity = cand.handle;
        label.seat_index = index;
        label.type = type;
        label.armory = armory;
        label.nearest = cand.handle == nearest.vehicle && index == nearest.seat_index;
        label.world_pos = lifted;
        out.push_back(label);
    };

    for_each_scan_candidate(world, player, [&](const Entity &cand) {
        // A ready weapon limits labels to the nearest entity [orig: !Player_CanFireWeapon()
        // || entity == nearest_entity @0x5a3354].
        if (can_fire && cand.handle != nearest.vehicle) return;
        if (!candidate_relevant_for_mode(cand, armory_mode)) return;
        if (scan_entity_rejected(cand, player, hostile_mounts)) return;
        if (!armory_mode) {
            // [orig: the seat-label loop @0x5a3464; occupied seats never label @0x5a348f]
            for (int i = 0; i < static_cast<int>(cand.seats.size()); ++i) {
                const Seat &s = cand.seats[i];
                if (s.type == SeatType::None) continue;
                if (s.occupant.valid()) continue;
                emit(cand, seat_world_pos(cand, s), i, s.type, false);
            }
            return;
        }
        // [orig: the armory-label walk @0x5a36f5..@0x5a38e2]
        for (int i = 0; i < static_cast<int>(cand.armory_points.size()); ++i)
            emit(cand, local_point_world_pos(cand, cand.armory_points[i]), i,
                 SeatType::ArmoryPoint, true);
    });
}

bool player_toggle_vehicle_mount(World &world, EntityHandle player) {
    Entity *p = world.registry.get(player);
    if (p == nullptr || !p->alive || p->health <= 0) return false;

    if (!p->mounted) {
        // Standing ON a seat-bearing carrier -> best free seat on it [orig: the Flags 0x200
        // deck branch @0x4368cf -> Entity_FindBestSeatSlot @0x4351f0; represented by
        // the generic ground_target carrier, not a CL ladder volume].
        Entity *g = world.registry.get(p->ground_target);
        if (g != nullptr && !g->seats.empty()) {
            const int si = world.commands.find_best_seat(*g, player);
            if (si >= 0 && attach_to_seat_index(world, player, g->handle, si)) return true;
        }
        NearestSeatHit hit;
        if (find_nearest_free_seat(world, *p, hit, false))
            return attach_to_seat_index(world, player, hit.vehicle, hit.seat_index);
        return false;
    }

    // Mounted: a seat in scan reach swaps [orig: @0x4369ac -> TryEnterNearestVehicle],
    // else detach [orig: Entity_SendDetachPacket @0x4369c7 — the authority applies
    // directly through the same server leg].
    NearestSeatHit hit;
    if (find_nearest_free_seat(world, *p, hit, false))
        return attach_to_seat_index(world, player, hit.vehicle, hit.seat_index);
    return entity_detach_from_vehicle(world, player);
}

} // namespace opennova::world
