// The ladder climb legs of the infantry motor (the D-COL-5 port), split out of
// infantry.cpp (W3-7 size gate): the org2 on-ladder override + dismounts, the
// org1 on-ladder block, the view clamp, and the resolver-IO build. The gravity
// variant, the jump/bottom exit tails, and the root/gravity gates stay inline
// in tick_infantry where the witnessed flow interleaves them.
// Witness record: docs/world/world-wac-ai-re.md §30.

#include <cmath>

#include <base/io/bam.h>

#include <runtime/world/ai.h>
#include <runtime/world/infantry_ladder.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

void ladder_push_back(int32_t pos[3], int32_t yaw_bam) {
    const double rad =
        static_cast<double>(yaw_bam) * io::kRadiansPerBam;
    const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
    const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
    pos[0] -= static_cast<int32_t>((static_cast<int64_t>(c) << 15) >> 22);
    pos[1] -= static_cast<int32_t>((static_cast<int64_t>(s) << 15) >> 22);
}

void ladder_unlatch(Entity *ent) {
    if (ent == nullptr) return;
    ent->flags &= ~kEntityFlagLadderContact;
    ent->engine_flags &= ~kEntityFlagLadderContact;
}

void infantry_ladder_view_clamp(InfantryState &inf, uint32_t entity_flags) {
    if ((entity_flags & kEntityFlagLadderContact) == 0) return;
    const int32_t d = io::bam_sub(inf.target_heading, inf.body_heading);
    if (d > 1431655680)
        inf.target_heading = io::bam_add(inf.body_heading, 1431655680);
    else if (d < -1431655680)
        inf.target_heading = io::bam_sub(inf.body_heading, 1431655680);
}

LadderResolveIO make_ladder_resolve_io(AiEntity &e, int32_t tick_start_z) {
    InfantryState &inf = e.inf;
    LadderResolveIO lio;
    lio.tick_start_z = tick_start_z;
    lio.prone = inf.stance == InfantryState::Stance::kProne;   // MoveOrder 0x100
    lio.crouch = inf.stance == InfantryState::Stance::kCrouch; // MoveOrder 0x200
    lio.ai_wants_climb = false; // the AI move-order writer rides its slice
    lio.is_local_player = inf.is_local_player;
    if (inf.is_local_player) {
        lio.view_yaw = &inf.target_heading; // entity+0x10 = the mouse yaw
        lio.view_pitch = &inf.look_pitch;   // entity+0x14
        lio.pitch_restore_active = &inf.pitch_restore_active;
        lio.pitch_restore_target = &inf.pitch_restore_target;
        lio.pitch_restore_prev = &inf.pitch_restore_prev;
    } else {
        lio.view_yaw = &e.heading;
        lio.view_pitch = &e.pitch;
        lio.ai_target_heading = &inf.target_heading;
        lio.ai_aim_heading = &inf.aim_heading;
    }
    lio.body_heading = &inf.body_heading;
    lio.body_pitch = &e.body_pitch;
    return lio;
}

// The on-ladder override + player dismounts (org2; EVERY tick — the 4th-tick
// gate covers only the stance selection, which is skipped while latched so
// our transition machinery never restarts the crossfade on an intermediate
// stamp; retail overwrites the field in the same tick, identical net state).
// While latched the body is never airborne and carries no vertical velocity;
// idle holds climb_idle, the forward fan climbs by the look-pitch sign, and
// the strafe/back fans step off the ladder and drop the latch. Climb MOTION is
// the climb clip's vertical root lane — the integrate zeroes only the
// horizontal pair. [orig: Entity_UpdateInfantryPlayerBody @ 0x4b7484-0x4b76d8]
void AiSystem::infantry_ladder_override(AiEntity &e, Entity *tick_entity) {
    InfantryState &inf = e.inf;
    if (!inf.is_local_player || tick_entity == nullptr || collision == nullptr ||
        e.health <= 0)
        return;
    const uint32_t flags = tick_entity->flags | tick_entity->engine_flags;
    if ((flags & kEntityFlagLadderContact) == 0 || (flags & kEntityFlagDead) != 0)
        return;
    const LadderContact &lf = collision->last_ladder_frame;
    tick_entity->flags &= ~kEntityFlagInAir; // [orig: & 0xFFFFDFFF @ 0x4b74ad]
    tick_entity->engine_flags &= ~kEntityFlagInAir;
    inf.airborne = false;
    inf.vel[2] = 0; // [orig: slideDecay = 0 @ 0x4b74b0]
    // STRAIGHT stamps like the jump block; the has_clip guard is the same
    // reimpl stripped-set guard the jump stamps carry.
    auto stamp = [&](int state) {
        if (root_motion == nullptr || !root_motion->has_clip(inf.adm_id, state))
            return;
        inf.begin_body_transition(state);
        inf.anim_pending = 0;
    };
    if (!inf.player_moving) {
        stamp(anim_state::kClimbIdle); // [orig: @ 0x4b76ce]
        return;
    }
    switch (inf.player_move_dir_index) {
        case 0:
        case 1:
        case 7: {
            // The forward fan climbs; the look-pitch sign picks the direction,
            // with the witnessed edge quirks: +0x7FFFFFFF, -1, and INT32_MIN
            // all resolve UP. [orig: cases 0/1/7 @ 0x4b74bc-0x4b750a]
            int sign = 0;
            if (inf.look_pitch > 0) sign = inf.look_pitch != 0x7FFFFFFF ? 1 : 0;
            if (static_cast<uint32_t>(inf.look_pitch) > 0x80000000u &&
                inf.look_pitch != -1)
                sign = -1;
            stamp(sign < 0 ? anim_state::kClimbDown : anim_state::kClimbUp);
            break;
        }
        case 2:
        case 6: {
            // Side dismount: 0.875u at the authored yaw -/+90°, a 0.25u hop,
            // then the 0.5u push off the face + unlatch.
            // [orig: case 2 @ 0x4b752a-0x4b75cd (yaw - 1073741760) /
            //  case 6 @ 0x4b75dc-0x4b767f (yaw + 1073741760)]
            const int32_t side_yaw = io::bam_add(
                lf.yaw,
                inf.player_move_dir_index == 2 ? -1073741760 : 1073741760);
            const double rad = static_cast<double>(side_yaw) *
                               io::kRadiansPerBam;
            const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
            const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
            e.pos[0] -= static_cast<int32_t>((57344LL * c) >> 22);
            e.pos[1] -= static_cast<int32_t>((57344LL * s) >> 22);
            e.pos[2] += 0x4000;
            ladder_push_back(e.pos, lf.yaw);
            ladder_unlatch(tick_entity);
            break;
        }
        case 3:
        case 4:
        case 5:
            // The back fan steps straight off the face.
            // [orig: cases 3-5 @ 0x4b7681-0x4b76cc]
            ladder_push_back(e.pos, lf.yaw);
            ladder_unlatch(tick_entity);
            break;
        default:
            break;
    }
}

// The org1 on-ladder block (NPC, post-resolve): hold behind a climber ahead,
// press into the face, and select climb_up vs climb_top by the anchor band.
// Dormant until the AI-order slice writes the entry order (the fresh-entry
// gate needs aiRuntime 0x400); live in tests via the latch. Retail's stamps
// here are DIRECT animStateId stores that leave the pending slot untouched, so
// the transition calls save/restore it.
// [orig: Entity_UpdateInfantryAI @ 0x4bf907-0x4bfad8]
void AiSystem::infantry_ladder_org1_block(AiEntity &e, World &world,
                                          Entity *tick_entity) {
    InfantryState &inf = e.inf;
    if (inf.is_local_player || tick_entity == nullptr || collision == nullptr)
        return;
    const uint32_t flags = tick_entity->flags | tick_entity->engine_flags;
    if ((flags & kEntityFlagLadderContact) == 0 || (flags & kEntityFlagDead) != 0)
        return;
    inf.vel[2] = 0; // [orig: slideDecay = 0 @ 0x4bf93a]
    // Direct-store mapping: the blend machinery zeroes anim_pending on every
    // retarget; retail's raw stores here do not. [orig: the bare mov stores
    // @ 0x4bfa2d / @ 0x4bfad8 / @ 0x4bfacc]
    auto stamp_keep_pending = [&](int state) {
        const int pending = inf.anim_pending;
        inf.begin_body_transition(state);
        inf.anim_pending = pending;
    };
    const LadderContact &lf = collision->last_ladder_frame;
    const double rad = static_cast<double>(inf.body_heading) *
                       io::kRadiansPerBam;
    const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
    const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
    const int32_t probe_x = e.pos[0] + static_cast<int32_t>((81920LL * c) >> 22);
    const int32_t probe_y = e.pos[1] + static_cast<int32_t>((81920LL * s) >> 22);
    bool held = false;
    if (inf.anim_state != anim_state::kClimbTop) {
        // The congestion hold: a live person near the probe point at or above
        // this climber freezes the climb at climb_idle. Both Z bounds read the
        // ENTITY bound radius, as retail reads entity->boundRadius on each row.
        // [orig: the pool-0 scan @ 0x4bf9ac-0x4bfa39 -> state 32]
        const int32_t bound = tick_entity->bound_radius > 0.0f
                                  ? to_fixed(tick_entity->bound_radius)
                                  : 0;
        held = collision->ladder_person_ahead(world, e.handle, probe_x, probe_y,
                                              e.pos[2], bound);
        if (held && root_motion != nullptr &&
            root_motion->has_clip(inf.adm_id, anim_state::kClimbIdle))
            stamp_keep_pending(anim_state::kClimbIdle);
    }
    if (held) return;
    if (!collision->resolver_applied_push) {
        // The 0.03125u facing press, gated on "the resolver applied no push
        // this resolve". [orig: @ 0x4bfa47-0x4bfaa3, gate @ 0x4bfa3e-0x4bfa45
        // on dword_B57C8C == 0 (the applied-push latch, not pass-2 contact)]
        const double lrad = static_cast<double>(lf.yaw) *
                            io::kRadiansPerBam;
        const int32_t lc = static_cast<int32_t>(std::cos(lrad) * io::kQ22One);
        const int32_t ls = static_cast<int32_t>(std::sin(lrad) * io::kQ22One);
        e.pos[0] += static_cast<int32_t>((static_cast<int64_t>(lc) << 11) >> 22);
        e.pos[1] += static_cast<int32_t>((static_cast<int64_t>(ls) << 11) >> 22);
    }
    // climb_up below (anchor - 0.75u) or when climb_top has no clip; climb_top
    // inside the anchor band. [orig: @ 0x4bfaca-0x4bfad8 — 49152 = 0.75u]
    const bool top_available =
        root_motion != nullptr &&
        root_motion->has_clip(inf.adm_id, anim_state::kClimbTop);
    const int next = (!top_available || e.pos[2] <= lf.anchor[2] - 49152)
                         ? anim_state::kClimbUp
                         : anim_state::kClimbTop;
    if (root_motion != nullptr && root_motion->has_clip(inf.adm_id, next))
        stamp_keep_pending(next);
}

} // namespace opennova::world
