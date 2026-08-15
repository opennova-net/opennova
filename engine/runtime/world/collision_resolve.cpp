#include "world/collision.h"

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Contact resolution — the vehicle hull and entity resolvers — plus the debug views
// the F3 overlay reads.

#include "world/angle.h"
#include "world/dir_table.h"
#include <cmath>
#include <cstdio>  // temp ladder trace
#include <cstdlib> // temp ladder trace

#include <io/bam.h>

#include "collision_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared fixed-point helpers, unqualified as before

// See collision.h — the vehicle hull contact. [orig: Entity_CheckCollisionState
// @ 0x462a30, the entity-collision half; the per-wheel terrain half rides the
// motor's terrain column (D-NET-161).]
int32_t CollisionWorld::resolve_vehicle_hull(World &world, EntityHandle source,
                                             const int32_t pos[3], const int32_t prev_pos[3],
                                             int32_t out_force[2]) {
    out_force[0] = 0;
    out_force[1] = 0;
    auto it = candidates_.find(source.packed);
    if (it == candidates_.end()) return 0;
    const CandidateSlice slice = it->second;
    if (slice.count <= 0) return 0;

    // The hull-center test point: +1.5 u lift (mid-hull, so a wall's bottom face
    // is never the cheapest SAT exit), radius 1.5 u — the wheel-point array and
    // per-wheel radii ride the unported wheel solver (D-NET-161).
    CollisionPoint point{pos[0], pos[1], pos[2] + 0x18000, 0};
    int32_t radius = 0x18000;

    ContactQuery q;
    q.points = &point;
    q.radii = &radius;
    q.num_points = 1;
    q.prev_pos[0] = prev_pos[0];
    q.prev_pos[1] = prev_pos[1];
    q.prev_pos[2] = prev_pos[2] + 0x18000;
    q.source_bound_radius = radius;
    // The vehicle contact mask is 8: use the VC/type-7 run when present, otherwise
    // fall back to ordinary CB/default solids. It is 24 when the def
    // attrib2 low byte has bit 7 set, adding type-12 volumes [orig: @ 0x462a91-
    // 0x462a9f — collisionMask = 8; attrib2 sign byte -> 24]. attrib2 is not
    // fed to the sim yet, so the 24 leg is a tracked residual (D-NET-161).
    q.mask = 8;
    q.query_is_player = false;

    BlinkAccum blink;       // vehicles accumulate no blink state
    LadderContact ladder;   // nor ladder contact frames
    int32_t severity = 0;
    CollisionTargetView view;
    std::vector<CollisionMatrix> mats;

    for (int32_t i = 0; i < slice.count; ++i) {
        const EntityHandle ch = arena_[slice.start + i];
        if (ch == source) continue;
        // Skip candidates whose groundEntity CHAIN rides this hull — the
        // mounted/carried children (an emplaced cannon whose 0x0D target seeds
        // groundEntity = this vehicle), up to three hops. The pre-fix port
        // inverted the relation (it skipped MY carrier instead), so a hull
        // ground against its own mounted cannon's collision volume every tick
        // and was shoved off its wire pose — the live joiner "vehicle jumping
        // around" (13 u false equilibrium, re-snapping every subrate record).
        // [orig: Entity_CheckCollisionState @0x462a30 proximity walk —
        //  v33 = candidate->groundEntity @0x462e26; skip v33 == ent @0x462e37,
        //  v33->groundEntity == ent or v33->groundEntity->groundEntity == ent
        //  @0x462e3d..0x462e4f]
        {
            const Entity *cand = world.registry.get(ch);
            if (cand != nullptr && cand->ground_target.valid()) {
                if (cand->ground_target == source) continue;
                const Entity *g1 = world.registry.get(cand->ground_target);
                if (g1 != nullptr && g1->ground_target.valid()) {
                    if (g1->ground_target == source) continue;
                    const Entity *g2 = world.registry.get(g1->ground_target);
                    if (g2 != nullptr && g2->ground_target == source) continue;
                }
            }
        }
        int32_t bound_pos[3];
        int32_t bound_radius = 0;
        if (!target_bound(world, ch, bound_pos, bound_radius)) continue;
        if (!contact_query_overlaps_bound(bound_pos, bound_radius, q)) continue;
        const CollisionTargetView *tv = target_view(world, ch, view, mats);
        if (tv == nullptr) continue;
        ContactResult res;
        if (!collision_contact_force(*tv, q, blink, ladder, res)) continue;
        // Verticality split [orig: @ 0x462fc2-0x462fcb — |fz|<<22 / |force| vs the
        // caller's slope thresholds]: a wall-like (horizontal-dominant) push lands
        // in FULL at severity 3 [orig: @ 0x46322d-0x463240]; vertical-dominant
        // force is the ground's — dropped here, the terrain column owns it (the
        // graded ¼/⅛ bands ride the wheel solver, D-NET-161).
        if (abs32(res.force[0]) + abs32(res.force[1]) < abs32(res.force[2])) continue;
        out_force[0] -= res.force[0];
        out_force[1] -= res.force[1];
        severity = 3;
    }
    return severity;
}

int32_t CollisionWorld::resolve_entity(World &world, EntityHandle source, ResolveState &state,
                                       int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                                       int32_t capsule_bottom, int32_t capsule_top,
                                       int32_t heading, int32_t body_pitch, bool is_player,
                                       bool is_authority, uint32_t tick, int32_t anim_state_id,
                                       uint32_t anim_state_flags, int16_t &health,
                                       EntityHandle *out_ground,
                                       const LadderResolveIO *ladder_io) {
    // [orig: movement collision resolver @ 0x4b2bd0]
    // heading/body_pitch feed the on-ladder 2-point capsule's body-axis sincos
    // chain — which retail multiplies by a constant-zero length (see the capsule
    // build below), so the values are witnessed-dead here.
    (void)heading;
    (void)body_pitch;
    Entity *ent = world.registry.get(source);

    if (!state.prev_valid) {
        state.prev_pos[0] = pos[0];
        state.prev_pos[1] = pos[1];
        state.prev_pos[2] = pos[2];
        state.prev_valid = true;
    }

    // Idle skip-throttle. [orig: @ 0x4b2c3d-0x4b2cba — full update when the anim
    // state's table bit 0 is set, moving, sliding, displaced > 200, swimming
    // (Flags 0x2000), or every 64th tick; otherwise counter 0..10 full, 11..20
    // skip (revert the caller's gravity integration + zero vel_z).]
    bool full_update = false;
    if ((anim_state_flags & 1u) != 0) full_update = true; // [orig: @ 0x4b2c1e]
    if (vel_xy[0] != 0 || vel_xy[1] != 0) full_update = true;
    if (vel_z > 0 || vel_z < -420) full_update = true;
    if (abs32(pos[0] - state.prev_pos[0]) > 200 || abs32(pos[1] - state.prev_pos[1]) > 200)
        full_update = true;
    if (ent != nullptr && (ent->flags & kEntityFlagInAir) != 0) full_update = true; // [orig: @ 0x4b2ca6]
    // The replica row's flags mirror serves the same discriminant.
    if (replica_flags_ != nullptr && (*replica_flags_ & kEntityFlagInAir) != 0) full_update = true;
    if ((tick & 0x3Fu) == 0) full_update = true;
    if (!full_update) {
        if (state.skip_counter <= 10) {
            ++state.skip_counter;
        } else {
            if (++state.skip_counter > 20) state.skip_counter = 10;
            // Revert the caller's gravity displacement so a skipped tick nets
            // zero — PER MOTOR, matching each caller's integrate: x1 for the
            // player body (org2 `pos += vel`, -208/tick) and x2 for the NPC
            // (org1 `pos += 2*vel`, -416/tick). [orig: @ 0x4b2cd9-0x4b2ce9 —
            // `test ecx,100h` skips the doubling for Flags&0x100 PLAYER bodies;
            // the resolver's old "0x100=mounted" gloss was a kong misnomer (the
            // kill router @0x4fd160 and the AI target filters key players on
            // 0x100 — world-wac-ai-re.md §16/§20).] While the reimpl player ran
            // the pre-§22 2-tick `pos += 2*vel` cadence this was deliberately
            // x2-for-both; the §22 per-tick -208 + `pos += vel` port restores
            // the witnessed split — a mismatched undo here leaks per gravity
            // tick through the skip band (the standing rise-and-snap sawtooth).
            pos[2] -= is_player ? vel_z : 2 * vel_z;
            vel_z = 0;
            return 0; // [orig: skip path returns 0 @ 0x4b2cec]
        }
    } else {
        state.skip_counter = 0;
    }

    // Per-resolve blink/query state. [orig: the g_Blink* clears @ 0x4b2d54-0x4b2d7d]
    // The +0x2c aux latches and resolver kill/sound/callback side effects remain
    // deferred (D-COL-8); the mounted/carried source gate is modeled below.
    // Mounted/carried sources still compute contacts and dispatch their flags,
    // but suppress every model push force. Retail forms this latch from a live
    // modeled parent, OR source Flags&0x40.
    // [orig: savedPosY @0x4b2be0..0x4b2d3f; force gates
    //  @0x4b3045..0x4b30af/@0x4b3658..0x4b36b9]
    BlinkAccum blink;
    bool suppress_model_force = ent != nullptr &&
            ((ent->flags | ent->engine_flags) & 0x40u) != 0;
    if (ent != nullptr && ent->mounted && health > 0) {
        const Entity *parent = world.registry.get(ent->mount_target);
        suppress_model_force = suppress_model_force ||
                (parent != nullptr &&
                 (parent->has_item_def || has_instance(world, parent->handle)));
    }
    const bool is_local = ent != nullptr && local_player.valid() && source == local_player;
    if (is_local) local_player_blink_flags = 0;
    // The previous-tick CL latch, read BEFORE the per-resolve clear: it selects
    // the on-ladder capsule variant, arms the recontact query mask bit, takes
    // the re-latch fast path, and drives the exit leg when nothing re-latches.
    // [orig: the resolver's v137/onPlatform local, loaded ahead of the clear]
    const bool was_on_ladder =
        (ent != nullptr && ((ent->flags | ent->engine_flags) & kEntityFlagLadderContact) != 0) ||
        (replica_flags_ != nullptr && (*replica_flags_ & kEntityFlagLadderContact) != 0);
    bool on_ladder = was_on_ladder; // v137 — flips true on a fresh entry mid-loop
    if (ent != nullptr) {
        ent->flags &= ~(kEntityFlagIndoors | kEntityFlagLadderContact | kEntityFlagArmoryZone |
                        kEntityFlagVehicleLoadoutZone);
        ent->engine_flags &= ~(kEntityFlagIndoors | kEntityFlagLadderContact |
                               kEntityFlagArmoryZone | kEntityFlagVehicleLoadoutZone);
    }
    if (replica_flags_ != nullptr)
        *replica_flags_ &= ~(kEntityFlagIndoors | kEntityFlagLadderContact | kEntityFlagArmoryZone |
                             kEntityFlagVehicleLoadoutZone);

    // Capsule test points. [orig: the not-on-ladder branch @ 0x4b2edb-0x4b2f2a —
    // 3 points: head (z + collisionRadius - halfRadius + 0.0625), eye (pos +
    // CameraOffset -> our head stand-in), feet; radii {collisionRadius, 0.3125,
    // outerRadius}. CameraOffset is not modeled: the eye point reuses the head
    // column (D-COL-4).]
    const int32_t half_radius = capsule_bottom >> 4;
    int32_t collision_radius =
        (capsule_bottom >> 4) + abs32(capsule_top - capsule_bottom) / 2;
    int32_t min_radius = 57344 - 2 * collision_radius;
    int32_t outer_radius = capsule_bottom >> 1;
    if (min_radius < 4096) min_radius = 4096;
    if (collision_radius < min_radius) {
        collision_radius = min_radius;
        outer_radius -= 4096;
    }
    if (outer_radius < 6144) outer_radius = 6144;
    if (collision_radius < 6144) collision_radius = 6144;

    CollisionPoint points[3];
    int32_t radii[3];
    const int32_t head_lift = collision_radius - half_radius + 4096;
    int32_t num_points;
    if (was_on_ladder) {
        // On-ladder recontact capsule: TWO points — head and feet — radii 25088
        // both. Retail also computes sincos(bodyPitch)/sincos(bodyHeading) here
        // and multiplies them into the second point's offsets, but the length
        // operand is a constant zero in the shipped image, so the chain is
        // arithmetically dead and both points sit on the entity column.
        // [orig: @ 0x4b2e1c-0x4b2ed7 — var_34 = 0 feeds every imul]
        points[0] = {pos[0], pos[1], pos[2] + head_lift, 0};
        points[1] = {pos[0], pos[1], pos[2], 0};
        points[2] = {pos[0], pos[1], pos[2], 0}; // unused slot (debug capture)
        radii[0] = 25088;
        radii[1] = 25088;
        radii[2] = 25088;
        num_points = 2;
    } else {
        points[0] = {pos[0], pos[1], pos[2] + head_lift, 0};
        points[1] = {pos[0], pos[1], pos[2] + head_lift, 0}; // eye stand-in (D-COL-4)
        points[2] = {pos[0], pos[1], pos[2], 0};
        radii[0] = collision_radius;
        radii[1] = 20480;
        radii[2] = outer_radius;
        num_points = 3;
    }

    // Debug capture (local player, full resolves only): the untouched test
    // points — the pass-2 relaxation shifts `points` in place below.
    if (is_local) {
        local_resolve_debug.valid = true;
        for (int32_t i = 0; i < 3; ++i) {
            local_resolve_debug.points[i][0] = points[i].x;
            local_resolve_debug.points[i][1] = points[i].y;
            local_resolve_debug.points[i][2] = points[i].z;
            local_resolve_debug.radii[i] = radii[i];
        }
        local_resolve_debug.capsule_bottom = capsule_bottom;
        local_resolve_debug.capsule_top = capsule_top;
    }

    ContactQuery q;
    q.points = points;
    q.radii = radii;
    q.num_points = num_points;
    q.prev_pos[0] = state.prev_pos[0];
    q.prev_pos[1] = state.prev_pos[1];
    q.prev_pos[2] = state.prev_pos[2];
    q.source_bound_radius = 0x10000; // [orig: entity boundRadius] (D-COL-3)
    // Mask bit 0x1 arms the inflated CL recontact test while the ladder latch
    // rides; retail recomputes the arg per query, so a mid-loop fresh entry
    // upgrades the remaining candidates. [orig: v137 + 2*v130 @ 0x4b2f7c/0x4b35af]
    q.mask = static_cast<uint8_t>((on_ladder ? 1 : 0) | (is_player ? 2 : 0));
    q.query_is_player = is_player;

    int32_t total_force[3] = {0, 0, 0};
    LadderContact ladder;
    EntityHandle ladder_entity;
    second_pass_contact_latch = false; // [orig: collisionFlags = 0 @ 0x4b3585]

    auto it = candidates_.find(source.packed);
    if (it != candidates_.end()) {
        const CandidateSlice slice = it->second;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        for (int pass = 0; pass < 2; ++pass) {
            // [orig: first pass @ 0x4b2f54, second relaxation pass at shifted points
            // adds half the force @ 0x4b3549-0x4b36ec]
            int32_t pass_force[3] = {0, 0, 0};
            bool pass_contact = false;
            for (int32_t i = 0; i < slice.count; ++i) {
                const EntityHandle ch = arena_[slice.start + i];
                int32_t bound_pos[3];
                int32_t bound_radius = 0;
                if (!target_bound(world, ch, bound_pos, bound_radius)) continue;
                if (!contact_query_overlaps_bound(bound_pos, bound_radius, q)) continue;
                const CollisionTargetView *tv = target_view(world, ch, view, mats);
                if (tv == nullptr) continue;
                if (ent != nullptr) {
                    CollisionTargetView &mut = view;
                    mut.is_ground_of_source = (ent->ground_target == ch);
                }
                // Retail recomputes the mask argument at every query, so a fresh
                // entry upgrades the remaining candidates to recontact mode.
                // [orig: v137 + 2*v130 @ 0x4b2f7c / @ 0x4b35af]
                q.mask = static_cast<uint8_t>((on_ladder ? 1 : 0) | (is_player ? 2 : 0));
                ContactResult res;
                const bool contact = collision_contact_force(*tv, q, blink, ladder, res);
                // The force fold PRECEDES the flag dispatch — a fresh CL entry
                // below then zeroes the accumulated force, this candidate's
                // included. Down-force suppression: a mostly-vertical negative
                // force is dropped (standing pressure, not a wall).
                // [orig: fold @ 0x4b3002-0x4b30af / @ 0x4b3603-0x4b36b9 before
                //  the dispatch @ 0x4b30b7; the f[2]<0 gate @ 0x4b3010]
                if (contact && !suppress_model_force) {
                    int32_t f[3] = {res.force[0], res.force[1], res.force[2]};
                    if (f[2] < 0) {
                        if (abs32(f[0]) + abs32(f[1]) < abs32(f[2])) {
                            f[0] = 0;
                            f[1] = 0;
                        }
                        f[2] = 0;
                    }
                    pass_force[0] -= f[0];
                    pass_force[1] -= f[1];
                    if (pass == 0) pass_force[2] -= f[2];
                    pass_contact = true;
                }
                if (pass == 0) {
                    // The contact-flag dispatch runs whether or not the query
                    // produced force — a pure ladder/zone touch still latches.
                    // [orig: the goto LABEL_67 on a zero return @ 0x4b2fa5]
                    if ((res.flags & 0x1u) != 0 && ladder.valid) {
                        if (ladder_io == nullptr) {
                            // Latch-only channel (replica rows / harness callers):
                            // the raw 0x100000 + groundEntity bookkeeping, no
                            // entry gate or chase — a remote row's climb pose is
                            // owned by its authority.
                            ladder_entity = ch;
                        } else {
                            // The CL latch + climb alignment, inline per
                            // contacting candidate. [orig: @ 0x4b3245-0x4b3495]
                            const uint32_t cur_flags =
                                ent != nullptr
                                    ? (ent->flags | ent->engine_flags)
                                    : (replica_flags_ != nullptr ? *replica_flags_ : 0u);
                            // Dead bodies never latch; a fresh entry needs the
                            // player class bit or the AI climb order besides a
                            // previous latch. [orig: @ 0x4b3271 / @ 0x4b325d]
                            if ((cur_flags & kEntityFlagDead) == 0 &&
                                (on_ladder || is_player || ladder_io->ai_wants_climb)) {
                                const int32_t height_diff =
                                    ladder.anchor[2] - ladder_io->tick_start_z; // [orig: @ 0x4b327d]
                                bool latched = false;
                                if (on_ladder) {
                                    latched = true; // re-latch [orig: @ 0x4b3287-0x4b329a]
                                } else {
                                    // Player entry gate: (already above the anchor
                                    // OR facing within 60° of the authored yaw) AND
                                    // the look-pitch sign agrees with the anchor
                                    // side — look up to mount from below, down to
                                    // step on from the top. AI entry has no gate.
                                    // [orig: @ 0x4b32a5-0x4b32c0; 715827840 = 60°]
                                    const int32_t facing_err =
                                        ladder_io->view_yaw != nullptr
                                            ? abs32(io::bam_sub(*ladder_io->view_yaw,
                                                                ladder.yaw))
                                            : 0;
                                    const int32_t view_pitch =
                                        ladder_io->view_pitch != nullptr
                                            ? *ladder_io->view_pitch
                                            : 0;
                                    const bool player_gate =
                                        (height_diff < 0 || facing_err < 715827840) &&
                                        (height_diff > 0) == (view_pitch > 0);
                                    if (!is_player || player_gate) {
                                        latched = true;
                                        // Snap onto the anchor column. Below the
                                        // anchor the stance-picked Z bump breaks
                                        // the grounded state so the climb can
                                        // start; from above, snap just under the
                                        // anchor (over the lip onto the ladder).
                                        // [orig: @ 0x4b32f7-0x4b334b / @ 0x4b3319]
                                        pos[0] = ladder.anchor[0];
                                        pos[1] = ladder.anchor[1];
                                        if (height_diff >= 0) {
                                            if (!ladder_io->prone || suppress_model_force)
                                                pos[2] += ladder_io->crouch ? 39936 : 20480;
                                            else
                                                pos[2] += 60416;
                                        } else {
                                            pos[2] = ladder.anchor[2] - 4096;
                                        }
                                        // Restage the on-ladder capsule pair at the
                                        // anchor and drop this pass's accumulated
                                        // force. [orig: @ 0x4b335a-0x4b3392]
                                        pass_force[0] = 0;
                                        pass_force[1] = 0;
                                        pass_force[2] = 0;
                                        points[0].x = ladder.anchor[0];
                                        points[0].y = ladder.anchor[1];
                                        points[1].x = ladder.anchor[0];
                                        points[1].y = ladder.anchor[1];
                                        radii[0] = 25088;
                                        radii[1] = 25088;
                                        q.num_points = 2;
                                        on_ladder = true; // v137 = 1 [orig: @ 0x4b3300]
                                    }
                                }
                                if (latched) {
                                    ladder_entity = ch;
                                    if (ent != nullptr)
                                        ent->flags |= kEntityFlagLadderContact;
                                    if (replica_flags_ != nullptr)
                                        *replica_flags_ |= kEntityFlagLadderContact;
                                    last_ladder_frame = ladder; // the persisting globals
                                    // The per-tick alignment chase.
                                    // [orig: @ 0x4b33a4-0x4b3495]
                                    if (is_player) {
                                        // Players ease: one sixteenth of the yaw
                                        // error moves the view yaw AND the body
                                        // heading; the local mouse accumulator
                                        // inherits it through the embedder
                                        // write-back. [orig: (delta+8)>>4
                                        // @ 0x4b33bc; dword_B75FCC @ 0x4b33d2]
                                        if (ladder_io->body_heading != nullptr) {
                                            const int32_t step = io::bam_sar(
                                                io::bam_add(
                                                    io::bam_sub(ladder.yaw,
                                                                *ladder_io->body_heading),
                                                    8),
                                                4);
                                            if (ladder_io->view_yaw != nullptr)
                                                *ladder_io->view_yaw = io::bam_add(
                                                    *ladder_io->view_yaw, step);
                                            *ladder_io->body_heading = io::bam_add(
                                                *ladder_io->body_heading, step);
                                        }
                                    } else {
                                        // AI hard-set. [orig: @ 0x4b33da-0x4b33fa]
                                        if (ladder_io->ai_aim_heading != nullptr)
                                            *ladder_io->ai_aim_heading = ladder.yaw;
                                        if (ladder_io->ai_target_heading != nullptr)
                                            *ladder_io->ai_target_heading = ladder.yaw;
                                        if (ladder_io->view_yaw != nullptr)
                                            *ladder_io->view_yaw = ladder.yaw;
                                        if (ladder_io->body_heading != nullptr)
                                            *ladder_io->body_heading = ladder.yaw;
                                    }
                                    if (ladder_io->pitch_restore_active != nullptr)
                                        *ladder_io->pitch_restore_active =
                                            false; // [orig: +0x2C &= ~4 @ 0x4b3405]
                                    if (ladder_io->body_pitch != nullptr)
                                        *ladder_io->body_pitch =
                                            ladder.pitch; // [orig: @ 0x4b3409]
                                    // The facing press (0.0625u along the authored
                                    // yaw) plus the sixty-fourth-step anchor chase.
                                    // [orig: @ 0x4b340f-0x4b3495]
                                    const double rad =
                                        static_cast<double>(ladder.yaw) *
                                        (3.14159265358979323846 / 2147483648.0);
                                    const int32_t c = static_cast<int32_t>(
                                        std::cos(rad) * 4194304.0);
                                    const int32_t s = static_cast<int32_t>(
                                        std::sin(rad) * 4194304.0);
                                    pos[0] += static_cast<int32_t>(
                                        (static_cast<int64_t>(c) << 12) >> 22);
                                    pos[1] += static_cast<int32_t>(
                                        (static_cast<int64_t>(s) << 12) >> 22);
                                    pos[0] += (ladder.anchor[0] - pos[0] + 32) >> 6;
                                    pos[1] += (ladder.anchor[1] - pos[1] + 32) >> 6;
                                }
                            }
                        }
                    }
                    apply_touch_flags(ent, res.flags, health, is_authority);
                }
            }
            if (pass == 0) {
                total_force[0] += pass_force[0];
                total_force[1] += pass_force[1];
                total_force[2] += pass_force[2];
                if (!pass_contact || (total_force[0] == 0 && total_force[1] == 0 &&
                                      total_force[2] == 0))
                    break;
                // Shift the test points by the accumulated force for the second pass.
                for (int32_t pi = 0; pi < num_points; ++pi) {
                    points[pi].x += total_force[0];
                    points[pi].y += total_force[1];
                }
            } else {
                // The pass-2 contact flag persists past the resolve — org1's
                // on-ladder facing press gates on it. [orig: dword_B57C8C
                // stored @ 0x4b3a5c from the flag set @ 0x4b36b1]
                second_pass_contact_latch = pass_contact;
                if (pass_contact && !ladder_entity.valid()) {
                    // [orig: @ 0x4b36da — second-pass half force only without a CL contact]
                    total_force[0] += pass_force[0] >> 1; // [orig: @ 0x4b36e2]
                    total_force[1] += pass_force[1] >> 1;
                }
            }
        }
    }

    // Apply the push-out; a net push resets the idle skip counter. [orig:
    // @ 0x4b3746-0x4b375a; pad_370[3] = 0 @ 0x4b3773]
    if (total_force[0] != 0 || total_force[1] != 0 || total_force[2] != 0)
        state.skip_counter = 0;
    pos[0] += total_force[0];
    pos[1] += total_force[1];
    pos[2] += total_force[2];

    // The CL latch bookkeeping (motor callers already set the flag inline at
    // the latch site; this keeps the replica/harness channel and the transient
    // groundEntity store). [orig: @ 0x4b3291-0x4b3297 — Flags |= 0x100000 +
    // groundEntity = ladder]
    if (ladder_entity.valid() && ent != nullptr) {
        ent->flags |= kEntityFlagLadderContact;
        ent->ground_target = ladder_entity;
    }
    // A replica row latches the same CL-contact bit (the tail probe owns its
    // ground store, as it does for entities — the mid-resolve ladder ground is
    // overwritten there either way).
    if (ladder_entity.valid() && replica_flags_ != nullptr)
        *replica_flags_ |= kEntityFlagLadderContact;

    // Blink apply. [orig: @ 0x4b34c2-0x4b3502 — bit 2 -> Flags 0x800000; local
    // player accumulates the flags word.]
    if (ent != nullptr) {
        ent->blink_hits[0] = blink.hits[0];
        ent->blink_hits[1] = blink.hits[1];
        ent->blink_hits[2] = blink.hits[2];
        ent->blink_hits[3] = blink.hits[3];
        if (is_local) local_player_blink_flags |= blink.flags;
        if ((blink.flags & kBlinkIndoorsBit) != 0) ent->flags |= kEntityFlagIndoors;
    }
    if (replica_flags_ != nullptr && (blink.flags & kBlinkIndoorsBit) != 0)
        *replica_flags_ |= kEntityFlagIndoors;

    // Inter-entity sphere repulsion (no model contact only). [orig: @ 0x4b3a5c-0x4b3c52 —
    // threshold 30% of summed radii, push (thr - dist)/4 along the atan2 direction
    // via the quantized table with the (0x200000 - bam) index.]
    const bool had_model_contact =
        total_force[0] != 0 || total_force[1] != 0 || total_force[2] != 0;
    // [orig: @ 0x4b3a77-0x4b3aa1 — the dragger/carry anim states skip repulsion]
    const bool repulse_exempt_state = anim_state_id == 27 || anim_state_id == 137 ||
                                      anim_state_id == 138 || anim_state_id == 139;
    // [orig: @ 0x4b3aac — Flags 0x43 (dead/hidden/carried) skips repulsion;
    // @ 0x4b3aba — Flags 0x20 widens the radius by 2.0u]
    const bool repulse_exempt_flags =
        (ent != nullptr && (ent->flags & 0x43u) != 0) ||
        (replica_flags_ != nullptr && (*replica_flags_ & 0x43u) != 0);
    if (!had_model_contact && !repulse_exempt_state && !repulse_exempt_flags) {
        int32_t my_radius = 0x10000; // [orig: entity boundRadius] (D-COL-3)
        if ((ent != nullptr && (ent->flags & kEntityFlagParachute) != 0) ||
            (replica_flags_ != nullptr && (*replica_flags_ & kEntityFlagParachute) != 0))
            my_radius += 0x20000;
        for (const PersonSlot &p : persons_) {
            if (p.h == source) continue;
            const int32_t threshold = 30 * (my_radius + p.radius) / 100;
            // Snapshot positions for the coarse reject... [orig: the g_PersonProx*
            // table reads @ 0x4b3b07-0x4b3b74]
            const int32_t ddx = p.x - pos[0];
            const int32_t ddy = p.y - pos[1];
            const int32_t ddz = p.z - pos[2];
            if (abs32(ddx) > threshold || abs32(ddy) > threshold || abs32(ddz) > threshold)
                continue;
            if (vec_len_ftol(ddx, ddy, 0) > threshold) continue;
            // ...then the LIVE entity for the second distance and the push, and
            // the dead/hidden peer skip. [orig: g_PersonProxEntity re-read
            // @ 0x4b3b7a-0x4b3bca, the +36 & 2 skip @ 0x4b3b8d]
            const Entity *peer = world.registry.get(p.h);
            if (peer == nullptr || (peer->flags & 2u) != 0) continue;
            int32_t live[3];
            entity_pos_fixed(*peer, live);
            const int32_t dist = vec_len_ftol(pos[0] - live[0], pos[1] - live[1], 0);
            if (dist > threshold) continue;
            const int32_t amount = (threshold - dist) >> 2;
            const int32_t ang = static_cast<int32_t>(
                std::atan2(static_cast<double>(pos[1] - live[1]),
                           static_cast<double>(pos[0] - live[0])) *
                kBamPerRadian);
            const uint32_t idx = (0x200000u - static_cast<uint32_t>(ang)) >> 22;
            const DirTable &t = dir_table();
            const int32_t sn = t.sin22[idx & 1023];
            const int32_t cs = t.sin22[(idx & 1023) + 256];
            pos[0] += static_cast<int32_t>((static_cast<int64_t>(amount) * cs) >> 22);
            pos[1] += static_cast<int32_t>((static_cast<int64_t>(amount) * sn) >> 22);
        }
        // The staged REPLICA peers walk the SAME witnessed loop (threshold
        // 30% of summed radii, push (thr - dist)/4 through the quantized
        // table). A ClientState peer's "live re-read" is its staged snapshot —
        // the row has no registry entity, and the caller stages only live,
        // undead peers. Empty for every ordinary resolve. [orig: the same
        // @ 0x4b3a5c-0x4b3c52 loop — replica rows are ordinary persons to it]
        for (int32_t pi = 0; pi < replica_peer_count_; ++pi) {
            const ReplicaPeer &p = replica_peers_[pi];
            if (p.handle == replica_exclude_handle_) continue;
            const int32_t threshold = 30 * (my_radius + p.radius) / 100;
            const int32_t ddx = p.x - pos[0];
            const int32_t ddy = p.y - pos[1];
            const int32_t ddz = p.z - pos[2];
            if (abs32(ddx) > threshold || abs32(ddy) > threshold || abs32(ddz) > threshold)
                continue;
            if (vec_len_ftol(ddx, ddy, 0) > threshold) continue;
            const int32_t dist = vec_len_ftol(pos[0] - p.x, pos[1] - p.y, 0);
            if (dist > threshold) continue;
            const int32_t amount = (threshold - dist) >> 2;
            const int32_t ang = static_cast<int32_t>(
                std::atan2(static_cast<double>(pos[1] - p.y),
                           static_cast<double>(pos[0] - p.x)) *
                kBamPerRadian);
            const uint32_t idx = (0x200000u - static_cast<uint32_t>(ang)) >> 22;
            const DirTable &t = dir_table();
            const int32_t sn = t.sin22[idx & 1023];
            const int32_t cs = t.sin22[(idx & 1023) + 256];
            pos[0] += static_cast<int32_t>((static_cast<int64_t>(amount) * cs) >> 22);
            pos[1] += static_cast<int32_t>((static_cast<int64_t>(amount) * sn) >> 22);
        }
    }

    // Leaving the ladder: latched at resolve start, nothing re-latched, a live
    // player body — push 0.375u along +bodyHeading (over the lip on a natural
    // top-out) and arm the local pitch restore. [orig: @ 0x4b3c5c-0x4b3cf9]
    if (ladder_io != nullptr && was_on_ladder && !ladder_entity.valid() && is_player &&
        (ent == nullptr || ((ent->flags | ent->engine_flags) & kEntityFlagDead) == 0) &&
        ladder_io->body_heading != nullptr) {
        const double rad = static_cast<double>(*ladder_io->body_heading) *
                           (3.14159265358979323846 / 2147483648.0);
        const int32_t c = static_cast<int32_t>(std::cos(rad) * 4194304.0);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * 4194304.0);
        pos[0] += static_cast<int32_t>((24576LL * c) >> 22);
        pos[1] += static_cast<int32_t>((24576LL * s) >> 22);
        if (ladder_io->is_local_player && ladder_io->pitch_restore_active != nullptr &&
            ladder_io->pitch_restore_target != nullptr &&
            ladder_io->pitch_restore_prev != nullptr &&
            ladder_io->view_pitch != nullptr) {
            *ladder_io->pitch_restore_active = true;          // [orig: +0x2C |= 4]
            *ladder_io->pitch_restore_target = 4096;          // [orig: aimPitch = 4096]
            *ladder_io->pitch_restore_prev = *ladder_io->view_pitch; // [orig: dword_B7900C]
        }
    }
    // The local-player pitch-restore chase, each resolve while armed: quarter-step
    // toward the target, per-tick step clamped ±0x1E00000, done inside +16 of the
    // target; a user pitch-up past the last written value cancels it.
    // [orig: @ 0x4b3d04-0x4b3d55]
    if (ladder_io != nullptr && ladder_io->is_local_player &&
        ladder_io->pitch_restore_active != nullptr && *ladder_io->pitch_restore_active &&
        ladder_io->pitch_restore_target != nullptr &&
        ladder_io->pitch_restore_prev != nullptr && ladder_io->view_pitch != nullptr) {
        const int32_t target = *ladder_io->pitch_restore_target;
        int32_t step = io::bam_sar(
            io::bam_add(io::bam_sub(target, *ladder_io->view_pitch), 2), 2);
        if (step > 0x1E00000) step = 0x1E00000;
        if (step < -0x1E00000) step = -0x1E00000;
        const int32_t next = io::bam_add(*ladder_io->view_pitch, step);
        if (next > io::bam_add(target, 16)) {
            if (on_ladder || *ladder_io->view_pitch <= *ladder_io->pitch_restore_prev) {
                *ladder_io->view_pitch = next;
                *ladder_io->pitch_restore_prev = next;
            } else {
                // The user pulled the view up past the chase — cancel.
                *ladder_io->pitch_restore_active = false;
                *ladder_io->pitch_restore_target = 0;
            }
        } else {
            *ladder_io->view_pitch = target; // arrived: snap + disarm
            *ladder_io->pitch_restore_active = false;
            *ladder_io->pitch_restore_target = 0;
        }
    }

    // Ground settle tail. [orig: @ 0x4b3d6e-0x4b3da9 — quantize Z up to the 6144
    // grid, probe down 2.0u through terrain + candidates, restore Z, return feet
    // clearance; the probe stores the hit entity into groundEntity.]
    const int32_t saved_z = pos[2];
    const int32_t feet_z = pos[2] - capsule_bottom;
    pos[2] = (pos[2] + 6143) & ~0x17FF;
    EntityHandle ground_hit;
    const int32_t ground =
        raycast_ground(world, source, pos, 0, 0, 0, 0x20000, &ground_hit);
    pos[2] = saved_z;
    // The probe's hit ALWAYS lands in groundEntity — null on a miss, overwriting
    // even a same-resolve CL latch. Generic ground is still resolved only by
    // terrain or a type-1 CB solid.
    // [orig: the unconditional +0x28 store in
    // Entity_RaycastGroundHeightAndObject @ 0x414370]
    if (ent != nullptr) ent->ground_target = ground_hit;
    if (out_ground != nullptr) *out_ground = ground_hit;

    state.prev_pos[0] = pos[0];
    state.prev_pos[1] = pos[1];
    state.prev_pos[2] = pos[2];
    if (is_local) {
        local_resolve_debug.pos[0] = pos[0];
        local_resolve_debug.pos[1] = pos[1];
        local_resolve_debug.pos[2] = pos[2];
        local_resolve_debug.foot_clearance = feet_z - ground;
    }
    return feet_z - ground;
}

// See collision.h — the replica seam. The interior IS resolve_entity: an
// invalid source handle takes every null-entity arm the resolver already
// carries (no flag latches, no blink store, no debug capture), the ad-hoc
// candidate slice is staged under the never-allocated invalid key, and the
// peer spheres ride the staged span the repulsion loop walks after persons_.
// [orig: the remote org rows run the SAME Entity_MovementCollisionResolver
//  @0x4b2bd0 through the shared mover tails — there is no replica variant in
//  retail; this wrapper only rebuilds the two per-entity tables (candidate
//  slice, person slot) the transport-side row does not have]
int32_t CollisionWorld::resolve_replica(World &world, ResolveState &state, int32_t pos[3],
                                        int32_t vel_xy[2], int32_t &vel_z,
                                        int32_t capsule_bottom, int32_t capsule_top,
                                        bool is_player, uint32_t tick, int32_t anim_state_id,
                                        uint32_t anim_state_flags, const ReplicaPeer *peers,
                                        int32_t peer_count, uint16_t exclude_handle,
                                        uint32_t *entity_flags, EntityHandle *out_ground) {
    // The ad-hoc candidate slice at the query position — the pool-0 rule of
    // the 17th-tick builder (source radius 0x10000 + 4.0 u pad)
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0, pool-0 leg].
    const size_t arena_mark = arena_.size();
    const int32_t range = 0x10000 + 0x40000;
    CandidateSlice slice;
    slice.start = static_cast<int32_t>(arena_.size());
    slice.count = 0;
    for (const DynSlot &d : dynamics_) {
        const int32_t total = range + d.radius;
        if (abs32(d.x - pos[0]) > total || abs32(d.y - pos[1]) > total ||
            abs32(d.z - pos[2]) > total)
            continue;
        if (vec_len_ftol(d.x - pos[0], d.y - pos[1], d.z - pos[2]) > total) continue;
        if (arena_.size() >= 3000) break;
        arena_.push_back(d.h);
        ++slice.count;
    }
    for (const StaticSlot &s : statics_) {
        const int32_t sx = static_slot_coord_q16(s.x);
        const int32_t sy = static_slot_coord_q16(s.y);
        const int32_t sz = static_slot_coord_q16(s.z);
        const int32_t total = range + (static_cast<int32_t>(s.radius) << 16);
        if (abs32(sx - pos[0]) > total || abs32(sy - pos[1]) > total ||
            abs32(sz - pos[2]) > total)
            continue;
        if (vec_len_ftol(sx - pos[0], sy - pos[1], sz - pos[2]) > total) continue;
        if (arena_.size() >= 3000) break;
        arena_.push_back(s.h);
        ++slice.count;
    }
    const EntityHandle replica_key; // kInvalid — pool 15 slot 4095, never allocated
    candidates_[replica_key.packed] = slice;
    replica_peers_ = peers;
    replica_peer_count_ = peer_count;
    replica_exclude_handle_ = exclude_handle;
    replica_flags_ = entity_flags;
    int16_t health_dummy = 100; // damage legs are authority-gated off anyway
    const int32_t clearance = resolve_entity(
        world, replica_key, state, pos, vel_xy, vel_z, capsule_bottom,
        capsule_top, /*heading=*/0, /*body_pitch=*/0, is_player,
        /*is_authority=*/false, tick, anim_state_id, anim_state_flags,
        health_dummy, out_ground);
    replica_peers_ = nullptr;
    replica_peer_count_ = 0;
    replica_exclude_handle_ = 0xFFFF;
    replica_flags_ = nullptr;
    candidates_.erase(replica_key.packed);
    arena_.resize(arena_mark);
    return clearance;
}

std::vector<CollisionWorld::DebugInstance> CollisionWorld::debug_instances(
    World &world, const int32_t anchor[3], int32_t range, int32_t max_instances) const {
    std::vector<DebugInstance> out;
    for (const auto &kv : instances_) {
        if (max_instances > 0 && static_cast<int32_t>(out.size()) >= max_instances) break;
        EntityHandle h;
        h.packed = kv.first;
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        // The SAME per-instance view every query goes through: entity pose ->
        // collision_matrix_from_heading (quantized dir table) per section.
        const CollisionTargetView *tv = target_view(world, h, view, mats);
        if (tv == nullptr) continue;
        if (range > 0 && anchor != nullptr &&
            (abs32(tv->pos[0] - anchor[0]) > range || abs32(tv->pos[1] - anchor[1]) > range ||
             abs32(tv->pos[2] - anchor[2]) > range))
            continue;

        DebugInstance inst;
        inst.handle = h;
        inst.pos[0] = tv->pos[0];
        inst.pos[1] = tv->pos[1];
        inst.pos[2] = tv->pos[2];
        if (const Entity *e = world.registry.get(h))
            inst.heading_bam = bam_heading_from_mission_yaw_deg(static_cast<double>(e->yaw));

        const CollisionModel &m = *tv->model;
        for (size_t si = 0; si < m.sections.size(); ++si) {
            const CollisionSection &sec = m.sections[si];
            const CollisionMatrix &mat = tv->matrices[si];
            if (sec.volume_count == 0 || mat.disabled()) continue; // mirrors the query skip
            for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
                const CollisionVolume &vol = m.volumes[sec.volume_start + vi];
                DebugVolume dv;
                dv.type = vol.type;
                dv.flags = vol.flags;
                dv.min[0] = vol.min_x; dv.max[0] = vol.max_x;
                dv.min[1] = vol.min_y; dv.max[1] = vol.max_y;
                dv.min[2] = vol.min_z; dv.max[2] = vol.max_z;
                for (int c = 0; c < 8; ++c) {
                    const int32_t local[3] = {(c & 1) ? vol.max_x : vol.min_x,
                                              (c & 2) ? vol.max_y : vol.min_y,
                                              (c & 4) ? vol.max_z : vol.min_z};
                    mat.transform_point(local, dv.corners[c]);
                }
                inst.volumes.push_back(dv);
            }
        }
        if (!inst.volumes.empty()) out.push_back(std::move(inst));
    }
    return out;
}

std::vector<CollisionWorld::DebugHitboxEntity> CollisionWorld::debug_hitboxes(
    World &world, const int32_t anchor[3], int32_t range, int32_t max_entities,
    int32_t max_faces) const {
    std::vector<DebugHitboxEntity> out;
    int32_t face_budget = max_faces > 0 ? max_faces : INT32_MAX;
    for (const auto &kv : instances_) {
        if (max_entities > 0 && static_cast<int32_t>(out.size()) >= max_entities) break;
        EntityHandle h;
        h.packed = kv.first;
        // Persons have their own posed sphere query. Range-reject every other
        // instance from the same fixed entity position target_view uses before
        // asking the animation provider for section matrices.
        const Entity *world_entity = world.registry.get(h);
        if (world_entity == nullptr || world_entity->kind == EntityKind::Organic)
            continue;
        if (range > 0 && anchor != nullptr) {
            int32_t entity_pos[3];
            entity_pos_fixed(*world_entity, entity_pos);
            if (abs32(entity_pos[0] - anchor[0]) > range ||
                abs32(entity_pos[1] - anchor[1]) > range ||
                abs32(entity_pos[2] - anchor[2]) > range)
                continue;
        }
        CollisionTargetView view;
        std::vector<CollisionMatrix> mats;
        // The SAME husk-aware view + full-euler placement matrices the
        // projectile raycast walks — the drawn mesh IS the tested mesh.
        const CollisionTargetView *tv = target_view(world, h, view, mats);
        if (tv == nullptr) continue;

        DebugHitboxEntity ent;
        ent.handle = h;
        ent.pos[0] = tv->pos[0];
        ent.pos[1] = tv->pos[1];
        ent.pos[2] = tv->pos[2];
        ent.bound_radius = tv->bound_radius;
        if (const Entity *e = world.registry.get(h)) {
            ent.husk = (e->engine_flags & kEntityFlagHusk) != 0 &&
                       instances_.at(h.packed).husk_model_id >= 0;
            if (e->bound_radius > 0.0f) ent.bound_radius = to_fixed(e->bound_radius);
        }

        const CollisionModel &m = *tv->model;
        ent.has_faces = !m.faces.empty();
        for (size_t si = 0; si < m.sections.size() && face_budget > 0; ++si) {
            const CollisionSection &sec = m.sections[si];
            if (sec.face_count <= 0) continue;
            const CollisionMatrix &mat = tv->matrices[si];
            if (mat.disabled()) continue; // mirrors the raycast skip
            const CollisionFaceVertex *verts =
                m.face_vertices.data() + sec.face_vertex_start;
            ent.face_total += sec.face_count;
            for (int32_t fi = 0; fi < sec.face_count && face_budget > 0; ++fi) {
                const CollisionFace &face = m.faces[sec.face_start + fi];
                DebugHitboxFace df;
                df.material = face.material;
                df.flags = face.flags;
                for (int k = 0; k < 3; ++k) {
                    const CollisionFaceVertex &vt = verts[face.v[k]];
                    // Q8 int16 -> 16.16 (<< 8), then the section world matrix —
                    // the raycast's own vertex scale and transform.
                    const int32_t local[3] = {static_cast<int32_t>(vt.x) << 8,
                                              static_cast<int32_t>(vt.y) << 8,
                                              static_cast<int32_t>(vt.z) << 8};
                    mat.transform_point(local, df.v[k]);
                }
                ent.faces.push_back(df);
                --face_budget;
            }
        }
        out.push_back(std::move(ent));
    }
    return out;
}

std::vector<CollisionWorld::DebugPersonSection>
CollisionWorld::debug_person_sections(World &world, const int32_t anchor[3],
                                      int32_t range, int32_t max_entities) {
    std::vector<DebugPersonSection> out;
    if (max_entities <= 0) return out;
    int32_t entity_count = 0;
    world.registry.for_each([&](const Entity &entity) {
        if (entity_count >= max_entities || entity.kind != EntityKind::Organic ||
            (entity.engine_flags & 0x02000001u) != 0)
            return;
        int32_t entity_pos[3];
        entity_pos_fixed(entity, entity_pos);
        if (range >= 0 &&
            (abs32(entity_pos[0] - anchor[0]) > range ||
             abs32(entity_pos[1] - anchor[1]) > range ||
             abs32(entity_pos[2] - anchor[2]) > range))
            return;

        // Late-spawned players enter pool 0 after the mission-start graphic
        // sweep. Resolve them through the same host hook RoundSim uses before
        // deciding whether an authored person model exists.
        ensure_entity_instance(world, entity.handle);

        CollisionTargetView view;
        std::vector<CollisionMatrix> matrices;
        const CollisionTargetView *target =
                target_view(world, entity.handle, view, matrices);
        if (target == nullptr || target->model == nullptr) return;
        bool any = false;
        for (int32_t si = 0;
             si < static_cast<int32_t>(target->model->sections.size()); ++si) {
            const CollisionSection &section = target->model->sections[si];
            if (section.radius < 0) continue; // host/test sentinel; authored zero is valid
            DebugPersonSection debug;
            debug.handle = entity.handle;
            debug.section = si;
            debug.authored_radius = section.radius;
            debug.radius = person_effective_radius(si, section.radius, 0);
            debug.masked =
                    (entity.section_mask &
                     (1u << (static_cast<uint32_t>(si) & 31u))) != 0;
            target->matrices[si].transform_point(section.center, debug.center);
            out.push_back(debug);
            any = true;
        }
        if (any) ++entity_count;
    });
    return out;
}

// See collision.h — the org1 on-ladder person probe: a live pool-0 person near
// the point 1.25u ahead of the climber holds the climb. Coarse box 1.125u on
// both axes, then the Z band: my feet + half my bound reach above their feet
// minus half their bound, while my feet stay at or below theirs (someone on the
// ladder above me). The person set is the same staged pool-0 slice repulsion
// walks; positions re-read live, as retail reads the pool entity directly.
// [orig: the g_pool_list[0] scan @ 0x4bf9b7-0x4bfa2b — live (ItemTypeIndex),
//  not dead (Flags & 2), not self; |Δ| <= 73728 per axis; the band
//  @ 0x4bfa08-0x4bfa29]
bool CollisionWorld::ladder_person_ahead(World &world, EntityHandle self,
                                         int32_t probe_x, int32_t probe_y,
                                         int32_t self_z, int32_t self_bound) {
    for (const PersonSlot &p : persons_) {
        if (p.h == self) continue;
        const Entity *peer = world.registry.get(p.h);
        if (peer == nullptr ||
            ((peer->flags | peer->engine_flags) & kEntityFlagDead) != 0)
            continue;
        int32_t live[3];
        entity_pos_fixed(*peer, live);
        if (abs32(probe_x - live[0]) > 73728 || abs32(probe_y - live[1]) > 73728)
            continue;
        if (self_z + (self_bound >> 1) >= live[2] - (p.radius >> 1) && self_z <= live[2])
            return true;
    }
    return false;
}

// Contact-flag side effects shared by both passes. [orig: the flag dispatch inside
// the resolver loop @ 0x4b30b7-0x4b351e]
void CollisionWorld::apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health,
                                       bool is_authority) {
    // A replica resolve latches the zone bits into the staged flags mirror —
    // the damage legs below stay entity-only (and authority-only) either way.
    if (replica_flags_ != nullptr && flags != 0) {
        if ((flags & 0x4u) != 0) *replica_flags_ |= kEntityFlagArmoryZone;
        if ((flags & 0x400u) != 0) *replica_flags_ |= kEntityFlagVehicleLoadoutZone;
    }
    if (ent == nullptr || flags == 0) return;
    // DH/DM/DL contact damage is authority-only AND gated off for
    // EngineFlags 0x4000000 entities.
    // [orig: the is_authority + (Flags & 0x4000000) == 0 wrap @ 0x4b3139-0x4b3148]
    if (is_authority && (ent->engine_flags & kEntityFlagIndestructible) == 0) {
        // Damage low/medium/high. [orig: @ 0x4b317b-0x4b31d7 — -1 / -6 / -50 HP]
        if ((flags & 0x40u) != 0 && health > 0) health = static_cast<int16_t>(health - 1);
        if ((flags & 0x80u) != 0 && health > 0) health = static_cast<int16_t>(health - 6);
        if ((flags & 0x100u) != 0 && health > 0) health = static_cast<int16_t>(health - 50);
        // CT/change-team touch (0x200) feeds the retail capture/team-change request
        // callback (`Server_OnPlayerTouchCaptureZone @ 0x500ba0`). Ours still rides
        // the zone system's independent proximity path (D-COL-6).
    }
    if ((flags & 0x4u) != 0) ent->flags |= kEntityFlagArmoryZone; // type 6 [orig: @ 0x4b34a0]
    if ((flags & 0x400u) != 0)
        ent->flags |= kEntityFlagVehicleLoadoutZone; // type 11 [orig: @ 0x4b34ae]
}


} // namespace opennova::world
