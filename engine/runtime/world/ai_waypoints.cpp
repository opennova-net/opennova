#include <runtime/world/ai.h>

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Waypoint targeting and movement (P1: GROUND_FOLLOWWP), plus the vehicle-physics
// AI/parked input staging.

#include <runtime/world/angle.h>
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include <runtime/world/world.h>
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
        // Tracked deviation: the original reads entryIndex[34*navMeshId + node] and
        // Pool_GetEntryUnchecked(3,idx) UNCHECKED (always returns 0, writes fields). Our
        // container rebase adds a bounds/null guard that returns -1 only for indices the
        // original would treat as wild pointers (UB). On valid data (node < count <= 32,
        // populated pool) it never fires, so resolved values stay byte-identical.
        int32_t sub = b.f[AiBrain::kWpNode];
        if (sub < 0 || sub >= 32) return -1;  // entries[] holds 32 nodes max
        int32_t nodeIdx = ch->entries[sub];   // [orig: entryIndex[34*navMeshId + wp[2]]]
        const NavEntry *node = nav.entry(nodeIdx);
        if (!node) return -1;                 // [orig: unchecked Pool_GetEntryUnchecked(3,idx)]
        b.f[AiBrain::kWpResolved] = nodeIdx;  // [orig: waypointData+12 = navEntry]
        int32_t dx = node->f[1] - pos[0];
        int32_t dz = node->f[2] - pos[1];
        int32_t dy = node->f[3] - pos[2];
        wp_dist_bearing(dx, dz, dy, b.f[AiBrain::kWpDistance], b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kWpNodeVal] = node->f[0]; // [orig: *navEntry]
        b.f[AiBrain::kWpExtra] = node->f[4];   // [orig: navEntry[4]]
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
    world.relations.mark_waypoint_visited(ssn, group, list, node);
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

    const NavEntry *node = nav.entry(b.f[AiBrain::kWpResolved]); // aiState[16]
    int32_t nodeX = node ? node->f[1] : 0; // *(waypointPtr+4)
    int32_t nodeY = node ? node->f[2] : 0; // *(waypointPtr+8)
    int32_t result = b.f[AiBrain::kWpBearing];                   // aiState[21]
    b.f[AiBrain::kWorkPosX] = nodeX;
    b.f[AiBrain::kWorkPitch] = 0;   // aiState[133]
    b.f[AiBrain::kWorkRoll] = 0;    // aiState[134]
    b.f[AiBrain::kWorkPosY] = nodeY;
    b.f[AiBrain::kWorkHeading] = result;
    b.f[AiBrain::kOutSpeed] = moveSpeed;
    return result;
}

// The brain half of a waypoint REDIRECT order [orig: Entity_SetWaypointByTeam @0x43cdb4
// per-entity block — aiComp[35]=1 mode, [37]=list, [38]=node (nearest of the list when
// unresolved [orig: Entity_FindNearestTriggerByType @0x407ea0]), think cooldown 0,
// carrier ref cleared, then the brain wp slots + the per-leg turn budget seed].
void AiSystem::apply_route_order(AiEntity &e, int32_t list, int32_t node) {
    AiBrain &b = e.brain;
    const NavChannel *ch = nav.channel(list);
    if (ch == nullptr || ch->count <= 0) return; // dangling list: no order lands
    if (node < 0) {
        // Nearest node of THIS list [orig: @0x407ea0 — min 2D distance].
        int best = 0;
        int64_t best_d2 = INT64_MAX;
        for (int i = 0; i < ch->count && i < 32; ++i) {
            const NavEntry *ne = nav.entry(ch->entries[i]);
            if (ne == nullptr) continue;
            const int64_t dx = static_cast<int64_t>(ne->f[1]) - e.pos[0];
            const int64_t dy = static_cast<int64_t>(ne->f[2]) - e.pos[1];
            const int64_t d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = i; }
        }
        node = best;
    }
    b.f[AiBrain::kWpType] = 1;                                     // [orig: aiComp[35] = 1]
    b.f[AiBrain::kWpChannel] = list;                               // [orig: aiComp[37]]
    b.f[AiBrain::kWpNode] = std::min<int32_t>(node, ch->count - 1); // [orig: aiComp[38]]
    // The SLOT half of the same witnessed block — the INFANTRY think navigates
    // from slot+140/+148/+152, not the brain registers, and a spawn command
    // (waypoint_id 123..127) parked in slot[37] otherwise short-circuits the
    // think forever (the 00TRg debarked-crew freeze: detached, brain routed,
    // slot still 125 -> the reserved-command early-return every think).
    // [orig: Entity_SetWaypointByTeam @0x43cdb4 per-entity block — aiComp+140=1,
    // +148=list, +152=node, think cooldown 0, carrier ref cleared]
    e.slot.f[35] = 1;
    e.slot.f[37] = list;
    e.slot.f[38] = b.f[AiBrain::kWpNode];
    e.slot.f[36] = 0;          // carrier ref cleared [orig: aiComp+144 = 0]
    e.inf.wait_cooldown = 0;   // think cooldown 0 [orig: entity[74] = 0]
    // The turn-budget seed [orig: the tail block @0x43cdb4 — AIWaypoint_UpdateTarget +
    // budget = 32*|Yaw - bearing| / ((speed_param >> 15) + 32)].
    if (ai_waypoint_update_target(b, e.pos, nav) == 0) {
        const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
        // 32-bit wrapping sub like the delta clamp — the short-way angle near the
        // ±half-turn seam [orig: a plain x86 sub, then cdq/xor/sub abs].
        const int32_t err = iabs32(e.heading - b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kAnimFlag] = static_cast<int32_t>(32LL * err / denom);
    }
}

// The 1024-entry engine cos table at 2^22, computed form (the D-INF-4
// equivalence) [orig: off_849934, idx = (bam + 0x200000) >> 22].
static int32_t avoid_cos22(int32_t bam) {
    const double a = static_cast<double>(bam) * io::kRadiansPerBam;
    return static_cast<int32_t>(std::cos(a) * io::kQ22One);
}

// x87 sin at the same 2^22 scale — the cbot slip block multiplies fsin by the
// 4194304.0 constant [orig: dbl_7C3600, consumed @0x48E43D].
static int32_t avoid_sin22(int32_t bam) {
    const double a = static_cast<double>(bam) * io::kRadiansPerBam;
    return static_cast<int32_t>(std::sin(a) * io::kQ22One);
}

// The pool-1 avoid BRAKE, shared by the ground and cbot AI-driver legs — the
// two sites are instruction-identical (footprint ellipse, dead-ahead cone,
// id/frame-keyed factor) [orig: ground @0x48bd8f-0x48bf26; cbot
// @0x48E577..0x48E756]: for every pool-1 neighbor whose heading-aware footprint
// ellipse overlaps ours (+1.0 u) AND that sits within ~30 deg of dead ahead,
// the command speed multiplies by an id/frame-keyed factor in [0.25, 0.75) per
// tick — vehicles brake behind obstacles; deflecting off them through the hull
// contact was never the retail path-follow behavior.
static int32_t vehicle_avoid_brake(World &world, Entity &veh, int32_t heading,
                                   int32_t cmd_speed) {
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
        // retail reads entity+0x1C and skips the slot when it is zero.
        // entity+0x1C is ItemTypeIndex, stamped at spawn as `defIndex`
        // [orig: the gate @0x48bdd9 `*(_DWORD *)(base + 28) == 0`;
        //  ItemTypeIndex written at Entity_SpawnFromBMSRecord @0x40E9F0].
        // (The occupancy half of retail's walk is our null check above:
        //  EntityRegistry::get returns nullptr for an unused slot.)
        if (o->item_id == 0) continue;
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
        const int32_t ang = static_cast<int32_t>(
                std::llround(std::atan2(fdy, fdx) * 683565275.5764316));
        // Directional footprints: r/2 + (r/2)*|cos(yaw - ang)| — an end-on
        // vehicle projects its full bound along the axis, side-on half
        // [orig: the 1024-entry cos table off_849934 @0x48be8f-0x48bef1].
        const int32_t oyaw = o->veh.yaw_seeded
                ? o->veh.yaw_bam
                : bam_heading_from_mission_yaw_deg(static_cast<double>(o->yaw));
        const int32_t other_r =
                static_cast<int32_t>((static_cast<int64_t>(ob >> 1) *
                                      iabs32(avoid_cos22(oyaw - ang))) >> 22) +
                (ob >> 1);
        const int32_t self_r =
                static_cast<int32_t>((static_cast<int64_t>(self_bound >> 1) *
                                      iabs32(avoid_cos22(heading - ang))) >> 22) +
                (self_bound >> 1);
        if (dist > self_r + other_r + 0x10000) continue; // [orig: @0x48bef8]
        // Dead-ahead gate: the other within ~30 deg of the nose
        // [orig: |Yaw - ang - 0x7FFFFF80| <= 357913920 @0x48bf05-0x48bf0f].
        if (iabs32(heading - ang - 0x7FFFFF80) > 357913920) continue;
        // The brake factor ((id + (frame << 8)) & 0x7FFF) + 0x4000 — keyed
        // off DcbId + the global frame counter dword_24C1948 (our net id +
        // logic tick stand in) [orig: @0x48bf17-0x48bf26].
        const int32_t f = static_cast<int32_t>(
                ((static_cast<uint32_t>(veh.net_id) +
                  (static_cast<uint32_t>(world.logic_tick) << 8)) &
                 0x7FFFu) +
                0x4000u);
        cmd_speed = static_cast<int32_t>(
                (static_cast<int64_t>(f) * cmd_speed + 0x8000) >> 16);
    }
    return cmd_speed;
}

// See ai.h — the vehicle-physics AI/parked input staging. [orig: Entity_UpdateVehiclePhysics
// @0x48af00: parked @0x48c002-0x48c02d, AI-driver leg @0x48bc12-0x48c034]
void AiSystem::vehicle_ai_drive(World &world, Entity &veh, const Entity *controller,
                                const VehicleTraits &traits, VehicleDriveCmd &out) {
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return; // no brain: the motor's own no-controller hold stands in
    AiBrain &b = ve->brain;

    const bool wrecked = veh.health <= 0 || !veh.alive;
    if (controller == nullptr || wrecked || (veh.flags & kEntityFlagDead) != 0) {
        // Parked/no driver: the motor's no-controller branch holds heading + zeroes the
        // command; the brain drops into the player-mode/parked state. The stuck-state
        // check is unported (D-NET-161). [orig: @0x48c002-0x48c02d — aiComp[132] = Yaw,
        // [136] = 0, [137] = 0, AI_CheckVehicleStuckState, Flags &= ~0x80, state = 22]
        // The pend mirror is ours: the original has ONE state field; without it the
        // SM's transition pass reverts the stamp to the pending 16 next tick.
        b.f[AiBrain::kCurState] = 22;
        b.f[AiBrain::kPendState] = 22;
        return; // out.ai_drive stays false
    }

    // An AI controller sits in the ctrl/drvr seat — the autopilot leg.
    if (b.f[AiBrain::kCurState] == 22) { // [orig: @0x48bc16]
        b.f[AiBrain::kCurState] = 16;
        b.f[AiBrain::kPendState] = 16;
    }

    const int32_t heading = veh.veh.yaw_seeded
            ? veh.veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));

    // cmd speed = the SM mover's out-speed, capped at the def player_speed
    // [orig: @0x48bc23-0x48bc48 — aiComp[136] = min(brain[128], playerSpeed);
    //  the aiComp[135] <- brain[127] target mirror is an unmodeled slot].
    int32_t cmd_speed = b.f[AiBrain::kOutSpeed];
    if (cmd_speed > traits.player_speed) cmd_speed = traits.player_speed;
    // The minAI crew health clamp [orig: @0x48bc4e-0x48bc94] rides D-NET-161 (def
    // minai/criticalHp unparsed).

    // Per-leg turn budget: recomputed whenever the mover's node advance cleared it.
    // [orig: @0x48bc9a-0x48bccf — budget = 32 * |Yaw - bearing| / ((storedKeyTime >> 15) + 32)]
    if (b.f[AiBrain::kAnimFlag] == 0 && b.f[AiBrain::kWpType] != 0) {
        ai_waypoint_update_target(b, ve->pos, nav);
        const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
        // 32-bit wrapping sub like the delta clamp below — the short-way angle near
        // the ±half-turn seam [orig: a plain x86 sub, then cdq/xor/sub abs].
        const int32_t err = iabs32(heading - b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kAnimFlag] = static_cast<int32_t>(32LL * err / denom);
    }

    // Bearing delta clamped to the budget (32-bit wrap semantics are load-bearing near
    // the +-half-turn seam) [orig: @0x48bcdf-0x48bcf9].
    int32_t delta = b.f[AiBrain::kWpBearing] - heading;
    const int32_t budget = b.f[AiBrain::kAnimFlag];
    if (delta > budget) delta = budget;
    if (delta < -budget) delta = -budget;

    // Sharp legs on a slow-steering vehicle damp the speed 0.75x per ~30/60 deg of
    // residual turn [orig: @0x48bcfb-0x48bd51 — turnRate2 << 6 < budget, thresholds
    // 357913920 / 715827840, factor 49152/65536].
    if ((traits.turn_rate2 << 6) < budget) {
        const int32_t a = std::abs(delta);
        if (a > 357913920)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
        if (a > 715827840)
            cmd_speed = static_cast<int32_t>((49152LL * cmd_speed + 0x8000) >> 16);
    }

    // The pool-1 avoid BRAKE [orig: @0x48bd8f-0x48bf26] — shared with the cbot
    // leg (vehicle_avoid_brake above).
    cmd_speed = vehicle_avoid_brake(world, veh, heading, cmd_speed);
    out.ai_drive = true;
    out.cmd_speed = cmd_speed;
    out.steer_target_bam = heading + delta + (delta >> 3); // [orig: @0x48bd7f]
    // The wait-for-boarders stop @0x48bf6f-0x48bff9 (a full stop while any live
    // unmounted pool-0 entity runs the boarding think toward THIS vehicle —
    // brain mode 125 + the vehicle id; moot until the AI boarding think lands),
    // the handbrake byte-973 latch @0x48c03a and the aim-lock stop @0x48c086
    // stay tracked deferrals (D-NET-161).
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
        // [136] = 0, [137] = 0, AI_CheckVehicleStuckState (deferred, D-NET-161),
        // Flags &= ~0x80, state = 22]. The pend mirror is ours — one state field
        // in the original (see vehicle_ai_drive).
        b.f[AiBrain::kCurState] = 22;
        b.f[AiBrain::kPendState] = 22;
        return; // out.ai_drive stays false
    }

    // The 22 -> 16 hand-back at the AI-leg head [orig: @0x48E247..0x48E24D].
    if (b.f[AiBrain::kCurState] == 22) {
        b.f[AiBrain::kCurState] = 16;
        b.f[AiBrain::kPendState] = 16;
    }

    const int32_t heading = veh.veh.yaw_seeded
            ? veh.veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));

    // cmd = the SM mover's out-speed capped at def waterSpeed [orig:
    // @0x48E260..0x48E279 — aiComp[136] = min(brain[128], waterSpeed)]. The
    // aiComp[135] <- brain[127] target mirror (@0x48E254) is an unmodeled slot;
    // the minAI crew health clamp @0x48E27F..0x48E2C7 (def minai + criticalHp:
    // undercrewed AI hulls bleed to critical) rides D-NET-161.
    int32_t cmd_speed = b.f[AiBrain::kOutSpeed];
    if (cmd_speed > traits.water_speed) cmd_speed = traits.water_speed;

    // Per-leg turn budget, boat form: truncate-divide THEN << 4 [orig:
    // @0x48E2D5..0x48E31C — refresh when brain[32] is spent and the waypoint
    // block is live; denom = (brain[35] >> 15) + 32].
    if (b.f[AiBrain::kAnimFlag] == 0 && b.f[AiBrain::kWpType] != 0) {
        ai_waypoint_update_target(b, ve->pos, nav);
        const int32_t denom = (b.f[AiBrain::kStoredKeyTime] >> 15) + 32;
        const int32_t err = iabs32(heading - b.f[AiBrain::kWpBearing]);
        b.f[AiBrain::kAnimFlag] = (err / denom) << 4;
    }

    // Bearing delta clamped to the budget [orig: @0x48E322..0x48E33C].
    int32_t delta = b.f[AiBrain::kWpBearing] - heading;
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
    int32_t steer = heading + delta;

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
        const int32_t motion = static_cast<int32_t>(
                std::llround(std::atan2(fdy, fdx) * 683565275.5764316));
        const int32_t s22 = avoid_sin22(heading - motion);
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

    // The pool-1 avoid BRAKE — the cbot copy of the ground block
    // [orig: @0x48E577..0x48E756].
    cmd_speed = vehicle_avoid_brake(world, veh, heading, cmd_speed);

    out.ai_drive = true;
    out.cmd_speed = cmd_speed;
    out.steer_target_bam = steer;
    // The wait-for-boarders stop @0x48E75B..0x48E7EC (hold at cmd 0 while any
    // live unmounted pool-0 AI runs boarding mode 125 toward THIS hull's id —
    // moot until the boarding think lands) stays a tracked deferral (D-NET-161).
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
// The three brain slots are the ones the board think already writes: f[37] is
// the command (125 = "Goto SSN and board"), f[38] the target's authored id.
// [orig: the shared wait-for-boarders block — air @ kong 94590, watercraft
//  @0x48E75B; gates `entity[7] != 0`, `(Flags & 3) == 0`, aiComp non-null,
//  aiComp[37] == 125, aiComp[38] == entity->DcbId, and `!entity[90]` (unmounted)]
bool AiSystem::vehicle_waits_for_boarders(World &world, const Entity &veh) {
    // Seats full -> nobody can still be coming, so nothing holds it.
    // [orig: the enclosing `if (Entity_CanEnterVehicle(nullptr, entity))`]
    bool has_free_seat = false;
    for (const Seat &s : veh.seats) {
        if (!s.occupant.valid()) { has_free_seat = true; break; }
    }
    if (!has_free_seat) return false;

    bool waiting = false;
    world.registry.for_each([&](const Entity &e) {
        if (waiting) return;
        if (e.handle.pool() != 0) return;      // [orig: the pool-0 walk]
        if (!e.has_item_def) return;           // [orig: entity[7] != 0]
        // [orig: (entity[36] & 3) == 0 — hidden (bit0) or dead (bit1) are skipped]
        if ((e.flags & 3u) != 0) return;
        if (e.mounted) return;                 // [orig: !entity[90]]
        const AiEntity *b = for_handle(e.handle);
        if (b == nullptr) return;              // [orig: the aiComp null test]
        if (b->brain.f[37] != 125) return;     // not running the board order
        if (b->brain.f[38] != static_cast<int32_t>(veh.net_id)) return; // not THIS hull
        waiting = true;
    });
    return waiting;
}

void AiSystem::chel_ai_drive(World &world, Entity &veh, const Entity *controller,
                             const VehicleTraits &traits) {
    (void)traits;
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return;
    AiBrain &b = ve->brain;
    Entity::VehicleMotorState &m = veh.veh;
    if (!m.yaw_seeded) {
        m.yaw_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(veh.yaw));
        m.yaw_seeded = true;
    }
    m.ai_drive = true;
    const int32_t ground =
            m.ground_cache != INT32_MIN ? m.ground_cache : ve->pos[2];

    const bool wrecked = veh.health <= 0 || !veh.alive ||
                         (veh.flags & kEntityFlagDead) != 0;
    const bool crewed =
            controller != nullptr && controller->alive && controller->health > 0;
    if (!crewed || wrecked) {
        // Parked. [orig: the state-14 block — thrust slots zeroed, altitude
        // pinned below ground (collective off), engine flag cleared; our
        // shared parked stamp is 22 like the ground movers' player/parked leg]
        b.f[AiBrain::kCurState] = 22;
        b.f[AiBrain::kPendState] = 22;
        m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
        m.steer_target_bam = m.yaw_bam;
        m.net_alt_target = ground - 0x4000;
        m.net_engine_on = false;
        return;
    }

    // Crewed: parked -> FOLLOWWP. [orig: `if (brain[16] == 14) brain[16] = 7`]
    if (b.f[AiBrain::kCurState] == 22) {
        b.f[AiBrain::kCurState] = 16;
        b.f[AiBrain::kPendState] = 16;
    }
    m.net_engine_on = true;

    // The patrol height stand-in until the HELO .aip profile rows are plumbed
    // to vehicle brains (stage-1 parse landed; patrol_altitude authored ~40u).
    constexpr int32_t kPatrolAglStandIn = 40 << 16;

    // NO ROUTE -> the aircraft does not fly. Retail reads its waypoint target
    // and then THROWS IT AWAY unless the brain carries a channel or a node, so
    // the whole flight computation below - including the altitude command - is
    // skipped. The altitude target therefore keeps whatever the parked leg last
    // wrote (ground - 0x4000, collective off), which is why a crewed helicopter
    // with no orders sits on its skids with the engine running and the blades
    // turning instead of lifting off.
    //
    // This is 05TRcoop's whole co-op choreography: its WAC watches what the
    // player is riding (`if area(24) and eq(v2,1) and not meride(423) then
    // set(v2,2)`) and the BMS misvar triggers hand the group its route, so the
    // helicopters wait on the ground until the script sends them.
    //
    // We previously gated on kWpType and, worse, commanded a 40 u AGL patrol
    // hover here - so every routeless helicopter climbed and hovered the moment
    // anyone sat in it.
    // [orig: Entity_UpdateAircraftPhysics @0x490310, kong 120676 —
    //  `v91 = brain[16]; if (!brain[15] && !brain[14]) v91 = nullptr;` and the
    //  flight block's `if (v91 && brain[4] == 7)` guard; the altitude store
    //  brain[131] lives INSIDE that guard @ kong 120816]
    if (b.f[AiBrain::kWpChannel] == 0 && b.f[AiBrain::kWpNode] == 0) {
        m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
        m.steer_target_bam = m.yaw_bam;
        // net_alt_target deliberately untouched — retail does not write it here.
        return;
    }

    // Waypoint target through the shared SM mover: refreshes bearing/distance,
    // marks arrivals, advances nodes, honors one-shot ends.
    // [orig: AIWaypoint_UpdateTarget from inside the physics @0x490310, with
    //  the same turn-budget seed (f[35]>>15)+32 the ground mover uses]
    update_waypoint_movement(*ve, world);
    if (b.f[AiBrain::kWpType] == 0) { // the route just completed (one-shot end)
        m.cmd_speed = 0;
        m.cmd_lateral_speed = 0;
        m.steer_target_bam = m.yaw_bam;
        m.net_alt_target = ground + kPatrolAglStandIn;
        return;
    }
    const int32_t bearing = b.f[AiBrain::kWpBearing];
    m.steer_target_bam = bearing;

    // Cyclic pair from the heading error [orig: (132 * sin/cos) >> 22 over the
    // Q22 trig of the target bearing; forward dominates as the nose lines up].
    const double rad = static_cast<double>(io::bam_sub(bearing, m.yaw_bam)) *
                       io::kRadiansPerBam;
    const int32_t cos_q22 = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
    const int32_t sin_q22 = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
    int32_t fwd = static_cast<int32_t>((132LL * cos_q22) >> 22);
    int32_t lat = static_cast<int32_t>((132LL * sin_q22) >> 22);
    if (fwd < 0) fwd = 0; // behind the nose: turn in place, no reverse thrust
    // Near-ground damp [orig: the <<13 >>16 (x1/8) fold under 6.0u AGL].
    if (ve->pos[2] - ground < 0x60000) {
        fwd >>= 3;
        lat >>= 3;
    }
    m.cmd_speed = fwd;
    m.cmd_lateral_speed = lat;

    // Target altitude: patrol height AGL. Retail flies the node's authored Z
    // only when the .aip profile's use-waypoint-z key says so [orig: the
    // slope-based target + the AGL floor avgGround + bound/4]; until the HELO
    // profile rows are plumbed to vehicle brains, hold the AGL stand-in —
    // feeding node Z unconditionally sends the hull to authored-garbage
    // altitudes on routes that never meant to fly it.
    m.net_alt_target = ground + kPatrolAglStandIn;

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
        m.net_engine_on = false; // the wire's Flags 0x80 [orig: `Flags &= ~0x80u`]
    }
}


} // namespace opennova::world
