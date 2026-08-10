#include "world/ai.h"

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Target scoring, the AI state-name table, and one handler per AI state — plus the
// dispatch table itself, which is why AiSystem::row is defined here.

#include "world/angle.h"
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

// ----------------------------------------------------------------------------
// Target scoring. [orig: AI_FindBestTargetB @0x466f60 scoring core.]
// ----------------------------------------------------------------------------
int32_t ai_score_target(int angle_diff, int distance, int primary_fov, int secondary_fov,
                        int primary_max, int secondary_max, int cand_primary_max,
                        int cand_secondary_max, int visibility, int cand_flags) {
    int angle_score;
    if (angle_diff >= ((primary_fov | 2) >> 1) || distance > primary_max ||
        distance > cand_primary_max) {
        if (angle_diff >= ((secondary_fov | 2) >> 1) || distance > secondary_max ||
            distance > cand_secondary_max)
            return -1; // [orig: goto LABEL_17 skip] outside both FOV/range gates. -1 (not 0) so the
                       // caller distinguishes a gate-fail from a legitimate in-gate score of 0 (a
                       // fully-stealthed target), which matters for the priority-bypass ordering.
        angle_score = static_cast<int>((static_cast<uint32_t>(secondary_fov - angle_diff) << 16) /
                                       static_cast<uint32_t>(secondary_fov));
    } else {
        angle_score = static_cast<int>((static_cast<uint32_t>(primary_fov - angle_diff) << 16) /
                                       static_cast<uint32_t>(primary_fov));
    }
    // range_score = 0x640000 / distance (closer = higher); distance 0 -> the numerator.
    int range_score = 0x640000;
    if (distance > 0x640000 || distance) range_score = 0x640000 / distance;
    // stealth_score from the visibility counter: 0 = fully visible (0x10000), >=16 = invisible (0).
    int stealth_score;
    if (visibility) {
        if (visibility >= 16) stealth_score = 0;
        else stealth_score = 0x10000 - (1 << (16 - visibility));
    } else {
        stealth_score = 0x10000;
    }
    int priority = (cand_flags & 0x4000) != 0 ? 393216 : 0x10000; // flag &0x4000 -> 6.0x weight
    int64_t inner = (static_cast<int64_t>(angle_score) * range_score + 0x8000) >> 16;
    int64_t mid = (static_cast<int64_t>(priority) * inner + 0x8000) >> 16;
    int64_t score = (static_cast<int64_t>(stealth_score) * mid + 0x8000) >> 16;
    return static_cast<int32_t>(score);
}

// ----------------------------------------------------------------------------
// State name table. [orig: Entity_LookupAIStateName @0x455cc0.]
// ----------------------------------------------------------------------------
const char *ai_state_name(int32_t state) {
    switch (state) {
        case kAiHeloLand: return "HELO_LAND";
        case kAiHeloFollowWp: return "HELO_FOLLOWWP";
        case kAiHeloCombat: return "HELO_COMBAT";
        case kAiHeloHunt: return "HELO_HUNT";
        case kAiHeloEvade: return "HELO_EVADE";
        case kAiHeloFormation: return "HELO_FORMATION";
        case kAiHeloReturnToBase: return "HELO_RETURNTOBASE";
        case kAiHeloPretty: return "HELO_PRETTY";
        case kAiHeloDead: return "HELO_DEAD";
        case kAiGroundFollowWp: return "GROUND_FOLLOWWP";
        case kAiGroundCombat: return "GROUND_COMBAT";
        case kAiGroundEvade: return "GROUND_EVADE";
        case kAiGroundFormation: return "GROUND_FORMATION";
        case kAiGroundReturnToBase: return "GROUND_RETURNTOBASE";
        case kAiGroundPretty: return "GROUND_PRETTY";
        case kAiGroundDead: return "GROUND_DEAD";
        default: return "?";
    }
}

// ----------------------------------------------------------------------------
// Handlers. Trivial ones are byte-exact ports; complex per-state behaviors route
// to h_not_yet_ported (a visible coverage stub) pending their port phase.
// ----------------------------------------------------------------------------
namespace {

void h_noop(AiThinkCtx &) {} // [orig: nullsub_69/70/71]

void h_not_yet_ported(AiThinkCtx &ctx) { ++ctx.sys->unported_calls; }

// [orig: AI_SetStateIdle @0x457f00] brain+28 (=f[7] move step) = 16.
void h_set_state_idle(AiThinkCtx &ctx) { ctx.self->brain.f[AiBrain::kStep] = 16; }

// [orig: AI_ResetToIdle @0x4578e0] step=16; slot+136 byte=0; alert+prev=0.
void h_reset_to_idle(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.brain.f[AiBrain::kStep] = 16;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
}

// [orig: AI_ResetToPatrol @0x457a70] step=64; slot+136 byte=0; alert+prev=0.
void h_reset_to_patrol(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.brain.f[AiBrain::kStep] = 64;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
}

// [orig: AI_FullResetToIdle @0x457f20] same as reset-to-idle (null-safe in orig).
void h_full_reset_to_idle(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
    e.brain.f[AiBrain::kStep] = 16;
}

// [orig: AI_FullResetToPatrol @0x458090] step=64.
void h_full_reset_to_patrol(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
    e.brain.f[AiBrain::kStep] = 64;
}

// [orig: AI_ClearTargetRef @0x457890] brain+508 (=f[127]) = 0.
void h_clear_target_ref(AiThinkCtx &ctx) { ctx.self->brain.f[AiBrain::kTargetRef] = 0; }

// [orig: AI_ClearTargetAndBoneFlag @0x4578b0] target ref=0; bone flag byte+784=0.
void h_clear_target_and_bone_flag(AiThinkCtx &ctx) {
    AiBrain &b = ctx.self->brain;
    b.f[AiBrain::kTargetRef] = 0;
    reinterpret_cast<uint8_t *>(b.f)[784] = 0;
}

// [orig: AI_ClearBoneFlag @0x457ee0] bone flag byte+784=0.
void h_clear_bone_flag(AiThinkCtx &ctx) {
    reinterpret_cast<uint8_t *>(ctx.self->brain.f)[784] = 0;
}

// [orig: AI_HandleAlertEvent @0x457a30] event handler: type==4 -> pending state=15.
void h_handle_alert_event(AiThinkCtx &ctx) {
    if (ctx.event && ctx.event->type() == 4)
        ctx.self->brain.set_pend(15);
}

// [orig: AI_HandleEvent_HelicopterCombatD @0x467730] state-16 GROUND_FOLLOWWP tick.
// (The kong name is cross-wired; the address is ground truth — this is the GROUND
// follow-waypoint behavior.) On death: queue a crash (3) or still (4) death event by
// horizontal speed. Alive: acquire a target (P2 stub -> none), tick the fire timer,
// then either engage (P2) or walk the waypoint path (P1 core).
void h_ground_followwp_tick(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;

    if (e.health <= 0) { // [orig: *(int16*)(entity+286) <= 0]
        ctx.sys->queue_death_event(e);
        return;
    }

    // Target acquisition is skipped when profile+100 & 2 (use-fallback). [orig: 0x46775c]
    AiTarget tgt{};
    bool has_target =
        ((e.profile.flags100 & 2) != 0) ? false : ctx.sys->acquire_target(*ctx.world, e, tgt);

    if ((e.profile.flags96 & 0x10) != 0) { // can-fire
        if (b.f[AiBrain::kFireTimer] <= 0)
            b.f[AiBrain::kFireTimer] = 0;
        else
            ++ctx.sys->unported_calls; // [orig: AIEntity_ReleaseFlareCountermeasures @0x455ef0] -> flare countermeasures (unported)
    }
    int32_t ft = b.f[AiBrain::kFireTimer];
    if (ft <= 0)
        b.f[AiBrain::kFireTimer] = 0;
    else
        b.f[AiBrain::kFireTimer] = ft - b.f[AiBrain::kStep];

    if (has_target)
        ctx.sys->engage_target(*ctx.world, e, tgt); // [orig: rel quads + SetAITarget + pending=17]
    else
        ctx.sys->update_waypoint_movement(e, *ctx.world); // [orig: AI_UpdateWaypointMovement @0x457bd0]
}

// [orig: AI_UpdatePatrolBehavior @0x457d70] state-18 GROUND_EVADE tick. On death: queue a
// crash/still event. Alive: tick the fire timer, then either clear the patrol goal on arrival
// (heading within def+2340 of the working heading) or decide the next state (fallback / engage).
void h_patrol_tick(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;

    if (e.health <= 0) { // [orig: *(int16*)(entity+286) <= 0]
        ctx.sys->queue_death_event(e);
        return;
    }

    if ((e.profile.flags96 & 0x10) != 0) { // can-fire
        if (b.f[AiBrain::kFireTimer] <= 0)
            b.f[AiBrain::kFireTimer] = 0;
        else
            ++ctx.sys->unported_calls; // [orig: AIEntity_ReleaseFlareCountermeasures @0x455ef0] -> flare countermeasures (unported)
    }
    int32_t ft = b.f[AiBrain::kFireTimer];
    if (ft <= 0)
        b.f[AiBrain::kFireTimer] = 0;
    else
        b.f[AiBrain::kFireTimer] = ft - b.f[AiBrain::kStep]; // -= step [brain[9] -= brain[7]]

    if (e.patrol_goal != 0) { // [orig: *(scheduler+8)] still en route to the patrol goal
        int32_t prox = e.arrival_prox;          // [orig: *(entity+32 def +2340)]
        int32_t target_h = b.f[AiBrain::kWorkHeading]; // brain[132] working heading
        int32_t h = e.heading;                  // entity+16
        if (h < target_h + prox && h > target_h - prox)
            e.patrol_goal = 0;                  // arrived -> clear the goal
    } else if ((e.profile.flags100 & 2) != 0) {
        b.set_pend(b.f[AiBrain::kFallback]);    // [orig: brain[5] = brain[6]] use fallback state
    } else {
        b.set_pend(kAiGroundCombat);            // [orig: brain[5] = 17] -> GROUND_COMBAT
    }
}

// [orig: AI_HandleEvent_HelicopterCombatB/A @0x4679f0 / @0x4676a0 / @0x4675c0] the shared GROUND
// combat event handler (states 16/17/18 event column). AI_HandleCommand runs first (deferred for
// events 1/3/4 it returns 0); then: damage(1) -> pending 18 (guarded), death(3) -> 21, destroy(4)
// -> 23. The damage event stashes its extra into brain[39].
void h_combat_event(AiThinkCtx &ctx) {
    if (!ctx.event) return; // [orig: if !brain return 1 — brain is always live in this path]
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    if (ctx.sys->ai_handle_command(e, *ctx.event)) return; // [orig: AI_HandleCommand @0x465770]
    switch (ctx.event->type()) {                            // [orig: switch(*event)]
        case 1: // damage
            b.f[AiBrain::kDamageInfo] = ctx.event->f[3];    // [orig: ai_data[39] = event[3]]
            if (b.f[AiBrain::kPendState] != 21 && b.f[AiBrain::kCurState] != 21 &&
                (e.profile.flags96 & 2) == 0)
                b.set_pend(18);                             // GROUND_EVADE
            break;
        case 3: b.set_pend(21); break; // death  -> transition state 21
        case 4: b.set_pend(23); break; // destroy -> GROUND_DEAD
        default: break;
    }
}

// [orig: AI_EnterState_GroundCombat @0x467650] state-17 enter: AiSlot+136 = 2, brain
// alert cur/pend = 2, TriggerGroup_SetAlertRed(commandGroup) [orig: @0x40d630], ally
// wake at 100 u (0x640000), brain moveStep = 1. (world-wac-ai-re §16.1)
void h_enter_ground_combat(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    b.f[AiBrain::kAlert] = 2;
    b.f[AiBrain::kPrevAlert] = 2;
    if (ctx.world != nullptr) {
        if (const Entity *se = ctx.world->registry.get(e.handle))
            ctx.world->relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000); // sets slot+136 = 2 too
    } else {
        e.slot.bytes()[AiSlot::kMoveFlagByte] = 2;
    }
    b.f[AiBrain::kStep] = 1;
}

// [orig: AI_EnterState_GroundEvade @0x467400] state-18 enter: the same alert block, then
// route by def class flags (profile+96): bit0 organic -> pending 17 when brain[38] holds a
// target else 16, TAIL-CALLING that state's enter (apply_transition re-reads kPendState
// after enter, so the re-route commits — the witnessed off_815238[4*state] tail call);
// bit2 tracked -> 17; else wheeled-alive -> the flee waypoint (controller triple
// {1, 0x7FFFFFFF, 1}, freeze work transform, moveStep 16); dead -> death event 3|4.
void h_enter_ground_evade(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    b.f[AiBrain::kAlert] = 2;
    b.f[AiBrain::kPrevAlert] = 2;
    if (ctx.world != nullptr) {
        if (const Entity *se = ctx.world->registry.get(e.handle))
            ctx.world->relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000);
    } else {
        e.slot.bytes()[AiSlot::kMoveFlagByte] = 2;
    }
    if ((e.profile.flags96 & 1) != 0) {        // organic class
        const int32_t next = (b.f[AiBrain::kTargetSlot] != 0) ? kAiGroundCombat
                                                              : kAiGroundFollowWp;
        b.set_pend(next);
        ctx.sys->row(next).enter(ctx);          // tail-call the routed enter
        return;
    }
    if ((e.profile.flags96 & 4) != 0) {        // tracked class
        b.set_pend(kAiGroundCombat);
        ctx.sys->row(kAiGroundCombat).enter(ctx);
        return;
    }
    if (e.health > 0) {                        // wheeled, alive: flee waypoint
        e.patrol_f0 = 1;                       // [orig: controller triple {1, 0x7FFFFFFF, 1}]
        e.patrol_delta = 0x7FFFFFFF;
        e.patrol_goal = 1;
        b.f[AiBrain::kWorkPosX] = e.pos[0];    // freeze the work transform at self
        b.f[AiBrain::kWorkPosY] = e.pos[1];
        b.f[AiBrain::kWorkPosZ] = e.pos[2];
        b.f[AiBrain::kWorkHeading] = e.heading;
        b.f[AiBrain::kStep] = 16;
        return;
    }
    ctx.sys->queue_death_event(e);             // dead: crash(3)/still(4) by |vel|
}

// [orig: AIEntity_ProcessWeaponFire @0x472e00] the state-17 GROUND_COMBAT tick — the
// vehicle/emplacement fire+combat routine (full digest world-wac-ai-re §17.6). Ported
// structurally; the fire-transform solver (Entity_ComputeWeaponFireTransform_0
// @0x455b30, §17.7 item 2) is a visible stub, so no SM-layer round spawns yet — the
// D-AI-2 residual. Riflemen fire through the infantry pass instead (§17.4).
void h_ground_combat_tick(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    World &world = *ctx.world;

    if (e.health <= 0) { // [orig: the death leg — event 3/4 by horizontal speed]
        ctx.sys->queue_death_event(e);
        return;
    }

    const int32_t step = b.f[AiBrain::kStep];
    // The PACKED cooldown pair: one add advances both u16 words (+208/+210) by step,
    // low-word carry included. [orig: brain[52] += 65537 * deltaTime]
    b.f[AiBrain::kCooldownPair] = static_cast<int32_t>(
        static_cast<uint32_t>(b.f[AiBrain::kCooldownPair]) +
        0x10001u * static_cast<uint32_t>(step));
    b.f[AiBrain::kTickAccum] += step;
    // The burst window: += step while armed and <= 186, else reset. [orig: brain[181]]
    if ((e.profile.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] != 0 &&
        static_cast<uint32_t>(b.f[AiBrain::kBurstWindow]) <= 0xBAu)
        b.f[AiBrain::kBurstWindow] += step;
    else
        b.f[AiBrain::kBurstWindow] = 0;

    bool processed = false;
    if (b.f[AiBrain::kTickAccum] >= 16) { // one processed tick per accumulated 16
        b.f[AiBrain::kCombatTimer] += 16;
        b.f[AiBrain::kTickAccum] = 0;
        processed = true;
        if ((e.profile.flags96 & 0x10) != 0 && b.f[AiBrain::kFireTimer] > 0)
            ++ctx.sys->unported_calls; // the flare dispenser [orig: 0x455ef0; §16.5 item 5]
        b.f[AiBrain::kFireTimer] = std::max(0, b.f[AiBrain::kFireTimer] - 16);
        b.f[AiBrain::kFireDelay] = std::max(0, b.f[AiBrain::kFireDelay] - 16);
        b.f[AiBrain::kRetargetTimer] += 16; // the §16.3 retarget cadence incrementer
    }

    // Stationary mode (profile+100 & 0x80): fire rides the turret solver's aligned flag —
    // unported (the D-AI-2 residual), so only the give-up + bookkeeping run.
    if ((e.profile.flags100 & 0x80) != 0) {
        ++ctx.sys->unported_calls;
        if (b.f[AiBrain::kCombatTimer] > 620)
            b.set_pend(b.f[AiBrain::kFallback]); // [orig: pending = fallback past 620]
        if (processed) {
            ctx.sys->ai_set_target(world, e, EntityHandle{}); // [orig: SetAITarget(0)/tick]
            if ((e.profile.flags100 & 1) != 0) ++ctx.sys->unported_calls; // AI_UpdateMovementTarget
        }
        return;
    }

    const int32_t tgt_packed = b.f[AiBrain::kTargetSlot];
    Entity *tent = (tgt_packed != 0)
                       ? world.registry.get(EntityHandle{static_cast<uint16_t>(tgt_packed - 1)})
                       : nullptr;

    if (tent == nullptr) { // no target: sweep-search + acquire [orig: the brain[38]==0 leg]
        b.f[AiBrain::kSweepPhase] = -196608;
        b.f[AiBrain::kWorkPosX] = e.pos[0]; // [orig: brain[129]/[130] = 0 — work-relative;
        b.f[AiBrain::kWorkPosY] = e.pos[1]; //  rebase freezes the absolute work pos instead]
        if ((e.profile.flags100 & 1) != 0) {
            ctx.sys->update_waypoint_movement(e, *ctx.world); // moves while searching
        } else if ((e.profile.flags100 & 4) != 0) {
            b.f[AiBrain::kWorkHeading] = e.heading; // hold heading
        } else {
            // The turret search sweep: yaw +- ~90 deg by the no-target timer phase.
            // [orig: <=124 or >434 -> yaw + 1073741760; else yaw]
            const int32_t t = b.f[AiBrain::kCombatTimer];
            b.f[AiBrain::kWorkHeading] =
                (t <= 124 || t > 434) ? e.heading + 1073741760 : e.heading;
            b.f[AiBrain::kWorkPitch] = 0;
            b.f[AiBrain::kWorkRoll] = 0;
            b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedB];
            AiTarget found{};
            if (ctx.sys->acquire_target(world, e, found)) {
                // This site's engage: both branches jitter via the inline LCG, guarded by
                // base-delay != 0 (unlike the state-16 engage's A/B split — §17.6 NOTE).
                const Entity *se = world.registry.get(e.handle);
                const Entity *te = found.handle.valid() ? world.registry.get(found.handle)
                                                        : nullptr;
                if (se != nullptr && te != nullptr)
                    ctx.sys->apply_engage_relations(world, *se, *te); // sees + targeted quads
                ctx.sys->ai_set_target(world, e, found.handle);
                ctx.sys->target_set_calls.push_back(found.net_id);
                b.f[AiBrain::kCombatTimer] = 0;
                b.f[AiBrain::kFireDelay] = e.profile.field104;
                if (e.profile.field104 != 0)
                    b.f[AiBrain::kFireDelay] += static_cast<int32_t>(
                        static_cast<uint16_t>(ctx.sys->prng_step_a()) % 62);
                return;
            }
        }
        if (b.f[AiBrain::kCombatTimer] > 620) { // give up -> fallback state
            ctx.sys->ai_set_target(world, e, EntityHandle{});
            b.set_pend(b.f[AiBrain::kFallback]);
        }
        return;
    }

    if (tent->health <= 0) { // target dead -> clear [orig: SetAITarget(0), return]
        ctx.sys->ai_set_target(world, e, EntityHandle{});
        return;
    }

    if (!processed) return; // continuation volleys ride the solver — stubbed (D-AI-2)

    if (b.f[AiBrain::kFireDelay] != 0) { // the engage fire delay: move only
        if ((e.profile.flags100 & 1) != 0) ctx.sys->update_waypoint_movement(e, *ctx.world);
        return;
    }

    // Retarget cadence [orig: AIEntity_TryAcquireTarget @0x4716b0 — brain[42] > 248].
    if (b.f[AiBrain::kRetargetTimer] > 248) {
        b.f[AiBrain::kRetargetTimer] = 0;
        AiTarget fresh{};
        if (ctx.sys->acquire_target(world, e, fresh) && fresh.handle.valid()) {
            ctx.sys->ai_set_target(world, e, fresh.handle);
            tent = world.registry.get(fresh.handle);
            if (tent == nullptr) return;
        }
    }

    // Chase movement [orig: the mobile leg]: face the target; within the approach cap
    // chase at speedA (min range / match-speed refinements ride the solver witness);
    // beyond it for > 620 ticks, give up to the fallback state.
    const int32_t tpos[3] = {static_cast<int32_t>(tent->position.x * 65536.0f),
                             static_cast<int32_t>(tent->position.y * 65536.0f),
                             static_cast<int32_t>(tent->position.z * 65536.0f)};
    const int32_t bearing = bearing_bam(tpos[1] - e.pos[1], tpos[0] - e.pos[0]);
    if ((e.profile.flags100 & 1) != 0) {
        ctx.sys->update_waypoint_movement(e, *ctx.world);
    } else if ((e.profile.flags100 & 4) != 0) {
        b.f[AiBrain::kWorkHeading] = bearing + 2147483520; // [orig: +0x7FFFFF80 half-turn]
    } else {
        b.f[AiBrain::kWorkHeading] = bearing;
        const int32_t dist = dist3d_units(tpos, e.pos);
        const int32_t cap_units = e.profile.approach_cap >> 16;
        if (cap_units > 0 && dist > cap_units) {
            if (b.f[AiBrain::kCombatTimer] > 620) {
                b.set_pend(b.f[AiBrain::kFallback]);
                ctx.sys->ai_set_target(world, e, EntityHandle{});
                return;
            }
        } else {
            b.f[AiBrain::kCombatTimer] = 0; // in range: the give-up timer rests
        }
        b.f[AiBrain::kWorkPosX] = tpos[0];
        b.f[AiBrain::kWorkPosY] = tpos[1];
        b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
    }

    // The fire gate + weapon legs [orig: heading delta <= (profile+67|1|2)>>1, then the
    // per-weapon cooldown/ammo/transform/scatter chain] ride the fire-transform solver —
    // the visible D-AI-2 stub until Entity_ComputeWeaponFireTransform_0 is witnessed.
    const uint32_t hd = static_cast<uint32_t>(bearing - e.heading) >> 24;
    const uint32_t folded = hd >= 0x80 ? 256 - hd : hd;
    if (folded <= static_cast<uint32_t>(((e.profile.fov_secondary | 1) | 2) >> 1))
        ++ctx.sys->unported_calls;
}

// The shared alert block every death-family enter runs: alert cur/prev = 2, the command
// group goes red, allies wake at 100 u, slot move flag 2. [orig: the common head of
// @0x467650 / @0x467400 / @0x467b20 / @0x467de0]
void death_alert_block(AiThinkCtx &ctx, AiEntity &e) {
    AiBrain &b = e.brain;
    b.f[AiBrain::kAlert] = 2;
    b.f[AiBrain::kPrevAlert] = 2;
    if (ctx.world != nullptr) {
        if (const Entity *se = ctx.world->registry.get(e.handle))
            ctx.world->relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000); // sets slot+136 = 2 too
    } else {
        e.slot.bytes()[AiSlot::kMoveFlagByte] = 2;
    }
}

// Queue the DESTROY event (type 4) that lands the brain in GROUND_DEAD 23.
// [orig: the inline queue tails of @0x467b20 / @0x467cd0 — channel 0, timer 0]
void queue_destroy_event(AiThinkCtx &ctx, AiEntity &e) {
    AiEventEntry ev{};
    ev.f[0] = 4;
    ev.f[1] = (ctx.sys->index_of(e) << 16);
    ev.set_timer(0.0f);
    ctx.sys->events.queue(ev);
}

// Horizontal speed with the death-velocity saturation clamp.
// [orig: the fsqrt + flt_7C19E0 min pattern shared by every death leg]
int32_t death_speed(const AiEntity &e) {
    double sp = std::sqrt(static_cast<double>(e.vel_x) * e.vel_x +
                          static_cast<double>(e.vel_z) * e.vel_z);
    if (sp > kDeathSpeedClamp) sp = kDeathSpeedClamp;
    return static_cast<int32_t>(sp);
}

// [orig: AI_TransitionToDeath_GroundVehicle @0x467b20] state-21 (vehicle DYING) enter:
// death transforms + net notify when not yet husked (Flags&4), the def+1352 mounted-
// children kill loop, the alert block, moveStep 16, and — already slow (< 1057) — the
// immediate destroy event. The death-transform leg runs the witnessed order
// [orig: Entity_UpdateDeathTransforms @0x494660]: pose snapshot, the unitType death
// dispatch (husk swap Flags|=6 + death pieces), the death sounds/effects + kz blasts —
// entity_update_death_transforms (world/destruction.cpp). The S2C 0x26 emit stays a
// net-track stub (§24).
void h_enter_vehicle_dying(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    e.net_saved_live_pose[0] = e.pos[0]; // [orig: savedLivePose = Position @0x494684]
    e.net_saved_live_pose[1] = e.pos[1];
    e.net_saved_live_pose[2] = e.pos[2];
    if (ctx.world != nullptr) {
        if (Entity *ent = ctx.world->registry.get(e.handle)) {
            if ((ent->engine_flags & kEntityFlagHusk) == 0) // [orig: the Flags&4 gate @0x467b4e]
                entity_update_death_transforms(*ctx.world, *ent, /*silent=*/false);
        }
    }
    ++ctx.sys->unported_calls; // the def+1352 kill-mounted-children loop (def byte unparsed)
    death_alert_block(ctx, e);
    b.f[AiBrain::kStep] = 16;  // [orig: ai_data[7] = 16 @0x467c02]
    if (death_speed(e) < 1057) // [orig: @0x467c51 — stopped -> destroy now]
        queue_destroy_event(ctx, e);
}

// [orig: AI_TickState_VehicleDying @0x467cd0 (ex kong 'AI_CheckVehicleStuck')] state-21
// tick: settle the falling death physics, then once slow (< 1057) OR static since the
// death pose (|pos - savedLivePose| < 1024 per axis) queue the destroy event; while
// still moving, zero the work pitch/roll, the commanded speed, and the mover output.
void h_vehicle_dying_tick(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    ++ctx.sys->unported_calls; // [orig: Entity_ProcessFallingDeathPhysics @0x461d30]
    const bool stopped = death_speed(e) < 1057;
    const bool still =
        std::abs(e.pos[0] - e.net_saved_live_pose[0]) < 1024 &&
        std::abs(e.pos[1] - e.net_saved_live_pose[1]) < 1024 &&
        std::abs(e.pos[2] - e.net_saved_live_pose[2]) < 1024;
    if (stopped || still) {
        queue_destroy_event(ctx, e); // [orig: @0x467dcb]
    } else {
        b.f[AiBrain::kWorkPitch] = 0; // [orig: ai_data[133] @0x467d77]
        b.f[AiBrain::kWorkRoll] = 0;  // [orig: ai_data[134]]
        b.f[136] = 0;                 // [orig: ai_data[136] — the commanded speed +544]
        b.f[AiBrain::kOutSpeed] = 0;  // [orig: ai_data[128]]
    }
}

// [orig: AI_HandleEvent_VehicleDying @0x457f50 (ex kong 'AI_HandleRetreatEvent')]
// state-21 event: only the destroy event (4) lands -> pending GROUND_DEAD 23.
void h_vehicle_dying_event(AiThinkCtx &ctx) {
    if (ctx.event == nullptr) return;
    if (ctx.event->type() != 4) return;
    ctx.self->brain.set_pend(23);
}

// [orig: AI_TransitionToDestroyed_Vehicle @0x467de0] state-23 (GROUND_DEAD) enter:
// the alert block, clear the target word + aim byte, the death transforms when not
// yet husked, stamp the death tick (entity+428, first write wins — informational, not
// modeled), clear every entity reference incl. the AI target's +530 refcount, and
// moveStep 62.
void h_enter_vehicle_dead(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    death_alert_block(ctx, e);
    if (ctx.world != nullptr) {
        if (Entity *ent = ctx.world->registry.get(e.handle)) {
            if ((ent->engine_flags & kEntityFlagHusk) == 0) // [orig: the Flags&4 gate]
                entity_update_death_transforms(*ctx.world, *ent, /*silent=*/false);
            ent->corpse_timer = 0; // [orig: entity+328 = 0 @0x467e22 — wrecks never expire]
            ent->team = 0;         // [orig: entity+354 = 0 @0x467e2c — a wreck goes teamless
                                   //  and drops out of ordinary target scans]
            if (ent->death_tick == 0) // [orig: +0x1AC first write wins @0x467e4a]
                ent->death_tick = ctx.world->logic_tick;
        }
        if (b.f[AiBrain::kTargetSlot] != 0)
            ctx.sys->ai_set_target(*ctx.world, e, EntityHandle{}); // [orig: @0x467e86]
    }
    e.team = 0; // the motor-side copy the candidate scan reads
    b.f[AiBrain::kStep] = 62; // [orig: ai_data[7] = 62 @0x467e8e]
}

// [orig: AI_TickState_VehicleDead @0x467ea0 (ex kong 'Entity_ProcessMountedInfantryFrame')]
// state-23 tick: keep settling; the itemDef attrib&0x40 authority RESPAWN watcher
// (cooldown countdown -> teleport to the spawn pose / bury + Entity_RespawnVehicle) is
// a visible stub — mission vehicles without the attrib settle forever, the witnessed
// default.
void h_vehicle_dead_tick(AiThinkCtx &ctx) {
    ++ctx.sys->unported_calls; // [orig: Entity_ProcessFallingDeathPhysics + the respawn leg]
}

// [orig: AI_HandleEvent_ConsumeAll @0x458080] state-23 event: swallow everything —
// dead entities ignore commands, damage, and further death events.
void h_vehicle_dead_event(AiThinkCtx &) {}

// The 24-state dispatch table, mirroring off_815238/3C/40/44 @0x815238.
// Columns: {enter, tick, exit, event}. U = not-yet-ported (visible stub), _ = noop.
// State 17/18 rows ported 2026-07-16 (world-wac-ai-re §16.1/§17.6:
// enter17 = AI_EnterState_GroundCombat @0x467650, tick17 = AIEntity_ProcessWeaponFire @0x472e00,
// enter18 = AI_EnterState_GroundEvade @0x467400, event18 = AI_HandleEvent_GroundEvade @0x4675c0
// = the shared h_combat_event family).
// State 21/23 rows (the vehicle death chain) ported 2026-07-16 (world-wac-ai-re §19:
// enter21 = AI_TransitionToDeath_GroundVehicle @0x467b20, tick21 = AI_TickState_VehicleDying
// @0x467cd0, event21 = AI_HandleEvent_VehicleDying @0x457f50, enter23 =
// AI_TransitionToDestroyed_Vehicle @0x467de0, tick23 = AI_TickState_VehicleDead @0x467ea0,
// event23 = AI_HandleEvent_ConsumeAll @0x458080; the shared exit column is nullsub_70).
constexpr AiHandler U = h_not_yet_ported;
constexpr AiHandler _ = h_noop;

const StateRow kTable[kAiStateCount] = {
    /* 0  */ {_, _, _, _},
    /* 1  */ {_, _, _, _},
    /* 2  */ {_, _, _, _},
    /* 3  */ {_, _, _, _},
    /* 4  */ {_, _, _, _},
    /* 5  */ {_, _, _, _},
    /* 6  HELO_LAND        */ {U, U, _, U},
    /* 7  HELO_FOLLOWWP    */ {U, U, _, U},
    /* 8  HELO_COMBAT      */ {U, U, h_clear_target_and_bone_flag, U},
    /* 9                   */ {_, _, _, _},
    /* 10 HELO_EVADE       */ {U, U, h_clear_target_ref, U},
    /* 11 HELO_FORMATION   */ {h_reset_to_idle, U, _, U},
    /* 12                  */ {_, _, _, _},
    /* 13 (transition)     */ {U, U, _, h_handle_alert_event},
    /* 14 HELO_PRETTY      */ {h_reset_to_patrol, U, _, U},
    /* 15 HELO_DEAD        */ {U, U, _, U},
    /* 16 GROUND_FOLLOWWP  */ {h_set_state_idle, h_ground_followwp_tick, _, h_combat_event},
    /* 17 GROUND_COMBAT    */ {h_enter_ground_combat, h_ground_combat_tick, h_clear_bone_flag,
                               h_combat_event},
    /* 18 GROUND_EVADE     */ {h_enter_ground_evade, h_patrol_tick, _, h_combat_event},
    /* 19 GROUND_FORMATION */ {h_full_reset_to_idle, U, _, U},
    /* 20 GROUND_RETURNTOBASE */ {_, _, _, _},
    /* 21 GROUND_DYING     */ {h_enter_vehicle_dying, h_vehicle_dying_tick, _,
                               h_vehicle_dying_event},
    /* 22 GROUND_PRETTY    */ {h_full_reset_to_patrol, U, _, U},
    /* 23 GROUND_DEAD      */ {h_enter_vehicle_dead, h_vehicle_dead_tick, _,
                               h_vehicle_dead_event},
};

const StateRow kDefaultRow = {h_noop, h_noop, h_noop, h_noop};

} // namespace

const StateRow &AiSystem::row(int32_t state) const {
    if (state < 0 || state >= kAiStateCount) return kDefaultRow;
    return kTable[state];
}

} // namespace opennova::world
