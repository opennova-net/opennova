#include <runtime/world/collision.h>
#include <base/io/perf_clock.h>

// Split out of collision.cpp (quality campaign W3-2). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Contact resolution — the vehicle hull and entity resolvers — plus the debug views
// the F3 overlay reads.

#include <runtime/world/angle.h>
#include <runtime/world/dir_table.h>
#include <algorithm>
#include <cmath>

#include <base/io/bam.h>

#include "collision_detail.h"

#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/world/vehicle_collision_damage.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

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
        if (!target_bound(world, ch, bound_pos, bound_radius, /*solid_only=*/false)) continue;
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
        if (contact_debug_enabled_)
            contact_debug_record(ContactDebugKind::kVehicleHull, world.logic_tick,
                                 ch, pos, 0xFF);
    }
    return severity;
}

int32_t CollisionWorld::resolve_entity(World &world, EntityHandle source, ResolveState &state,
                                       int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                                       int32_t capsule_bottom, int32_t capsule_top,
                                       int32_t heading, int32_t body_pitch, bool is_player_class,
                                       bool is_authority, uint32_t tick, int32_t anim_state_id,
                                       uint32_t anim_state_flags, int16_t &health,
                                       EntityHandle *out_ground,
                                       const LadderResolveIO *ladder_io,
                                       const int32_t *eye_offset,
                                       devtools::TickProfile *profile) {
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
            pos[2] -= is_player_class ? vel_z : 2 * vel_z;
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
    // CameraOffset, all three axes @ 0x4b2ee0-0x4b2ef8), feet; radii
    // {collisionRadius, 0.3125, outerRadius}.] The eye is the entity's +0x74
    // CameraOffset, produced by the think before it calls the resolver (org1
    // @0x4b9910 kong 155519-155521, resolver calls at 155739/155831). With it
    // missing the point sat at the head column, ~0.9 u lower than retail's, and
    // a body walking along a truck bed into the cab met the cab's BACK face as
    // the least-penetration plane (pushed back, pinned on the bed for good)
    // where retail's higher point meets the TOP face and the body rides up
    // onto the roof and off the front -- the 00TRg wave-3 convoy pin, probe
    // diff 2026-08-23 (AI-PARITY-CONCEPT 6.11). D-COL-4.
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
        // The org1 CameraOffset when the caller carries none: h = max(top -
        // bottom, 0x9000), Z = h*cos(lean), lateral = (3*(h*sin(lean))>>2)
        // rotated by Yaw -- lean at rest here, so X = Y = 0.
        // [orig: @0x4b9910 kong 155492-155521]
        int32_t eye[3] = {0, 0, capsule_top - capsule_bottom};
        if (eye[2] < 0x9000) eye[2] = 0x9000;
        if (eye_offset != nullptr) {
            eye[0] = eye_offset[0];
            eye[1] = eye_offset[1];
            eye[2] = eye_offset[2];
        }
        points[0] = {pos[0], pos[1], pos[2] + head_lift, 0};
        points[1] = {pos[0] + eye[0], pos[1] + eye[1], pos[2] + eye[2], 0}; // [orig: @ 0x4b2ee0-0x4b2ef8]
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

    // Production entities carry the exact Entity_InitFromModel stamp; replica
    // rows arrive with the same typed result staged by resolve_replica. A row
    // without either has no invented compatibility radius.
    const int32_t source_bound_radius_q16 =
        ent != nullptr && ent->bound_radius > 0.0f
            ? to_fixed(ent->bound_radius)
            : replica_source_bound_radius_q16_;

    ContactQuery q;
    q.points = points;
    q.radii = radii;
    q.num_points = num_points;
    q.prev_pos[0] = state.prev_pos[0];
    q.prev_pos[1] = state.prev_pos[1];
    q.prev_pos[2] = state.prev_pos[2];
    q.source_bound_radius = source_bound_radius_q16;
    // Mask bit 0x1 arms the inflated CL recontact test while the ladder latch
    // rides; retail recomputes the arg per query, so a mid-loop fresh entry
    // upgrades the remaining candidates. [orig: v137 + 2*v130 @ 0x4b2f7c/0x4b35af]
    q.mask = static_cast<uint8_t>((on_ladder ? 1 : 0) | (is_player_class ? 2 : 0));
    q.query_is_player = is_player_class;

    int32_t total_force[3] = {0, 0, 0};
    LadderContact ladder;
    EntityHandle ladder_entity;
    // The last candidate that pushed in the first force pass — the run-over
    // kill's pusher [orig: the var_80 store @0x4b30a9].
    EntityHandle pusher;
    // Retail leaves the global stale across SKIPPED resolves (the early ret
    // @ 0x4b2cfe precedes the store); zeroing at entry only diverges on skip
    // ticks, where no org1 climber runs anyway.
    resolver_applied_push = false; // [orig: slot re-zero @ 0x4b3734]

    devtools::ProfileLap lap(profile);
    auto it = candidates_.find(source.packed);
    dbg_last_contact = EntityHandle{};
    dbg_last_contact_item = 0;
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
                if (!target_bound(world, ch, bound_pos, bound_radius, /*solid_only=*/false)) continue;
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
                q.mask = static_cast<uint8_t>((on_ladder ? 1 : 0) | (is_player_class ? 2 : 0));
                ContactResult res;
                const bool contact = collision_contact_force(*tv, q, blink, ladder, res);
                const Entity *target_entity = world.registry.get(ch);
                const bool powerup = contact && target_entity != nullptr &&
                    target_entity->has_item_def &&
                    (target_entity->item_attrib & kItemAttribPowerup) != 0;
                const bool move_callback = contact && target_entity != nullptr &&
                    target_entity->has_item_def && !powerup &&
                    (target_entity->item_attrib & kItemAttribMoveCallback) != 0;
                if (pass == 0 && is_authority && move_callback)
                    record_movement_callback_contact(source, ch);
                // Powerup and MoveCB ItemDefs bypass the ordinary solid-force
                // fold. MoveCB publishes above; the distinct Powerup callback
                // remains D-COL-8. A fresh CL entry below zeroes accumulated
                // ordinary force; mostly-vertical negative force is standing
                // pressure and is dropped.
                // [orig: attrib branches @0x4B2F90..0x4B2FF5; force fold
                // @0x4B3002..0x4B30AF/@0x4B3603..0x4B36B9]
                if (contact && !powerup && !move_callback &&
                    !suppress_model_force) {
                    dbg_last_contact = ch; // debug-card tap
                    if (const Entity *ce = world.registry.get(ch))
                        dbg_last_contact_item = ce->item_id;
                    // Dev capture, pass 0 only so the relaxation passes never
                    // double-count one contact.
                    if (contact_debug_enabled_ && pass == 0)
                        contact_debug_record(ContactDebugKind::kMoveContact, tick,
                                             ch, pos, 0xFF);
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
                    if (pass == 0) {
                        pass_force[2] -= f[2];
                        pusher = ch; // [orig: @0x4b30a9]
                    }
                    pass_contact = true;
                }
                if (pass == 0) {
                    // The contact-flag dispatch runs whether or not the query
                    // produced force — a pure ladder/zone touch still latches.
                    // [orig: the goto LABEL_67 on a zero return @ 0x4b2fa5]
                    if (is_authority && (res.flags & kTouchChangeTeam) != 0)
                        record_change_team_contact(source, ch);
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
                                (on_ladder || is_player_class || ladder_io->ai_wants_climb)) {
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
                                    if (!is_player_class || player_gate) {
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
                                    if (is_player_class) {
                                        // Every class-bit body eases (@ 0x4b33aa):
                                        // one sixteenth of the yaw error moves
                                        // the view yaw (+0x10) AND the body
                                        // heading (+0x8C); only the LOCAL entity
                                        // drags g_LocalPlayerLookYaw along
                                        // (@ 0x4b33ca), which the embedder's
                                        // mouse accumulator inherits through the
                                        // write-back. [orig: (delta+8)>>4
                                        // @ 0x4b33bc; g_LocalPlayerLookYaw @ 0x4b33d2]
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
                                        io::kRadiansPerBam;
                                    const int32_t c = static_cast<int32_t>(
                                        std::cos(rad) * io::kQ22One);
                                    const int32_t s = static_cast<int32_t>(
                                        std::sin(rad) * io::kQ22One);
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
                // The pass-2 contact flag stays LOCAL: its only retail
                // reader is the half-force gate below. [orig: set @ 0x4b36b1,
                // sole read @ 0x4b36cc-0x4b36da]
                if (pass_contact && !ladder_entity.valid()) {
                    // [orig: @ 0x4b36da — second-pass half force only without a CL contact]
                    total_force[0] += pass_force[0] >> 1; // [orig: @ 0x4b36e2]
                    total_force[1] += pass_force[1] >> 1;
                }
            }
        }
    }

    // Apply the push-out; a net push resets the idle skip counter and latches
    // the resolver-applied-push global org1's facing press gates on. [orig:
    // @ 0x4b3746-0x4b375a; pad_370[3] = 0 @ 0x4b3773; the outFlags slot is 1
    // only when the total is nonzero @ 0x4b3767, stored @ 0x4b3a62]
    if (total_force[0] != 0 || total_force[1] != 0 || total_force[2] != 0) {
        state.skip_counter = 0;
        resolver_applied_push = true;
    }
    pos[0] += total_force[0];
    pos[1] += total_force[1];
    pos[2] += total_force[2];
    lap.mark(devtools::Slot::SIM_AI_INFANTRY_COLLISION_CONTACTS);

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

    // The run-over kill [orig: @0x4b37c2..0x4b39f7 — gates and the kill in
    // vehicle_collision_damage.h]. The pusher's displacement is its mover-entry
    // savedLivePose delta (+0x80); the victim's is measured from the resolver's
    // previous-tick pose (its +0x80 stamp lives in the body motors' prologues).
    if (pusher.valid() && ent != nullptr && is_authority) {
        const Entity *p = world.registry.get(pusher);
        if (p != nullptr && p->has_item_def) {
            const int32_t pdx = p->saved_live_valid
                    ? to_fixed(p->position.x) - p->saved_live_pos[0] : 0;
            const int32_t pdy = p->saved_live_valid
                    ? to_fixed(p->position.y) - p->saved_live_pos[1] : 0;
            const int32_t vdx = pos[0] - state.prev_pos[0];
            const int32_t vdy = pos[1] - state.prev_pos[1];
            const int32_t rdx = pdx - vdx;
            const int32_t rdy = pdy - vdy;
            const double rl = std::sqrt(static_cast<double>(rdx) * rdx +
                                        static_cast<double>(rdy) * rdy);
            const double pl = std::sqrt(static_cast<double>(pdx) * pdx +
                                        static_cast<double>(pdy) * pdy);
            const int32_t rel_move = rl >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(rl);
            const int32_t pusher_move = pl >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(pl);
            bool berserk = false;
            const AiEntity *pa = world.ai.for_handle(pusher);
            const AiEntity *va = world.ai.for_handle(source);
            berserk = (pa != nullptr && (pa->slot.f[AiSlot::kBehaviorFlags] & 0x200) != 0) ||
                      (va != nullptr && (va->slot.f[AiSlot::kBehaviorFlags] & 0x200) != 0);
            const uint32_t vflags = ent->flags | ent->engine_flags;
            if (run_over_kill_applies(p->item_type == 1, ent->ground_target == pusher,
                                      (p->flags & kEntityFlagDead) != 0, health,
                                      (vflags & kEntityFlagDead) != 0, pusher_move, rel_move,
                                      p->team == ent->team, berserk, is_authority,
                                      (vflags & kEntityFlagIndestructible) != 0)) {
                // A player victim: retail also exempts a spectating slot
                // (@0x4b393f — the player-slot byte +0x188D7); the +0x124
                // dword gate @0x4b391f is unmodeled (a deployed player's is
                // non-zero).
                const int quadrant = run_over_quadrant(
                        bam_heading_from_mission_yaw_deg(static_cast<double>(ent->yaw)),
                        rdx, rdy);
                ent->death_anim_state = compute_death_anim_state(
                        kRunOverDeathBone, quadrant, kRunOverDeathCause);
                ent->last_attacker = p->primary_occupant; // [orig: +0x178 = pusher->occupantEntity]
                health = 0;
                RoundDeath d;
                d.victim = source;
                d.killer = p->primary_occupant;
                d.victim_handle = source.packed;
                d.killer_handle = p->primary_occupant.valid()
                        ? p->primary_occupant.packed : 0xFFFFu;
                world.round_sim.deaths.push_back(d);
            }
        }
    }

    // Inter-entity sphere repulsion (no model contact only). [orig: @ 0x4b3a5c-0x4b3c52 —
    // threshold 30% of summed radii, push (thr - dist)/4 along the atan2 direction
    // via the quantized table with the (0x200000 - bam) index.]
    lap.restart();
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
        int32_t my_radius = source_bound_radius_q16;
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
        const auto replica_peer_push = [&](int32_t pi) {
            const ReplicaPeer &p = replica_peers_[pi];
            if (p.handle == replica_exclude_handle_) return;
            const int32_t threshold = 30 * (my_radius + p.radius) / 100;
            const int32_t ddx = p.x - pos[0];
            const int32_t ddy = p.y - pos[1];
            const int32_t ddz = p.z - pos[2];
            if (abs32(ddx) > threshold || abs32(ddy) > threshold || abs32(ddz) > threshold)
                return;
            if (vec_len_ftol(ddx, ddy, 0) > threshold) return;
            const int32_t dist = vec_len_ftol(pos[0] - p.x, pos[1] - p.y, 0);
            if (dist > threshold) return;
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
        };
        // The cell gather: every peer that can pass the threshold test lies
        // within max_reach of the row on x and y. The gather covers TWICE
        // that box around the pre-walk pose, so a peer outside it is more
        // than 2 * max_reach away; walked in table order, the pushes are
        // identical to the full walk. The walk moves the row: while it stays
        // within max_reach of the pre-walk pose every peer in reach is still
        // inside the gathered box, and the first push that carries it past
        // that restarts the row on the full walk from its pre-walk pose
        // (rare: one push is at most a quarter of one threshold).
        bool walked = false;
        if (replica_peer_index_enabled_ &&
            replica_peer_index_.src == replica_peers_ &&
            replica_peer_index_.count == replica_peer_count_ &&
            replica_peer_count_ > 0) {
            const int32_t max_reach =
                30 * (my_radius + replica_peer_index_.max_radius) / 100;
            const int32_t pos0[2] = {pos[0], pos[1]};
            std::vector<int32_t> &gathered = replica_peer_index_.gathered;
            gathered.clear();
            const int64_t cx0 = static_cast<int64_t>(pos0[0] - 2 * max_reach) >> 18;
            const int64_t cx1 = static_cast<int64_t>(pos0[0] + 2 * max_reach) >> 18;
            const int64_t cy0 = static_cast<int64_t>(pos0[1] - 2 * max_reach) >> 18;
            const int64_t cy1 = static_cast<int64_t>(pos0[1] + 2 * max_reach) >> 18;
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                for (int64_t cy = cy0; cy <= cy1; ++cy) {
                    const uint64_t key = (static_cast<uint64_t>(cx) << 32) ^
                                         (static_cast<uint64_t>(cy) & 0xFFFFFFFFu);
                    const auto cell = replica_peer_index_.cells.find(key);
                    if (cell == replica_peer_index_.cells.end()) continue;
                    gathered.insert(gathered.end(), cell->second.begin(),
                                    cell->second.end());
                }
            }
            std::sort(gathered.begin(), gathered.end());
            walked = true;
            for (const int32_t pi : gathered) {
                replica_peer_push(pi);
                if (abs32(pos[0] - pos0[0]) > max_reach ||
                    abs32(pos[1] - pos0[1]) > max_reach) {
                    pos[0] = pos0[0];
                    pos[1] = pos0[1];
                    walked = false;
                    break;
                }
            }
        }
        if (!walked) {
            for (int32_t pi = 0; pi < replica_peer_count_; ++pi)
                replica_peer_push(pi);
        }
    }

    lap.mark(devtools::Slot::SIM_AI_INFANTRY_COLLISION_REPULSION);

    // Leaving the ladder: latched at resolve start, nothing re-latched, a live
    // class-bit body — push 0.375u along +bodyHeading (over the lip on a natural
    // top-out; @ 0x4b3c78) and, for the LOCAL entity only (@ 0x4b3cdc), arm the
    // pitch restore. [orig: @ 0x4b3c5c-0x4b3cf9]
    if (ladder_io != nullptr && was_on_ladder && !ladder_entity.valid() && is_player_class &&
        (ent == nullptr || ((ent->flags | ent->engine_flags) & kEntityFlagDead) == 0) &&
        ladder_io->body_heading != nullptr) {
        const double rad = static_cast<double>(*ladder_io->body_heading) *
                           io::kRadiansPerBam;
        const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
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
    lap.restart();
    const int32_t ground =
        raycast_ground(world, source, pos, 0, 0, 0, 0x20000, &ground_hit);
    lap.mark(devtools::Slot::SIM_AI_INFANTRY_COLLISION_GROUND);
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
void CollisionWorld::stage_replica_peer_index(const ReplicaPeer *peers,
                                              int32_t count, uint32_t tick) {
    ReplicaPeerIndex &index = replica_peer_index_;
    if (index.src == peers && index.count == count && index.tick == tick) return;
    index.src = peers;
    index.count = count;
    index.tick = tick;
    index.max_radius = 0;
    index.cells.clear(); // keys follow the rows; retaining them would grow with every cell ever visited
    for (int32_t i = 0; i < count; ++i) {
        const ReplicaPeer &p = peers[i];
        if (p.radius > index.max_radius) index.max_radius = p.radius;
        // 4.0 u cells (q16 >> 18); the gather spans every cell the reach box
        // touches, so the cell size only trades gather width for cell count.
        const int64_t cx = static_cast<int64_t>(p.x) >> 18;
        const int64_t cy = static_cast<int64_t>(p.y) >> 18;
        const uint64_t key = (static_cast<uint64_t>(cx) << 32) ^
                             (static_cast<uint64_t>(cy) & 0xFFFFFFFFu);
        index.cells[key].push_back(i); // table order within a cell
    }
}

int32_t CollisionWorld::resolve_replica(World &world, ResolveState &state, int32_t pos[3],
                                        int32_t vel_xy[2], int32_t &vel_z,
                                        int32_t capsule_bottom, int32_t capsule_top,
                                        int32_t source_bound_radius_q16,
                                        bool is_player_class, uint32_t tick, int32_t anim_state_id,
                                        uint32_t anim_state_flags, const ReplicaPeer *peers,
                                        int32_t peer_count, uint16_t exclude_handle,
                                        uint32_t *entity_flags, EntityHandle *out_ground) {
    // The row's candidate slice. Retail refreshes every pool-0 entity's slice
    // on the 17-tick edge — the client's wire-built persons included — and the
    // movement resolver walks that slice, up to 16 ticks stale by design
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0 behind the
    // g_ProxSliceRefreshCounter >= 0x10 gate in Entity_UpdateAllEntities
    // @ 0x4c240f]. build_tables keeps
    // exactly that slice per wire person proxy (wire_candidates_, the pool-0
    // rule: source bound + 4.0 u pad), so a resolved row reads it here. A row
    // no edge has seen yet (it arrived between edges) builds the same slice
    // ad hoc at the query position, and so does a row that has moved past
    // the pad since its slice was built: retail rebuilds every list on the
    // spawn/teleport edges that make such a jump [orig: the
    // Entity_BuildProximityListsFromPools callers Entity_ResetToSpawnState
    // @ 0x4b98eb, Entity_RespawnVehicle @ 0x460133, WacCmd_Tele @ 0x4f2384,
    // WacCmd_TeleSsn @ 0x4f7f48, EventAction_TeleportEntityToSpawn @ 0x43e14f].
    const size_t arena_mark = arena_.size();
    const int32_t range = source_bound_radius_q16 + 0x40000;
    CandidateSlice slice;
    slice.start = static_cast<int32_t>(arena_.size());
    slice.count = 0;
    auto wire_slice = candidate_slices_built_
            ? wire_candidates_.find(exclude_handle)
            : wire_candidates_.end();
    if (wire_slice != wire_candidates_.end()) {
        const CandidateSlice &w = wire_slice->second;
        if (abs32(pos[0] - w.built_pos[0]) > 0x40000 ||
            abs32(pos[1] - w.built_pos[1]) > 0x40000 ||
            abs32(pos[2] - w.built_pos[2]) > 0x40000)
            wire_slice = wire_candidates_.end();
    }
    if (wire_slice != wire_candidates_.end()) {
        const CandidateSlice &w = wire_slice->second;
        for (int32_t i = 0; i < w.count; ++i)
            arena_.push_back(wire_arena_[static_cast<size_t>(w.start + i)]);
        slice.count = w.count;
    }
    for (const DynSlot &d : dynamics_) {
        if (wire_slice != wire_candidates_.end()) break;
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
        if (wire_slice != wire_candidates_.end()) break;
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
    stage_replica_peer_index(peers, peer_count, tick);
    replica_source_bound_radius_q16_ = source_bound_radius_q16;
    replica_flags_ = entity_flags;
    int16_t health_dummy = 100; // damage legs are authority-gated off anyway
    const int32_t clearance = resolve_entity(
        world, replica_key, state, pos, vel_xy, vel_z, capsule_bottom,
        capsule_top, /*heading=*/0, /*body_pitch=*/0, is_player_class,
        /*is_authority=*/false, tick, anim_state_id, anim_state_flags,
        health_dummy, out_ground);
    replica_peers_ = nullptr;
    replica_peer_count_ = 0;
    replica_exclude_handle_ = 0xFFFF;
    replica_source_bound_radius_q16_ = 0;
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
        // Both bounds read the row's ENTITY bound radius — the staged prox slot
        // radius is pose-widened for the projectile gate and would deepen the
        // band. [orig: other->boundRadius @ 0x4bfa29]
        const int32_t peer_bound =
            peer->bound_radius > 0.0f ? to_fixed(peer->bound_radius) : 0;
        if (self_z + (self_bound >> 1) >= live[2] - (peer_bound >> 1) &&
            self_z <= live[2])
            return true;
    }
    return false;
}

void CollisionWorld::record_change_team_contact(EntityHandle source,
                                                 EntityHandle trigger) {
    if (!source.valid() || !trigger.valid()) return;
    for (const GameplayContact &contact : change_team_contacts_) {
        if (contact.source == source && contact.target == trigger) return;
    }
    change_team_contacts_.push_back({source, trigger});
}

void CollisionWorld::record_movement_callback_contact(EntityHandle source,
                                                       EntityHandle target) {
    if (!source.valid() || !target.valid()) return;
    for (const GameplayContact &contact : movement_callback_contacts_) {
        if (contact.source == source && contact.target == target) return;
    }
    movement_callback_contacts_.push_back({source, target});
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
        // CT/change-team touches are recorded above from pass 0 with both exact
        // entity identities, then consumed by the capture transaction.
    }
    if ((flags & 0x4u) != 0) ent->flags |= kEntityFlagArmoryZone; // type 6 [orig: @ 0x4b34a0]
    if ((flags & 0x400u) != 0)
        ent->flags |= kEntityFlagVehicleLoadoutZone; // type 11 [orig: @ 0x4b34ae]
}


} // namespace opennova::world
