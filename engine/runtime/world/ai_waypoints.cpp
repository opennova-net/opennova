#include <runtime/world/ai.h>

// Waypoint targeting and movement (P1: GROUND_FOLLOWWP), plus the vehicle-physics
// AI/parked input staging.

#include <runtime/world/angle.h>
#include <runtime/world/dir_table.h>
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include <runtime/world/world.h>
#include <runtime/world/vehicle_attach.h>
#include <base/io/fixed.h>
#include <base/io/bam.h>

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

// Waypoint targeting + movement (P1: GROUND_FOLLOWWP).
// ----------------------------------------------------------------------------

// [orig: AIWaypoint_UpdateTarget @0x457380] wp = brain+52 (= b.f[13..]). Deviation:
// kWpResolved stores the pool-3 node INDEX (orig stored the resolved pointer); the
// dword values written are byte-identical.
int ai_waypoint_update_target(AiBrain &b, const int32_t pos[3], const NavNodeTable &nav) {
    int32_t type = b.f[AiBrain::kWpType];
    if (type == 1) { // nav node
        int32_t navMeshId = b.f[AiBrain::kWpChannel];
        const NavChannel *ch = (navMeshId != 0) ? nav.channel(navMeshId) : nullptr;
        if (!ch || ch->count == 0) return -1; // [orig: navMeshId==0 || dword_A71DD4[34*id]==0]
        // Past the two zero tests nothing is bounded: the node indexes the flat
        // record block (a node past 31 reads the next record's words) and the
        // pool-3 slot resolves unchecked, so an index no marker filled is the
        // zeroed slot at the origin with radius 0 [orig: `mov edx,
        // Buffer+8[ecx*4]` @0x457476, Pool_GetEntryUnchecked(3, idx) @0x457481].
        const int32_t nodeIdx = nav.entry_index(navMeshId, b.f[AiBrain::kWpNode]);
        const NavEntry &node = nav.slot(nodeIdx);
        b.f[AiBrain::kWpResolved] = nodeIdx;  // [orig: waypointData+12 = navEntry]
        int32_t dx = node.f[1] - pos[0];
        int32_t dz = node.f[2] - pos[1];
        int32_t dy = node.f[3] - pos[2];
        wp_dist_bearing(dx, dz, dy, b.f[AiBrain::kWpDistance], b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kWpNodeVal] = node.f[0]; // [orig: *navEntry]
        b.f[AiBrain::kWpExtra] = node.f[4];   // [orig: navEntry[4]]
        return 0;
    }
    if (type == 3) { // literal coordinate
        int32_t dx = b.f[AiBrain::kWpCoordX] - pos[0];
        int32_t dz = b.f[AiBrain::kWpCoordY] - pos[1];
        int32_t dy = b.f[AiBrain::kWpCoordZ] - pos[2];
        wp_dist_bearing(dx, dz, dy, b.f[AiBrain::kWpDistance], b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kWpExtra] = 0;                          // [orig: waypointData+44 = 0]
        b.f[AiBrain::kWpNodeVal] = b.f[AiBrain::kWpCoordSrc];// [orig: +40 = +28]
        return 0;
    }
    return (type == 2) ? 0 : -1; // [orig: type 2 -> 0 (no-op); any other -> -1]
}

void AiSystem::mark_waypoint_visited(AiEntity &e, World &world, int32_t list, int32_t node) {
    // The original reads the group key from entity+284 and the single key from
    // entity+124. In the rebase, the live registry Entity owns those script-facing
    // identities; fall back to the AiEntity mirrors for isolated motor tests.
    // [orig: AI_UpdateWaypointMovement @0x457c6d..0x457c88]
    const Entity *entity = world.registry.get(e.handle);
    const int32_t group = entity != nullptr
                                  ? static_cast<int32_t>(entity->group_id)
                                  : static_cast<int32_t>(static_cast<int16_t>(e.relmat_id));
    const int32_t ssn = entity != nullptr ? static_cast<int32_t>(entity->net_id) : e.net_id;

    relmat_calls.push_back({1, group, list, node}); // SetBitB first
    relmat_calls.push_back({0, ssn, list, node});   // SetBitA second
    world.script.relations.mark_waypoint_visited(ssn, group, list, node);
}

// [orig: AI_UpdateWaypointMovement @0x457bd0] advance along the path; write the working
// target transform + out-speed. The return value is unused by the caller; we mirror the
// original's "freeze on path end / no waypoint" branches faithfully.
int AiSystem::update_waypoint_movement(AiEntity &e, World &world) {
    AiBrain &b = e.brain;
    int32_t moveSpeed = (b.f[AiBrain::kCurState] == 16) ? b.f[AiBrain::kSpeedB]
                                                        : b.f[AiBrain::kSpeedA];
    int wr = ai_waypoint_update_target(b, e.pos, nav);
    if (wr == -1) { // no resolvable waypoint -> freeze at current transform
        b.f[AiBrain::kWorkPosX] = e.pos[0];
        b.f[AiBrain::kWorkPosY] = e.pos[1];
        b.f[AiBrain::kWorkPosZ] = e.pos[2];
        b.f[AiBrain::kWorkHeading] = e.heading;
        b.f[AiBrain::kWorkPitch] = e.pitch;
        b.f[AiBrain::kWorkRoll] = e.roll;
        b.f[AiBrain::kOutSpeed] = 0;
        return e.roll; // [orig: returns *(entity+24); unused]
    }

    int32_t animTime = b.f[AiBrain::kWpNodeVal];     // aiState[23]
    if (b.f[AiBrain::kWpDistance] < animTime) {       // within arrival threshold -> advance
        int32_t ch = b.f[AiBrain::kWpChannel];        // aiState[14]
        int32_t kf = b.f[AiBrain::kWpNode];           // aiState[15] (pre-increment)
        b.f[AiBrain::kStoredKeyTime] = animTime;      // aiState[35]
        b.f[AiBrain::kAnimFlag] = 0;                  // aiState[32]
        mark_waypoint_visited(e, world, ch, kf);
        ++b.f[AiBrain::kWpNode];                      // ++aiState[15]
        const NavChannel *nc = nav.channel(ch);
        int32_t numKeyframes = nc ? nc->count : 0;    // dword_A71DD4[34*ch]
        if (b.f[AiBrain::kWpNode] >= numKeyframes) {
            int32_t loopflag = nc ? nc->loopflag : 0; // Buffer[34*ch]
            if ((loopflag & 1) != 0) {                // one-shot: terminate + freeze
                b.f[AiBrain::kWpType] = 0;            // aiState[13]=0 (path done)
                b.f[AiBrain::kWpNode] = numKeyframes - 1;
                b.f[AiBrain::kWorkPosX] = e.pos[0];
                b.f[AiBrain::kWorkPosY] = e.pos[1];
                b.f[AiBrain::kWorkPosZ] = e.pos[2];
                b.f[AiBrain::kWorkHeading] = e.heading;
                b.f[AiBrain::kWorkPitch] = e.pitch;
                b.f[AiBrain::kOutSpeed] = 0;
                b.f[AiBrain::kWorkRoll] = e.roll;
                return e.roll; // [orig: returns entity+4 (a ptr); unused]
            }
            b.f[AiBrain::kWpNode] = 0;                // loop: wrap to node 0
        }
    }

    int32_t timeDelta = b.f[AiBrain::kWpDistance] - b.f[AiBrain::kWpNodeVal]; // [22]-[23]
    bool halve = (b.f[AiBrain::kCurState] == 17) ? (timeDelta < 16 * moveSpeed)
                                                 : (timeDelta < moveSpeed * b.f[AiBrain::kStep]);
    if (halve) moveSpeed >>= 1;

    const NavEntry &node = nav.slot(b.f[AiBrain::kWpResolved]); // aiState[16]
    int32_t nodeX = node.f[1]; // *(waypointPtr+4)
    int32_t nodeY = node.f[2]; // *(waypointPtr+8)
    int32_t result = b.f[AiBrain::kWpBearing];                   // aiState[21]
    b.f[AiBrain::kWorkPosX] = nodeX;
    b.f[AiBrain::kWorkPitch] = 0;   // aiState[133]
    b.f[AiBrain::kWorkRoll] = 0;    // aiState[134]
    b.f[AiBrain::kWorkPosY] = nodeY;
    b.f[AiBrain::kWorkHeading] = result;
    b.f[AiBrain::kOutSpeed] = moveSpeed;
    return result;
}

// [orig: Entity_FindNearestTriggerByType @0x407EA0] The subtraction and
// squared-distance sum wrap at 32 bits, after arithmetic Q16 -> integer shifts.
int32_t AiSystem::nearest_route_node(const AiEntity &e, uint32_t list) const {
    if (list >= 123 && list <= 125) return e.slot.f[38];
    if (list >= 126) return 0;
    const NavChannel *ch = nav.channel(static_cast<int>(list));
    if (ch == nullptr) return 0;
    int32_t best = 0;
    int32_t best_distance = INT32_MAX;
    // The walk runs to the raw count over the flat record block and resolves
    // every slot unchecked: a count past 32 reads on into the next record's
    // words, and a slot no marker filled measures to the origin.
    // [orig: @0x407F0B..0x407F6B — `lea ebx, Buffer+8[34*list]`, one dword per
    //  node, Pool_GetEntryUnchecked(3, word) @0x407F17, `cmp ebp, count; jl`]
    for (int i = 0; i < ch->count; ++i) {
        const NavEntry &node = nav.slot(nav.entry_index(static_cast<int>(list), i));
        uint32_t squared = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t delta = static_cast<int32_t>(
                    uint32_t(e.pos[axis]) - uint32_t(node.f[axis + 1])) >> 16;
            squared += uint32_t(delta) * uint32_t(delta);
        }
        const int32_t distance = static_cast<int32_t>(squared);
        if (distance <= best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

// The per-leg turn budget the ground drive legs and three of the route writers
// seed: |Yaw - bearing| (a wrapping 32-bit sub, then cdq/xor/sub abs) through a
// signed 32-bit idiv by ((brain[35] >> 15) + 32), THEN `shl eax,5`.
// [orig: ctan @0x48988E..0x4898A2 and cveh @0x48BCC5..0x48BCD9 (the drive
//  legs); Entity_SetWaypointByTeam @0x43CE3B..0x43CE4F; Entity_SetWaypointForTeam
//  @0x43DE75..0x43DE89; WacCmd_SsnToWp @0x4F1DA4..0x4F1DB9]
static int32_t route_turn_budget(const AiBrain &b, int32_t heading) {
    const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
    const int32_t err = iabs32(io::bam_sub(heading, b.f[AiBrain::kWpBearing]));
    return static_cast<int32_t>(static_cast<uint32_t>(err / denom) << 5);
}

// The brain half every redirect writer shares: mode, list and node copied from
// the slot words the writer just stored (commands 0 and 123..127 included; their
// node is an authored operand such as a carrier SSN, never clamped to a route).
// [orig: Entity_SetWaypointByTeam @0x43CE02..0x43CE30 (pool 0) and
//  @0x43CEDD..0x43CEFE (pool 1); Entity_SetWaypointForTeam @0x43DDA8..0x43DDCB
//  (pool 0) and @0x43DE3C..0x43DE6A (pool 1); WacCmd_SsnToWp @0x4F1D6A..0x4F1D99]
void AiSystem::copy_route_order_to_brain(AiEntity &e) {
    e.brain.f[AiBrain::kWpType] = e.slot.f[35];
    e.brain.f[AiBrain::kWpChannel] = e.slot.f[37];
    e.brain.f[AiBrain::kWpNode] = e.slot.f[38];
}

// The seed that follows the copy in three of the five writer legs: the waypoint
// refresh, whose -1 result is not tested, then the turn budget from the entity
// Yaw. The group writer's pool-1 leg and the single writer's pool-0 leg stop at
// the copy, so a redirected vehicle keeps its current leg's budget there.
// [orig: Entity_SetWaypointByTeam @0x43CE36..0x43CE4F; Entity_SetWaypointForTeam
//  @0x43DE70..0x43DE89; WacCmd_SsnToWp @0x4F1D9F..0x4F1DB9]
void AiSystem::seed_route_turn_budget(AiEntity &e) {
    ai_waypoint_update_target(e.brain, e.pos, nav);
    e.brain.f[AiBrain::kAnimFlag] = route_turn_budget(e.brain, e.heading);
}

// Aircraft steering uses full-angle x87 FCOS, independently of the vehicle
// footprint table below. [orig: Entity_UpdateAircraftPhysics @0x4917E6]
static int32_t flight_cos22(int32_t bam) {
    const double a = static_cast<double>(bam) * io::kRadiansPerBam;
    return static_cast<int32_t>(std::cos(a) * io::kQ22One);
}

// x87 sin at the same 2^22 scale — the cbot slip block multiplies fsin by the
// 4194304.0 constant [orig: dbl_7C3600, consumed @0x48E43D].
static int32_t avoid_sin22(int32_t bam) {
    const double a = static_cast<double>(bam) * io::kRadiansPerBam;
    return static_cast<int32_t>(std::sin(a) * io::kQ22One);
}

// Which pool-1 neighbours the avoid-brake walk considers, by their ItemTypeIndex
// (entity+0x1C). The ground forms skip index 0 only; the boat and aircraft forms
// load 1 into ecx for the walk's decrement and compare the index against that
// register, so they consider ONLY neighbours of ItemTypeIndex 1 (the second
// items.def row).
// [orig: ground `cmp dword ptr [edi+1Ch],0; jz` — Entity_UpdateVehiclePhysics
//  @0x48BDD9, Entity_UpdateTankVehiclePhysics @0x4899A2,
//  Entity_UpdateLightVehiclePhysics @0x484F7F, Entity_ProcessInfantryPhysics
//  @0x46EE70; boat/air `mov ecx,1; sub [count],ecx ... cmp [edi+1Ch],ecx; jnz`
//  — Entity_UpdateWatercraftPhysics @0x48E5AA/@0x48E5C5,
//  Entity_ProcessAirVehiclePhysics @0x470B08/@0x470B23,
//  Entity_UpdateAircraftPhysics @0x4919D1/@0x4919E6]
enum class AvoidBrakeWalk { SkipIndexZero, OnlyIndexOne };

// The pool-1 avoid BRAKE, shared by the ground, cbot and aircraft AI legs: the
// sites are instruction-identical (footprint ellipse, dead-ahead cone,
// id/frame-keyed factor) apart from the walk gate above [orig: ground
// @0x48bd8f-0x48bf26; cbot @0x48E577..0x48E756; Entity_UpdateAircraftPhysics
// @0x4919A4..0x491B67]:
// for every admitted pool-1 neighbor whose heading-aware footprint ellipse
// overlaps ours (+1.0 u) AND that sits within ~30 deg of dead ahead, the
// command speed multiplies by an id/frame-keyed factor in [0.25, 0.75) per
// tick — vehicles brake behind obstacles; deflecting off them through the hull
// contact was never the retail path-follow behavior.
static int32_t vehicle_avoid_brake(World &world, Entity &veh, int32_t heading,
                                   int32_t cmd_speed, AvoidBrakeWalk walk) {
    const int32_t self_bound = to_fixed(veh.bound_radius);
    const int32_t sx = to_fixed(veh.position.x);
    const int32_t sy = to_fixed(veh.position.y);
    const int32_t sz = to_fixed(veh.position.z);
    const size_t cap = world.registry.pool_capacity(1);
    for (size_t si = 0; si < cap; ++si) {
        const Entity *o =
                world.registry.get(EntityHandle::make(1, static_cast<int>(si)));
        if (o == nullptr || o->handle == veh.handle) continue; // [orig: @0x48be19]
        // The pool walk's live gate is an ITEM-TYPE test, not a radius test:
        // retail reads entity+0x1C (ItemTypeIndex) and applies the family's
        // AvoidBrakeWalk rule [orig: the gates listed at AvoidBrakeWalk;
        //  ItemTypeIndex written at Entity_SpawnFromBMSRecord @0x40EBFC].
        // (The occupancy half of retail's walk is our null check above:
        //  EntityRegistry::get returns nullptr for an unused slot.)
        if (walk == AvoidBrakeWalk::SkipIndexZero ? o->item_type_index == 0
                                                  : o->item_type_index != 1)
            continue;
        const int32_t ob = to_fixed(o->bound_radius);
        const int32_t reach = ob + self_bound + 0x10000; // [orig: @0x48bdf2]
        const int32_t dx = sx - to_fixed(o->position.x);
        if (iabs32(dx) > reach) continue;
        const int32_t dy = sy - to_fixed(o->position.y);
        if (iabs32(dy) > reach) continue;
        // Carrier chains never brake for each other [orig: @0x48be1d-0x48be25].
        if (o->ground_target == veh.handle || veh.ground_target == o->handle)
            continue;
        const int32_t dz2 = 2 * (sz - to_fixed(o->position.z)); // [orig: @0x48be2d]
        if (iabs32(dz2) > reach) continue;
        const double fdx = static_cast<double>(dx);
        const double fdy = static_cast<double>(dy);
        const double fdz = static_cast<double>(dz2);
        const int32_t dist =
                static_cast<int32_t>(std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz));
        if (dist > reach) continue;
        // Bearing other->self in BAM [orig: fpatan(dy, dx) x 2^32/2pi
        // (dbl @0x7C19D8) @0x48be7a-0x48be89].
        // _ftol2_sse truncates toward zero. The widened conversion also
        // preserves +pi's 0x80000000 word without an out-of-range float cast.
        const int32_t ang = static_cast<int32_t>(static_cast<uint32_t>(
                static_cast<int64_t>(std::atan2(fdy, fdx) * 683565275.5764316)));
        // Directional footprints: r/2 + (r/2)*|cos(yaw - ang)| — an end-on
        // vehicle projects its full bound along the axis, side-on half
        // [orig: the 1024-entry cos table off_849934 @0x48be8f-0x48bef1].
        const int32_t oyaw = o->veh.yaw_seeded
                ? o->veh.yaw_bam
                : bam_heading_from_mission_yaw_deg(static_cast<double>(o->yaw));
        int32_t other_cos22, self_cos22, unused_sin22;
        // Add half a bin, then take the unsigned top ten bits before loading
        // off_849934; continuous cosine changes the overlap/brake threshold.
        // [orig: other @0x48BE98..0x48BEA0; self @0x48BEC9..0x48BEDB]
        quantized_dir(io::bam_sub(oyaw, ang), other_cos22, unused_sin22);
        quantized_dir(io::bam_sub(heading, ang), self_cos22, unused_sin22);
        const int32_t other_r =
                static_cast<int32_t>((static_cast<int64_t>(ob >> 1) *
                                      iabs32(other_cos22)) >> 22) +
                (ob >> 1);
        const int32_t self_r =
                static_cast<int32_t>((static_cast<int64_t>(self_bound >> 1) *
                                      iabs32(self_cos22)) >> 22) +
                (self_bound >> 1);
        if (dist > self_r + other_r + 0x10000) continue; // [orig: @0x48bef8]
        // Dead-ahead gate: the other within ~30 deg of the nose
        // [orig: |Yaw - ang - 0x7FFFFF80| <= 357913920 @0x48bf05-0x48bf0f].
        if (iabs32(io::bam_sub(io::bam_sub(heading, ang), 0x7FFFFF80)) > 357913920) continue;
        // The brake factor ((id + (counter << 8)) & 0x7FFF) + 0x4000 — keyed
        // off DcbId and the entity-update counter (dword_24C1948)
        // [orig: @0x48bf17-0x48bf26; the counter reads in
        //  Entity_UpdateVehiclePhysics @0x48BF26, Entity_UpdateTankVehiclePhysics
        //  @0x489AEF, Entity_UpdateLightVehiclePhysics @0x4850CC,
        //  Entity_ProcessInfantryPhysics @0x46EFC1, Entity_UpdateWatercraftPhysics
        //  @0x48E712, Entity_ProcessAirVehiclePhysics @0x470C70 and
        //  Entity_UpdateAircraftPhysics @0x491B2D].
        const int32_t f = static_cast<int32_t>(
                ((static_cast<uint32_t>(veh.net_id) +
                  (world.entity_update_counter << 8)) &
                 0x7FFFu) +
                0x4000u);
        cmd_speed = static_cast<int32_t>(
                (static_cast<int64_t>(f) * cmd_speed + 0x8000) >> 16);
    }
    return cmd_speed;
}

// Spawn-parent lookup, dead-parent refusal, full-pose lift and weighted proximity.
// [orig: @0x434F98, @0x434FA6, @0x434FC3, @0x43501E, @0x435051]
// See ai.h. [orig: Entity_IsBoneInProximity @0x434F90]
bool vehicle_at_spawn_anchor(const World &world, const Entity &veh) {
	int32_t pose[6] = { to_fixed(veh.spawn_position.x), to_fixed(veh.spawn_position.y),
		to_fixed(veh.spawn_position.z), 0, 0, 0 };
	if (veh.veh.spawn_pose_valid && !world.vehicles.resolve_spawn_pose(veh, pose))
		return false;
	const int32_t dx = io::bam_sub(pose[0], to_fixed(veh.position.x));
	const int32_t dy = io::bam_sub(pose[1], to_fixed(veh.position.y));
	const int32_t dz = io::bam_sub(pose[2], to_fixed(veh.position.z)) >> 1;
	const double d = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy +
			static_cast<double>(dz) * dz);
	const int32_t di = d >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(d);
    return di <= 0x80000;
}

// See ai.h. [orig: Entity_CountMountedEntities @0x435970 — the pool-0 walk
// @0x4359a0: `entity[8] != 0` (an ItemDef) @0x4359a9, `!(Flags & 2)`
// @0x4359b2, `+0x28 == target || (+0x28 && +0x28->groundEntity == target)`
// @0x4359c2]. A seated rider's ground link is its vehicle, so the mount
// relation counts alongside a free-standing ground link.
int count_mounted_entities(const World &world, const Entity &veh) {
    int count = 0;
    world.registry.for_each_in_pool(0, [&](const Entity &e) {
        if (!e.has_item_def) return;
        if ((e.flags & kEntityFlagDead) != 0) return;
        const EntityHandle ground = e.mounted ? e.mount_target : e.ground_target;
        if (!ground.valid()) return;
        if (ground == veh.handle) { ++count; return; }
        const Entity *g = world.registry.get(ground);
        if (g != nullptr && g->ground_target == veh.handle) ++count;
    });
    return count;
}

// See ai.h. [orig: @0x48DFD3..0x48DFDF — `occupant->Position.z +
// CameraOffset.z <= Env_WaterHeightFixed`]. Retail's CameraOffset.z is always
// live (the body updater writes it every tick); our eye offset is derived by
// the infantry body legs, so a body that never derived one (0) has no head
// height to test and keeps the wheel — a seated eye sits above the deck, never
// at it.
bool watercraft_driver_submerged(const World &world, const Entity &occ) {
    if (world.env.water_z == 0 || occ.eye_offset_z == 0) return false;
    return to_fixed(occ.position.z) + occ.eye_offset_z <= world.env.water_z;
}

// Stuck escalation uses the live spawn-parent anchor and rejects dead parents.
// [orig: @0x4653BE, @0x4653D4]
// See ai.h. [orig: AI_CheckVehicleStuckState @0x465290]
void AiSystem::check_vehicle_stuck(World &world, Entity &veh) {
    // [orig: `entity[74] != 1` @0x4652a3 — the think cooldown at +0x128]
    if (const AiEntity *ve = for_handle(veh.handle);
        ve != nullptr && ve->inf.wait_cooldown == 1)
        return;
    Entity::VehicleMotorState &m = veh.veh;
    const int32_t n = ++m.stuck_ticks;                 // [orig: @0x4652b6]
    if (!is_authority || (n & 0xF) != 0) return;       // [orig: @0x4652bc/@0x4652c4]
    const int32_t px = to_fixed(veh.position.x);
    const int32_t py = to_fixed(veh.position.y);
    const int32_t pz = to_fixed(veh.position.z);
    const int32_t self_r = to_fixed(veh.bound_radius); // [orig: entity+0]
    if (n > 32) {
        // Anyone alive nearby keeps the hull from being written off: a live,
        // unhidden pool-0 body inside the (both radii + 12 u) box and sphere
        // resets the count [orig: the pool-0 walk @0x4652e6..0x46538c].
        world.registry.for_each_in_pool(0, [&](const Entity &e) {
            if (m.stuck_ticks == 0) return;
            if (!e.has_item_def) return;               // [orig: entity[7] @0x465308]
            if ((e.flags & 3u) != 0) return;           // [orig: (Flags & 3) == 0 @0x465312]
            const int32_t r = to_fixed(e.bound_radius) + self_r + 0xC0000;
            const int32_t dx = px - to_fixed(e.position.x);
            if (iabs32(dx) > r) return;
            const int32_t dy = py - to_fixed(e.position.y);
            if (iabs32(dy) > r) return;
            const int32_t dz = pz - to_fixed(e.position.z);
            if (iabs32(dz) > r) return;
            const double d = std::sqrt(static_cast<double>(dx) * dx +
                                       static_cast<double>(dy) * dy +
                                       static_cast<double>(dz) * dz);
            const int32_t di = d >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(d);
            if (di <= r) m.stuck_ticks = 0;            // [orig: @0x46538c]
        });
    }
    if (m.stuck_ticks <= 3410) return;                 // [orig: @0x4653a6]
	// [orig: AI_CheckVehicleStuckState @0x4653AC..0x46542D]
	int32_t pose[6] = { to_fixed(veh.spawn_position.x), to_fixed(veh.spawn_position.y),
		to_fixed(veh.spawn_position.z), 0, 0, 0 };
	if (m.spawn_pose_valid && !world.vehicles.resolve_spawn_pose(veh, pose))
		return;
	const int32_t sx = io::bam_sub(px, pose[0]);
	const int32_t sy = io::bam_sub(py, pose[1]);
	const int32_t sz = io::bam_sub(pz, pose[2]);
	const double d = std::sqrt(static_cast<double>(sx) * sx + static_cast<double>(sy) * sy +
			static_cast<double>(sz) * sz);
	const int32_t di = d >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(d);
    if (di <= 0xC0000) return;                          // [orig: @0x465470]
    if (m.stuck_ticks <= 3720) {
        m.slide_z += 1024;                              // [orig: @0x465492 slideDecay += 0x400]
    } else {
        // Written off: zero health, no attacker credit [orig: @0x46547e/@0x465485].
        veh.health = 0;
    }
}

// See ai.h. [orig: ground @0x48bc4e..0x48bc94 — `minAI > 1` @0x48bc51,
// Entity_IsBoneInProximity @0x48bc5d, Entity_CountMountedEntities < minAI
// @0x48bc75, `Health > criticalHp -> Health = criticalHp` @0x48bc84..0x48bc8d]
void AiSystem::apply_min_ai_crew_clamp(World &world, Entity &veh,
                                       const VehicleTraits &traits) {
    if (traits.min_ai <= 1) return;
	if (vehicle_at_spawn_anchor(world, veh))
		return;
	if (count_mounted_entities(world, veh) >= traits.min_ai)
		return;
	if (veh.health > traits.critical_hp)
        veh.health = static_cast<int32_t>(static_cast<int16_t>(traits.critical_hp));
}

// See ai.h — the vehicle-physics AI/parked input staging, one staging for the
// ground movers whose legs are instruction-equivalent: cveh [orig:
// Entity_UpdateVehiclePhysics @0x48af00: parked @0x48c002-0x48c02d, AI-driver leg
// @0x48bc12-0x48c034] and ctan [orig: Entity_UpdateTankVehiclePhysics @0x488AB0:
// AI-driver leg @0x4897DB..0x489C00 (avoid brake @0x489958..0x489B33, boarder
// hold @0x489B38..0x489BCC), parked @0x489BCE..0x489BF9].
void AiSystem::vehicle_ai_drive(World &world, Entity &veh, const Entity *controller,
                                const VehicleTraits &traits, VehicleDriveCmd &out) {
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return; // no brain: the motor's own no-controller hold stands in
    AiBrain &b = ve->brain;

    // Parked is `occupant == 0 || Flags & 0x10000002` and nothing else: a hull
    // at zero health whose dead bit is not up yet keeps its driver's leg until
    // the dying state's death transforms land, exactly like the player leg.
    // [orig: ctan @0x48954B..0x489560; cveh @0x48B972..0x48B987]
    if (controller == nullptr || (veh.flags & 0x10000002u) != 0) {
		// Parked/no driver: the motor's no-controller branch holds heading + zeroes the
		// command; the brain drops into the player-mode/parked state and the
		// stuck escalation counts. [orig: @0x48c002-0x48c02d — aiComp[132] = Yaw,
		// [136] = 0, [137] = 0, AI_CheckVehicleStuckState @0x48c01e, Flags &= ~0x80,
		// state = 22]
		// Only the current state is stamped. Pending death/command transitions
		// remain owned by the state callback at entity+0x1C8.
		b.f[AiBrain::kCurState] = 22;
		check_vehicle_stuck(world, veh);
        return; // out.ai_drive stays false
    }

    // An AI controller sits in the ctrl/drvr seat — the autopilot leg.
    if (b.f[AiBrain::kCurState] == 22) { // [orig: @0x48bc16; ctan @0x4897DF]
        b.f[AiBrain::kCurState] = 16;
    }

    const int32_t heading = veh.veh.yaw_seeded
            ? veh.veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));

    // cmd speed = the SM mover's out-speed, capped at the def player_speed
    // [orig: @0x48bc23-0x48bc48; ctan @0x4897EC..0x489811 — aiComp[136] =
    //  min(brain[128], playerSpeed); the aiComp[135] <- brain[127] target mirror is
    //  an unmodeled slot].
    int32_t cmd_speed = b.f[AiBrain::kOutSpeed];
    if (cmd_speed > traits.player_speed) cmd_speed = traits.player_speed;
    // The minAI crew health clamp [orig: @0x48bc4e-0x48bc94; ctan @0x489817..0x48985D].
    apply_min_ai_crew_clamp(world, veh, traits);

    // Per-leg turn budget: recomputed whenever the mover's node advance cleared it.
    // [orig: @0x48bc9a-0x48bcd9; ctan @0x489863..0x4898A2]
    if (b.f[AiBrain::kAnimFlag] == 0 && b.f[AiBrain::kWpType] != 0) {
        ai_waypoint_update_target(b, ve->pos, nav);
        b.f[AiBrain::kAnimFlag] = route_turn_budget(b, heading);
    }

    // Bearing delta clamped to the budget (32-bit wrap semantics are load-bearing near
    // the +-half-turn seam) [orig: @0x48bcdf-0x48bcf9; ctan @0x4898A8..0x4898C2].
    int32_t delta = io::bam_sub(b.f[AiBrain::kWpBearing], heading);
    const int32_t budget = b.f[AiBrain::kAnimFlag];
    if (delta > budget) delta = budget;
    if (delta < -budget) delta = -budget;

    // Sharp legs on a slow-steering vehicle damp the speed 0.75x per ~30/60 deg of
    // residual turn [orig: @0x48bcfb-0x48bd73; ctan @0x4898C4..0x48993C — turnRate2 << 6
    // < budget, thresholds 357913920 / 715827840, factor 49152/65536].
    if ((traits.turn_rate2 << 6) < budget) {
        const int32_t a = std::abs(delta);
        if (a > 357913920)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
        if (a > 715827840)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
    }

    // The pool-1 avoid BRAKE [orig: @0x48bd8f-0x48bf26; ctan @0x489958..0x489B33] —
    // shared with the cbot leg (vehicle_avoid_brake above); the ground walk skips
    // ItemTypeIndex 0 only.
    cmd_speed = vehicle_avoid_brake(world, veh, heading, cmd_speed,
                                    AvoidBrakeWalk::SkipIndexZero);
    out.ai_drive = true;
    out.cmd_speed = cmd_speed;
    // [orig: @0x48bd7f; ctan @0x489948..0x489952]
    out.steer_target_bam = io::bam_add(io::bam_add(heading, delta), delta >> 3);
    // The wait-for-boarders stop: a full stop while any live unmounted pool-0
    // body runs the boarding think toward THIS vehicle [orig: @0x48bf6f-0x48bff9;
    // ctan @0x489B38..0x489BCC — aiComp[132] = Yaw, [136] = 0, [137] = 0,
    // Flags &= ~0x80; the motor zeroes the ramp for every AI command].
    if (vehicle_waits_for_boarders(world, veh)) {
        out.steer_target_bam = heading;
        out.cmd_speed = 0;
        veh.flags &= ~0x80u;
    }
    // The handbrake byte-973 latch @0x48c03a and the crashed stop @0x48c086 sit
    // past the AI leg in the motor itself (tick_vehicle_motor).
}

// See ai.h — the cbot AI-driver/parked staging (witnessed 2026-08-06).
// [orig: Entity_UpdateWatercraftPhysics @0x48D480 — AI leg @0x48E247..0x48E756,
// parked leg @0x48E7EE..0x48E81E]
void AiSystem::watercraft_ai_drive(World &world, Entity &veh,
                                   const Entity *controller,
                                   const VehicleTraits &traits,
                                   VehicleDriveCmd &out) {
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return; // no brain: the motor's no-controller hold stands in
    AiBrain &b = ve->brain;

    // The witnessed boat split is occupant-NULL or the dead flag ONLY — no
    // health term (a 0-hp capsize-drain hull that never took the kill edge
    // keeps driving in retail; the ground family's health check is its own
    // witness and stays in vehicle_ai_drive). The dead bit lives on
    // engine_flags in our split-field model, so read the combined view.
    if (controller == nullptr ||
        ((veh.flags | veh.engine_flags) & kEntityFlagDead) != 0) {
        // Parked/no driver [orig: @0x48E7EE..0x48E81E — aiComp[132] = Yaw,
        // [136] = 0, [137] = 0, AI_CheckVehicleStuckState @0x48e808,
        // Flags &= ~0x80, state = 22]. The pend mirror is ours — one state field
        // in the original (see vehicle_ai_drive).
        b.f[AiBrain::kCurState] = 22;
        check_vehicle_stuck(world, veh);
        return; // out.ai_drive stays false
    }

    // The 22 -> 16 hand-back at the AI-leg head [orig: @0x48E247..0x48E24D].
    if (b.f[AiBrain::kCurState] == 22) {
        b.f[AiBrain::kCurState] = 16;
    }

    const int32_t heading = veh.veh.yaw_seeded
            ? veh.veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));

    // cmd = the SM mover's out-speed capped at def waterSpeed [orig:
    // @0x48E260..0x48E279 — aiComp[136] = min(brain[128], waterSpeed)]. The
    // aiComp[135] <- brain[127] target mirror (@0x48E254) is an unmodeled slot.
    int32_t cmd_speed = b.f[AiBrain::kOutSpeed];
    if (cmd_speed > traits.water_speed) cmd_speed = traits.water_speed;
    // The minAI crew health clamp [orig: @0x48E27F..0x48E2C7 — the boat twin
    // of the ground block; undercrewed AI hulls bleed to critical].
    apply_min_ai_crew_clamp(world, veh, traits);

    // Per-leg turn budget, boat form: truncate-divide THEN << 4 [orig:
    // @0x48E2D5..0x48E31C — refresh when brain[32] is spent and the waypoint
    // block is live; denom = (brain[35] >> 15) + 32].
    if (b.f[AiBrain::kAnimFlag] == 0 && b.f[AiBrain::kWpType] != 0) {
        ai_waypoint_update_target(b, ve->pos, nav);
        const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
        const int32_t err = iabs32(io::bam_sub(heading, b.f[AiBrain::kWpBearing]));
        b.f[AiBrain::kAnimFlag] = (err / denom) << 4;
    }

    // Bearing delta clamped to the budget [orig: @0x48E322..0x48E33C].
    int32_t delta = io::bam_sub(b.f[AiBrain::kWpBearing], heading);
    const int32_t budget = b.f[AiBrain::kAnimFlag];
    if (delta > budget) delta = budget;
    if (delta < -budget) delta = -budget;

    // Sharp legs damp 0.75x per 15/30/45 deg of residual turn when
    // turn_rate2<<6 < budget [orig: @0x48E33E..0x48E3EA — thresholds
    // 0x0AAAAAA0 / 0x15555540 / 0x1FFFFFE0, factor 0xC000, round-half-up].
    if ((traits.turn_rate2 << 6) < budget) {
        const int32_t a = std::abs(delta);
        if (a > 0x0AAAAAA0)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
        if (a > 0x15555540)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
        if (a > 0x1FFFFFE0)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
    }

    // steer = heading + delta — the boat leg has NO delta/8 term
    // [orig: @0x48E3F0..0x48E3F5].
    int32_t steer = io::bam_add(heading, delta);

    // Slip counter-steer: steer INTO the hull/velocity mismatch and shed command
    // as the slip grows [orig: @0x48E3FB..0x48E577 — corr = (sin22(Yaw - motion)
    // * min(|v|, 1.0)) >> 22 (no rounding bias), steer += corr << 14; |corr|
    // tiers 0x800/0x1000/0x2000/0x3000 -> x0xC000/x0x8000/x0x6000/x0x4000, each
    // round-half-up and compounding].
    {
        const int32_t vx = veh.veh.vel_x;
        const int32_t vy = veh.veh.vel_y;
        const double fdx = static_cast<double>(vx);
        const double fdy = static_cast<double>(vy);
        // The motion bearing truncates through _ftol2_sse like every other
        // bearing; the widened conversion keeps +pi's 0x80000000 word
        // [orig: fpatan, fmul dbl_7C19D8, `call _ftol2_sse` @0x48E417..0x48E423].
        const int32_t motion = static_cast<int32_t>(static_cast<uint32_t>(
                static_cast<int64_t>(std::atan2(fdy, fdx) * 683565275.5764316)));
        const int32_t s22 = avoid_sin22(io::bam_sub(heading, motion));
        const double dm = std::sqrt(fdx * fdx + fdy * fdy);
        int32_t mag = dm >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(dm);
        if (mag > 0x10000) mag = 0x10000; // [orig: the 1.0 u/tick clamp @0x48E476]
        const int32_t corr = static_cast<int32_t>(
                (static_cast<int64_t>(s22) * mag) >> 22);
        steer = static_cast<int32_t>(
                static_cast<uint32_t>(steer) +
                (static_cast<uint32_t>(corr) << 14)); // [orig: shl edx,0Eh; add @0x48E49A]
        const int32_t a = iabs32(corr);
        if (a > 0x800)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
        if (a > 0x1000)
            cmd_speed = static_cast<int32_t>((32768LL * cmd_speed + 0x8000) >> 16);
        if (a > 0x2000)
            cmd_speed = static_cast<int32_t>((24576LL * cmd_speed + 0x8000) >> 16);
        if (a > 0x3000)
            cmd_speed = static_cast<int32_t>((16384LL * cmd_speed + 0x8000) >> 16);
    }

    // The pool-1 avoid BRAKE — the cbot copy of the ground block, walking only
    // ItemTypeIndex 1 [orig: @0x48E577..0x48E756, the gate @0x48E5C5].
    cmd_speed = vehicle_avoid_brake(world, veh, heading, cmd_speed,
                                    AvoidBrakeWalk::OnlyIndexOne);

    out.ai_drive = true;
    out.cmd_speed = cmd_speed;
    out.steer_target_bam = steer;
    // The wait-for-boarders stop: hold at cmd 0 while any live unmounted pool-0
    // AI runs boarding mode 125 toward THIS hull's id [orig: @0x48E75B..0x48E7EC
    // — aiComp[132] = Yaw, [136] = 0, [137] = 0, Flags &= ~0x80].
    if (vehicle_waits_for_boarders(world, veh)) {
        out.steer_target_bam = heading;
        out.cmd_speed = 0;
        veh.flags &= ~0x80u;
    }
}

// The CHel AI flight drive — see the ai.h declaration. Retail computes this
// inside the aircraft physics; the registers this stages are exactly the ones
// the mover's servos consume (cmd_speed/cmd_lateral fwd+lat cyclic,
// steer_target_bam, net_alt_target, net_engine_on).
// [orig: the AI leg of Entity_UpdateAircraftPhysics @0x490310]
// Is anyone still walking over to board this vehicle? Retail asks only while the
// vehicle can still take someone (Entity_CanEnterVehicle), then walks pool 0 for
// a live, unmounted body whose AI is running the BOARD order at this hull.
//
// The board order lives in the walker's AiSlot (entity+0x68), the words the
// boarding command writes: slot[37] (+0x94) is the command (125 = "Goto SSN and
// board"), slot[38] (+0x98) the target's authored id. The brain (entity+0x64)
// plays no part.
// [orig: the shared wait-for-boarders block — air @ kong 94590, watercraft
//  @0x48E75B, cveh @0x48BFB6..0x48BFE2, ctan @0x489B82..0x489BAE: gates
//  `entity[7] != 0`, `(Flags & 3) == 0`, `mov ecx,[eax+68h]` non-null,
//  `cmp [ecx+94h],7Dh`, `[ecx+98h] == entity->DcbId`, and `!entity[90]`
//  (unmounted); the writer WacCmd_SsnToSsn @0x4F73E4..0x4F73F7]
bool AiSystem::vehicle_waits_for_boarders(World &world, const Entity &veh) {
	// The whole admission predicate gates the hold, including the stable
	// saved-pose/deck/spawn arms. [orig: Entity_CanEnterVehicle @0x435480]
	if (!vehicle_can_enter(world, nullptr, veh))
		return false;

	bool waiting = false;
    world.registry.for_each([&](const Entity &e) {
        if (waiting) return;
        if (e.handle.pool() != 0) return;      // [orig: the pool-0 walk]
        if (!e.has_item_def) return;           // [orig: entity[7] != 0]
        // [orig: (entity[36] & 3) == 0 — hidden (bit0) or dead (bit1) are skipped]
        if ((e.flags & 3u) != 0) return;
        if (e.mounted) return;                 // [orig: !entity[90]]
        const AiEntity *b = for_handle(e.handle);
        if (b == nullptr) return;              // [orig: the aiSlot null test]
        if (b->slot.f[37] != 125) return;      // not running the board order
        if (b->slot.f[38] != static_cast<int32_t>(veh.net_id)) return; // not THIS hull
        waiting = true;
    });
    return waiting;
}

void AiSystem::chel_ai_drive(World &world, Entity &veh, const Entity *controller,
                             const VehicleTraits &traits) {
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return;
    AiBrain &b = ve->brain;
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.yaw_seeded) {
        // [orig: Entity_UpdateAircraftPhysics @0x49034C reads the row's Yaw,
        //  unmoved = the spawn form, Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
        m.yaw_bam = spawn_angle_bam(90 - veh.yaw);
        m.yaw_seeded = true;
    }
    m.ai_drive = true;
	// These are the same storage as the brain's work/command words in retail.
	// Carry them across our decision/physics split in both directions.
	m.net_alt_target = b.f[AiBrain::kWorkPosZ];
	m.steer_target_bam = b.f[AiBrain::kWorkHeading];
	m.net_climb = b.f[137];
	// A non-PlayerControl aircraft bypasses the complete occupant/AI driving
	// block, including its uncrewed parking reset. Keep its command registers;
	// the aircraft mover derives absolute hover height from the climb register.
	// [orig: Entity_UpdateAircraftPhysics @0x490ef6..0x490efa -> @0x491da7]
	if (!traits.player_control) {
		m.cmd_lateral_speed = b.f[135];
		m.cmd_speed = b.f[136];
		return;
	}

	const int32_t ground = m.ground_cache != INT32_MIN ? m.ground_cache : ve->pos[2];

	// Parked is `occupant == 0 || Flags & 0x10000002` and nothing else: no hull
	// or pilot health term. A dead pilot has already left the seat through the
	// infantry death edge's detach (the caller hands a dead controller in as
	// null). [orig: Entity_UpdateAircraftPhysics @0x490F16..0x490F25]
	if (controller == nullptr || ((veh.flags | veh.engine_flags) & 0x10000002u) != 0) {
		// Parked: both thrust registers and the climb zeroed, the altitude target
		// pinned 0x4000 under the ground (collective off), the stuck escalation,
		// then PRETTY on the CURRENT state word only; the pending word keeps any
		// order parked on it [orig: @0x491C31..0x491C58, AI_CheckVehicleStuckState
		//  @0x491C5E, `mov dword ptr [ebx+10h],0Eh` @0x491C66, Flags &= ~0x80
		//  @0x491C6D].
		// An uncrewed hull still airborne more than 2 u over its cached ground
		// (the probe offset taken off its Z) first yaws 2886390 BAM per visit, about
		// 15 deg/s, and the heading pin below takes the turned yaw.
		// [orig: `test dword ptr [esi+24h],2000h` @0x491C09, Z - brain+0x2C -
		//  ground > 0x20000 @0x491C18..0x491C28, `add dword ptr [esi+10h],
		//  0FFD3F50Ah` @0x491C2A, brain+0x210 = Yaw @0x491C54]
		if ((veh.flags & kEntityFlagInAir) != 0 &&
		    io::bam_sub(io::bam_sub(ve->pos[2], m.air_probe_z_off), ground) > 0x20000)
			m.yaw_bam = io::bam_sub(m.yaw_bam, 2886390);
		m.cmd_speed = 0;
		m.cmd_lateral_speed = 0;
		m.steer_target_bam = m.yaw_bam;
		m.net_alt_target = ground - 0x4000;
		m.net_climb = 0;
		check_vehicle_stuck(world, veh);
		b.f[AiBrain::kCurState] = 14;
		m.net_engine_on = false;
		return;
	}

	// Crewed: the stuck count rests [orig: the AI-leg head `entity+0x148 = 0`
	// @0x490F30], and PRETTY hands back to FOLLOWWP on the CURRENT word only
	// [orig: `cmp [ebx+10h],0Eh; jnz; mov [ebx+10h],7` @0x49158A..0x491590]. The
	// pending word is left alone, so the machine commits whatever it holds after
	// each think (the class init's 0 until an order lands) and the next mover
	// visit promotes 0 -> 14 -> 7 again: an AI helicopter thinks on every visit,
	// like the ground legs' 22 -> 16 hand-back [orig:
	// EntityAI_ProcessInfantryStateMachine @0x458384..0x4583B0].
	m.stuck_ticks = 0;
	if (b.f[AiBrain::kCurState] == 14)
		b.f[AiBrain::kCurState] = 7;
	m.net_engine_on = true;
	// The minAI crew clamp [orig: @0x4915b2..0x4915e2 — the air twin, gated
    // `itemDef+0x8D8 > 1`, Entity_IsBoneInProximity @0x4915c2,
    // Entity_CountMountedEntities @0x4915d2, criticalHp @0x4915e2].
    apply_min_ai_crew_clamp(world, veh, traits);

    // The command seeds [orig: `[540] = brain[127]; [544] = brain[128]`
    // @0x4915a3..0x4915a9 — [128] is the SM mover's out-speed (the state-16
    // tick's AI_UpdateWaypointMovement / the state-17 fire tick write it);
    // [127] has no live SM writer, so the lateral seed is the zero it holds].
    m.cmd_lateral_speed = b.f[AiBrain::kTargetRef];
    m.cmd_speed = b.f[AiBrain::kOutSpeed];

    // The per-leg turn budget, the AIR form: recomputed when the node advance
    // cleared it and the waypoint block is live [orig: @0x49160d..0x491663 —
    // `!brain[32] && brain[13]` -> AIWaypoint_UpdateTarget @0x491633, then
    // [32] = 8 * (|Yaw - brain[21]| / ((brain[35] >> 15) + 32)) — divide THEN
    // x8, unlike the ground leg's 32*err/denom].
    if (b.f[AiBrain::kAnimFlag] == 0 && b.f[AiBrain::kWpType] != 0) {
        ai_waypoint_update_target(b, ve->pos, nav);
        const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
        const int32_t err = iabs32(io::bam_sub(m.yaw_bam, b.f[AiBrain::kWpBearing]));
        b.f[AiBrain::kAnimFlag] = 8 * (err / denom);
    }

    // NO ROUTE -> the aircraft does not fly. Retail reads its waypoint node and
    // then THROWS IT AWAY unless the brain carries a channel or a node, so the
    // whole flight computation below - including the altitude command - is
    // skipped and the altitude target keeps whatever the parked leg last wrote
    // (ground - 0x4000, collective off): a crewed helicopter with no orders sits
    // on its skids with the engine running and the blades turning. This is
    // 05TRcoop's whole co-op choreography (the WAC hands the group its route).
    // A completed one-shot route (brain[13] = 0) keeps its LAST node: the hull
    // holds station over it.
    // [orig: `v91 = brain[16]; if (!brain[15] && !brain[14]) v91 = nullptr`
    //  @0x491576..0x49159a and the flight block's `if (v91 && brain[4] == 7)`
    //  guard @0x491671]
    const NavEntry *node = nav.entry(b.f[AiBrain::kWpResolved]);
    if (b.f[AiBrain::kWpChannel] == 0 && b.f[AiBrain::kWpNode] == 0) node = nullptr;
	if (node != nullptr && b.f[AiBrain::kCurState] == 7) {
		// ---- The flight block [orig: @0x491672..0x491998].
		const int32_t px = ve->pos[0], py = ve->pos[1], pz = ve->pos[2];
        // The node's Z, floored 0x4000 under the hull's own average ground
        // [orig: @0x491672..0x491684].
        int32_t tz = node->f[3];
        if (tz < ground - 0x4000) tz = ground - 0x4000;
        const int32_t dx = node->f[1] - px;
        const int32_t dy = node->f[2] - py;
        const int32_t dz = tz - pz;
        // Planar distance and bearing to the node (fpatan x 2^32/2pi), the
        // planar speed; zero lengths become 1 [orig: @0x491694..0x4916dd].
        const double fdx = static_cast<double>(dx), fdy = static_cast<double>(dy);
        const double dd = std::sqrt(fdx * fdx + fdy * fdy);
		int32_t dist = dd >= 2147418112.0 ? 2147418112 : static_cast<int32_t>(dd);
		const int32_t bearing = static_cast<int32_t>(
				static_cast<int64_t>(std::atan2(fdy, fdx) * 683565275.5764316));
		const double fvx = static_cast<double>(m.vel_x);
		const double fvy = static_cast<double>(m.vel_y);
        const double sd = std::sqrt(fvx * fvx + fvy * fvy);
		int32_t speed = sd >= 2147418112.0 ? 2147418112 : static_cast<int32_t>(sd);
		if (speed == 0)
			speed = 1;
		if (dist == 0) dist = 1;
        // The climb-per-tick the node's slope asks for at the current speed,
        // folded into the altitude target and the vertical velocity
        // [orig: @0x49175c..0x491796 — v104 = speed * dz / dist (64-bit);
        //  [524] = Z + 4*v104; slideDecay = (v104 + slideDecay) >> 1].
        const int32_t v104 = static_cast<int32_t>(
                static_cast<int64_t>(speed) * dz / dist);
        m.net_alt_target = pz + 4 * v104;
        m.slide_z = (v104 + m.slide_z) >> 1;
        // The climb register against the hull's ground, floored at zero — a
        // target under the ground parks 0x2000 below it
        // [orig: @0x4917a5..0x4917c9].
        int32_t climb = m.net_alt_target - ground - 0x4000;
        if (climb < 0) {
            climb = 0;
            m.net_alt_target = ground - 0x2000;
        }
        m.net_climb = climb;
        // The heading error's trig at 2^22 [orig: @0x4917d4..0x491821].
        const int32_t err = io::bam_sub(bearing, m.yaw_bam);
        const int32_t c22 = flight_cos22(err);
        const int32_t s22 = avoid_sin22(err);
        // Beyond 6 u planar, a zero command seeds the 132-scaled cyclic pair
        // from the heading error [orig: @0x4917f3..0x491834 — only a ZERO
        // register takes the seed].
        if (dist > 0x60000) {
            if (m.cmd_lateral_speed == 0)
                m.cmd_lateral_speed = static_cast<int32_t>((132LL * s22) >> 22);
            if (m.cmd_speed == 0)
                m.cmd_speed = static_cast<int32_t>((132LL * c22) >> 22);
        }
        // The two ground samples: the hull's own and the node's
        // [orig: Entity_CalcAverageGroundHeight @0x491845 (self) / @0x491855
        //  (the node entity)]. The hull's is a FRESH radius-0 sample at its
        // current Z, and it alone sets the floor both legs test; the 8-tick
        // cached ground (sampled with the probe offset taken off Z) feeds only
        // tz, the climb and the landing target [orig: ebp = the self sample
        // @0x491853, `mov eax,[esi]; sar eax,2; add eax,ebp` @0x491874 and
        // @0x4918E2].
        const int32_t self_ground = aircraft_ground_height(world, *ve, 0);
        int32_t node_ground = INT32_MIN;
        if (world.tables.terrain != nullptr) {
            const int32_t npos[3] = {node->f[1], node->f[2], node->f[3]};
            const GroundClearance clearance{};
            node_ground = calc_average_ground_height(*world.tables.terrain, npos, 0, clearance);
        }
        const int32_t node_agl = node_ground != INT32_MIN ? node->f[3] - node_ground : 0;
        const int32_t floor = self_ground + (to_fixed(veh.bound_radius) >> 2);
        if (node_agl > 0x60000 || dist > 0x60000) {
            // En route (or the node hangs in the air): never below the hull's
            // ground + bound/4 — a low target lifts 16 u above that floor and
            // the forward command drops to an eighth [orig: @0x491862..0x4918aa].
            if (m.net_alt_target < floor) {
                m.net_alt_target = floor + 0x100000;
                m.cmd_speed = static_cast<int32_t>(
                        ((static_cast<int64_t>(m.cmd_speed) << 13) + 0x8000) >> 16);
            }
        } else if (pz < floor) {
            // LANDING at a node on the ground: an eighth of the forward command,
            // the hull slides a sixty-fourth of the way onto the node each tick
            // and the target parks 0x2000 under the ground
            // [orig: @0x4918b0..0x4918fc].
            m.cmd_speed = static_cast<int32_t>(
                    ((static_cast<int64_t>(m.cmd_speed) << 13) + 0x8000) >> 16);
            const int32_t nx = px + ((node->f[1] - px) >> 6);
            const int32_t ny = py + ((node->f[2] - py) >> 6);
            ve->pos[0] = nx;
            ve->pos[1] = ny;
            veh.position.x = static_cast<float>(from_fixed(nx));
            veh.position.y = static_cast<float>(from_fixed(ny));
            m.net_alt_target = ground - 0x2000;
        }
        // Steer: the heading error clamped to the per-leg budget
        // [orig: @0x491928..0x49195c — [528] = Yaw + clamp(err, +-[32])].
        int32_t delta = err;
        const int32_t budget = b.f[AiBrain::kAnimFlag];
        if (delta > budget) delta = budget;
        if (delta < -budget) delta = -budget;
        m.steer_target_bam = io::bam_add(m.yaw_bam, delta);
        // The forward command scales by cos^2 of the heading error — a hull
        // still turning onto its leg creeps [orig: @0x491970..0x49198a].
        const int32_t ac = iabs32(c22);
        m.cmd_speed = static_cast<int32_t>((static_cast<int64_t>(m.cmd_speed) * ac) >> 22);
        m.cmd_speed = static_cast<int32_t>((static_cast<int64_t>(m.cmd_speed) * ac) >> 22);
	}

	// The pool-1 separation damp on the forward command [orig: @0x4919fc..0x491b67
	// — the same footprint ellipse, dead-ahead cone and id/frame factor as the
	// ground brake @0x48bd8f, the air walk gating on `entity+0x1C == 1`
	// @0x4919E6].
	m.cmd_speed = vehicle_avoid_brake(world, veh, m.yaw_bam, m.cmd_speed,
	                                  AvoidBrakeWalk::OnlyIndexOne);

    // WAIT FOR BOARDERS. A vehicle whose seats are not yet full HOLDS while any
    // live, unmounted body is still walking over to board it: heading pinned to
    // its own, both command words zeroed, and the powered bit dropped. The
    // ENGINE is untouched, and the rotor is gated on the occupant rather than on
    // power, so the blades keep turning while it waits — a helicopter spools up
    // where it stands instead of leaving the moment its first passenger climbs
    // in. Retail runs this AFTER the flight computation, overriding it, so the
    // override lives at the tail here too.
    // [orig: Entity_ProcessAirVehiclePhysics, the Entity_CanEnterVehicle block
    //  (kong line 94590) — work_heading = entity->Yaw, aiComp[136]/[137] = 0,
    //  Flags &= ~0x80; identical twins in the watercraft (@0x48E75B), light,
    //  infantry and mounted-infantry movers]
    if (vehicle_waits_for_boarders(world, veh)) {
        m.steer_target_bam = m.yaw_bam;
        m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
		m.net_alt_target = io::bam_sub(ground, 0x2000);
		m.net_climb = 0; // [orig: [548] = 0 @0x491c4e]
		m.net_engine_on = false; // the wire's Flags 0x80 [orig: `Flags &= ~0x80u`]
    }
}


} // namespace opennova::world
