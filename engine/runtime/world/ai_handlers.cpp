#include <runtime/world/ai.h>

// Target scoring, the AI state-name table, and one handler per AI state — plus the
// dispatch table itself, which is why AiSystem::row is defined here.

#include <runtime/world/angle.h>
#include <runtime/world/ground_conform.h>
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include <runtime/world/world.h>

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

void h_not_yet_ported(AiThinkCtx &ctx) {
    ++ctx.sys->unported_calls;
    if (ctx.world != nullptr)
        ctx.world->diagnostics.record({RuntimeGapKind::AiStateHandler,
                ctx.self->brain.f[AiBrain::kCurState], 0, -1, int32_t(ctx.self->handle.packed)}, ctx.world->logic_tick);
}

// [orig: AI_SetStateIdle @0x457f00] brain+28 (=f[7] move step) = 16.
void h_set_state_idle(AiThinkCtx &ctx) { ctx.self->brain.f[AiBrain::kStep] = 16; }

// [orig: AI_ResetToIdle @0x4578e0] step=16; slot+136 byte=0; alert+prev=0.
void h_reset_to_idle(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.brain.f[AiBrain::kStep] = 16;
    e.slot.bytes()[AiSlot::kAlertByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
}

// [orig: AI_ResetToPatrol @0x457a70] step=64; slot+136 byte=0; alert+prev=0.
void h_reset_to_patrol(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.brain.f[AiBrain::kStep] = 64;
    e.slot.bytes()[AiSlot::kAlertByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
}

// [orig: AI_FullResetToIdle @0x457f20] same as reset-to-idle (null-safe in orig).
void h_full_reset_to_idle(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.slot.bytes()[AiSlot::kAlertByte] = 0;
    e.brain.f[AiBrain::kPrevAlert] = 0;
    e.brain.f[AiBrain::kAlert] = 0;
    e.brain.f[AiBrain::kStep] = 16;
}

// [orig: AI_FullResetToPatrol @0x458090] step=64.
void h_full_reset_to_patrol(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    e.slot.bytes()[AiSlot::kAlertByte] = 0;
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
		else if (Entity *vehicle = ctx.world->registry.get(e.handle))
			ctx.world->vehicles.release_flares(*vehicle);
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
		else if (Entity *vehicle = ctx.world->registry.get(e.handle))
			ctx.world->vehicles.release_flares(*vehicle);
	}
	int32_t ft = b.f[AiBrain::kFireTimer];
    if (ft <= 0)
        b.f[AiBrain::kFireTimer] = 0;
    else
        b.f[AiBrain::kFireTimer] = ft - b.f[AiBrain::kStep]; // -= step [brain[9] -= brain[7]]

    if (e.patrol_goal != 0) { // [orig: *(scheduler+8)] still en route to the patrol goal
        // The arrival proximity is the entity def's turn-rate word (itemDef+0x924,
        // the word the ground and tank movers read as turnRate), compared with
        // brain[132] — the mover's last steer target, not a state-machine copy
        // (vehicle_system mirrors it back after the mover).
        // [orig: AI_UpdatePatrolBehavior `mov ecx,[edi+20h]; mov eax,[ecx+924h];
        //  mov esi,[esi+210h]` @0x457DCB..0x457DD4]
        int32_t prox = 0;
        if (ctx.world != nullptr)
            if (const Entity *ent = ctx.world->registry.get(e.handle))
                if (const VehicleTraits *t = ctx.world->vehicles.traits.get(ent->item_id))
                    prox = t->turn_rate;
        int32_t target_h = b.f[AiBrain::kWorkHeading]; // brain[132] working heading
        int32_t h = e.heading;                  // entity+16
        // Signed compares against the wrapping `lea`/`sub` bounds
        // [orig: @0x457DDD..0x457DEC].
        if (h < io::bam_add(target_h, prox) && h > io::bam_sub(target_h, prox))
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
    if (ctx.sys->ai_handle_command(*ctx.world, e, *ctx.event)) return; // [orig: AI_HandleCommand @0x465770]
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
            ctx.world->script.relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000); // sets slot+136 = 2 too
    } else {
        e.slot.bytes()[AiSlot::kAlertByte] = 2;
    }
    b.f[AiBrain::kStep] = 1;
}

// [orig: AI_EnterState_GroundEvade @0x467400] state-18 enter: the same alert block, then
// route by the profile's EVADE_FLAGS (profile+96, aip.h token names; `test al,1`
// @0x467452, `test al,4` @0x467488): FOLLOW_WP (0x1) -> pending 17 when brain[38] holds
// a target else 16, TAIL-CALLING that state's enter (apply_transition re-reads kPendState
// after enter, so the re-route commits — the witnessed off_815238[4*state] tail call);
// FLEE (0x4) -> 17; else alive -> the flee waypoint (controller triple
// {1, 0x7FFFFFFF, 1}, freeze work transform, moveStep 16); dead -> death event 3|4.
void h_enter_ground_evade(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    b.f[AiBrain::kAlert] = 2;
    b.f[AiBrain::kPrevAlert] = 2;
    if (ctx.world != nullptr) {
        if (const Entity *se = ctx.world->registry.get(e.handle))
            ctx.world->script.relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000);
    } else {
        e.slot.bytes()[AiSlot::kAlertByte] = 2;
    }
    if ((e.profile.flags96 & 1) != 0) {        // EVADE_FLAGS FOLLOW_WP
        const int32_t next = (b.f[AiBrain::kTargetSlot] != 0) ? kAiGroundCombat
                                                              : kAiGroundFollowWp;
        b.set_pend(next);
        ctx.sys->row(next).enter(ctx);          // tail-call the routed enter
        return;
    }
    if ((e.profile.flags96 & 4) != 0) {        // EVADE_FLAGS FLEE
        b.set_pend(kAiGroundCombat);
        ctx.sys->row(kAiGroundCombat).enter(ctx);
        return;
    }
    if (e.health > 0) {                        // neither flag, alive: flee waypoint
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

// The per-site aim offset retail passes to the solver through the `distance`
// global [orig: @0x473367..0x47337E, repeated at every mobile/continuation
// site — ATEAM (0x20) rides the sweep phase, ATEAM_LOCK (0x40) a fixed -3.0u,
// else 0].
static int32_t sm_aim_offset(const AiEntity &e) {
    if ((e.profile.flags100 & 0x20) != 0) return e.brain.f[AiBrain::kSweepPhase];
    if ((e.profile.flags100 & 0x40) != 0) return static_cast<int32_t>(0xFFFD0000u);
    return 0;
}

// One SM weapon chain — the per-weapon block repeated at the eight
// AIEntity_ProcessWeaponFire solver sites [orig: stationary @0x472F6E/0x472FF8,
// mobile @0x4733A4/0x473488/0x4735C9/0x47386B, continuation @0x473D57/0x473E44]:
// ammo + cooldown gate, solve, the two-draw scatter (mobile/continuation only),
// fire through the authoritative round path, then ammo--/cooldown-reset/
// last-weapon/bone-flag bookkeeping (§17.6).
static bool sm_weapon_fire(AiThinkCtx &ctx, AiEntity &e, int which, const Entity *tent,
                           int32_t aim_offset, bool skip_los, bool scatter) {
    AiBrain &b = e.brain;
    World &world = *ctx.world;
    const AiProfile::WeaponFire &wb = (which == 1) ? e.profile.fire_a : e.profile.fire_b;
    const int32_t interval =
            (which == 1) ? e.profile.fire_interval_a : e.profile.fire_interval_b;
    const int ammo_idx = (which == 1) ? AiBrain::kAmmoA : AiBrain::kAmmoB;
    if (wb.ammo_index < 0) return false; // unarmed block (no "*_weap" resolved)
    if (b.f[ammo_idx] == 0) return false; // [orig: the ammo dword gate @0x472F7E]
    // The packed cooldown words: low u16 = primary (+208), high = secondary
    // (+210), vs the .aip rate [orig: @0x472F0B cmp word +208, profile+124].
    const uint32_t pair = static_cast<uint32_t>(b.f[AiBrain::kCooldownPair]);
    const uint32_t cd = (which == 1) ? (pair & 0xFFFFu) : (pair >> 16);
    if (static_cast<int32_t>(cd) < interval) return false;

    int32_t out[6];
    if (!ctx.sys->solve_weapon_fire_transform(world, e, tent, wb, aim_offset, skip_los, out))
        return false;

    if (scatter) {
        // Saved continuation deltas, captured before the scatter draws
        // [orig: @0x4735F9..0x47363A — out minus entity pos/angles; primary
        // +0x2D8, secondary +0x2F0].
        const int base = (which == 1) ? AiBrain::kSavedDeltaA : AiBrain::kSavedDeltaB;
        b.f[base + 0] = out[0] - e.pos[0];
        b.f[base + 1] = out[1] - e.pos[1];
        b.f[base + 2] = out[2] - e.pos[2];
        b.f[base + 3] = out[3] - e.heading;
        b.f[base + 4] = out[4];
        b.f[base + 5] = out[5];
        // Two draws of the shared rotate-LCG, yaw then pitch: mag = (u16 %
        // (6 - brain[43])) scaled by 8947848.0f (~0.75 deg BAM); odd scaled
        // value adds, even subtracts [orig: @0x473640..0x473716 —
        // dword_31BFBB8 rol-LCG, flt_7C6F60, the &0x80000001 parity fold].
        const int32_t mod = 6 - b.f[AiBrain::kAccuracy];
        for (int angle = 3; angle <= 4 && mod > 0; ++angle) {
            const uint16_t draw = static_cast<uint16_t>(ctx.sys->prng_step_a());
            const int32_t scaled = static_cast<int32_t>(
                    static_cast<double>(static_cast<int32_t>(draw) % mod) * 8947848.0f);
            if ((scaled & 1) != 0)
                out[angle] += scaled;
            else
                out[angle] -= scaled;
        }
    }

    if (!ctx.sys->fire_ai_round(world, e, out, out[3], out[4], wb.ammo_index))
        return false;

    // Fire bookkeeping [orig: §17.6 — ammo--, cooldown word = 0, brain[106] =
    // which, bone-flag |= 0x40 @0x47306F; the burst window arms on fire].
    if (b.f[ammo_idx] > 0) --b.f[ammo_idx]; // [orig: @0x473008 dec only when > 0]
    b.f[AiBrain::kCooldownPair] = static_cast<int32_t>(
            (which == 1) ? (pair & 0xFFFF0000u) : (pair & 0x0000FFFFu));
    b.f[AiBrain::kLastWeapon] = which;
    b.bytes()[AiBrain::kBoneFlagByte] |= 0x40;
    if ((e.profile.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] == 0)
        b.f[AiBrain::kBurstWindow] = 1;
    return true;
}

// [orig: AIEntity_ProcessWeaponFire @0x472e00] the state-17 GROUND_COMBAT tick — the
// vehicle/emplacement fire+combat routine (full digest world-wac-ai-re §17.6). The
// fire-transform solver is ported (solve_weapon_fire_transform — Entity_
// ComputeWeaponFireTransform_0 @0x456980), so SM vehicles and emplacements spawn
// rounds through the same authoritative path as infantry. Riflemen still fire
// through the infantry pass (§17.4).
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
			if (Entity *vehicle = ctx.world->registry.get(e.handle))
				ctx.world->vehicles.release_flares(*vehicle);
		b.f[AiBrain::kFireTimer] = std::max(0, b.f[AiBrain::kFireTimer] - 16);
		b.f[AiBrain::kFireDelay] = std::max(0, b.f[AiBrain::kFireDelay] - 16);
        b.f[AiBrain::kRetargetTimer] += 16; // the §16.3 retarget cadence incrementer
    }

    const int32_t tgt_packed = b.f[AiBrain::kTargetSlot];
    Entity *tent = (tgt_packed != 0)
                       ? world.registry.get(EntityHandle{static_cast<uint16_t>(tgt_packed - 1)})
                       : nullptr;

    // Stationary mode (RC_FIRE, profile+100 & 0x80): fire only while the
    // commanded weapons-free byte is set [orig: @0x472EDE cmp brain+0x311 —
    // written by the WAC AI command, case 0x15]. The per-processed-tick
    // SetAITarget(0) below means the solver usually holds no target here, so
    // in practice only the WEAPON_PITCHLOCKED* legs fire (artillery at the
    // commanded elevation). No scatter on this path (the LCG draws appear only
    // at the mobile/continuation sites).
    if ((e.profile.flags100 & 0x80) != 0) {
        if (b.bytes()[AiBrain::kGuardFireByte] != 0) {
            if (!sm_weapon_fire(ctx, e, 1, tent, 0, false, false) &&
                !sm_weapon_fire(ctx, e, 2, tent, 0, false, false))
                b.bytes()[AiBrain::kBoneFlagByte] = 0; // both held [orig: @0x47308B]
        }
        if (b.f[AiBrain::kCombatTimer] > 620)
            b.set_pend(b.f[AiBrain::kFallback]); // [orig: pending = fallback past 620]
        if (processed) {
            ctx.sys->ai_set_target(world, e, EntityHandle{}); // [orig: SetAITarget(0)/tick]
			if ((e.profile.flags100 & 1) != 0)
				ctx.sys->update_waypoint_movement(e, world);
		}
		return;
    }

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

    if (!processed) {
        // Continuation volleys between processed ticks: the last-fired weapon
        // keeps the volley running, cooldown-gated, re-solved WITH LOS (those
        // sites pass arg7 = 0) and with a fresh aim offset [orig: the
        // @0x473D57 (primary) / @0x473E44 (secondary) sites; brain[106] routes
        // which]. Only sub-16-tick rates ever pass the cooldown gate here.
        const int32_t which = b.f[AiBrain::kLastWeapon];
        if (which == 1 || which == 2)
            (void)sm_weapon_fire(ctx, e, static_cast<int>(which), tent, sm_aim_offset(e),
                                 /*skip_los=*/false, /*scatter=*/true);
        return;
    }

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

    // The sweep advance (ATEAM, per processed tick with a live target): phase
    // += 10918 until +3.0u, then reset to -3.0u and drop the target
    // [orig: §17.6 sweep — brain[180] += 10918, > 196608 -> -196608 + clear].
    if ((e.profile.flags100 & 0x20) != 0) {
        b.f[AiBrain::kSweepPhase] += 10918;
        if (b.f[AiBrain::kSweepPhase] > 196608) {
            b.f[AiBrain::kSweepPhase] = -196608;
            ctx.sys->ai_set_target(world, e, EntityHandle{});
            return;
        }
    }

    // The fire gate + weapon legs [orig: heading delta <= (profile+67|1|2)>>1
    // folded to 1/256 turns, then the per-weapon ammo/cooldown/solve/scatter
    // chain — the mobile sites solve with LOS deferred (arg7 = 1)].
    const uint32_t hd = static_cast<uint32_t>(bearing - e.heading) >> 24;
    const uint32_t folded = hd >= 0x80 ? 256 - hd : hd;
    if (folded <= static_cast<uint32_t>(((e.profile.fov_secondary | 1) | 2) >> 1)) {
        if (!sm_weapon_fire(ctx, e, 1, tent, sm_aim_offset(e), /*skip_los=*/true,
                            /*scatter=*/true) &&
            !sm_weapon_fire(ctx, e, 2, tent, sm_aim_offset(e), /*skip_los=*/true,
                            /*scatter=*/true))
            b.bytes()[AiBrain::kBoneFlagByte] = 0; // both held [orig: @0x473402]
    }
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
            ctx.world->script.relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000); // sets slot+136 = 2 too
    } else {
        e.slot.bytes()[AiSlot::kAlertByte] = 2;
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
// entity_update_death_transforms (world/destruction.cpp). An in-session authority then
// sends the S2C 0x26 kill record (section 0); vehicles never use the organic 0x13.
void h_enter_vehicle_dying(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    e.net_saved_live_pose[0] = e.pos[0]; // [orig: savedLivePose = Position @0x494684]
    e.net_saved_live_pose[1] = e.pos[1];
    e.net_saved_live_pose[2] = e.pos[2];
    if (ctx.world != nullptr) {
        if (Entity *ent = ctx.world->registry.get(e.handle)) {
            if ((ent->engine_flags & kEntityFlagHusk) == 0) { // [orig: AI_TransitionToDeath_GroundVehicle Flags&4 gate @0x467b32]
                entity_update_death_transforms(*ctx.world, *ent, /*silent=*/false);
                // [orig: AI_TransitionToDeath_GroundVehicle @0x467B43..0x467B58 ->
                //  Server_SendEntityStatePacket(entity, 0)]
                emit_item_state(*ctx.world, *ent, 0);
            }
        }
    }
	// The `Parent` (ItemDef+0x548) gate over the brain's +576/+580 gunner-attachment
	// list the class init built (VehicleSystem::setup_gunner_attachments): every
	// live child's health word is zeroed and its class death callback runs with
	// phase 1, on clients too [orig: AI_TransitionToDeath_GroundVehicle
	// @0x467B6E (the +1352 byte) .. @0x467BBB (child deathCallback(child, 1, 0));
	// the +286 > 0 gate @0x467B9A, the hit-record attacker clear @0x467BAD].
	if (ctx.world != nullptr) {
		World &world = *ctx.world;
		const Entity *parent = world.registry.get(e.handle);
		const VehicleTraits *traits =
				parent != nullptr ? world.vehicles.traits.get(parent->item_id) : nullptr;
		if (traits != nullptr && traits->attrib_parent) {
			const int32_t count =
					std::min<int32_t>(b.f[AiBrain::kAttachCount], AiBrain::kAttachSlotMax);
			for (int slot = 0; slot < count; ++slot) {
				const int32_t word = b.f[AiBrain::kAttachSlots + 2 * slot + 1];
				if (word <= 0) continue;
				Entity *child = world.registry.get(EntityHandle{ static_cast<uint16_t>(word - 1) });
				if (child == nullptr || child->health <= 0) continue;
				child->health = 0;
				child->last_attacker = {};
				if (AiEntity *brain = world.ai.for_handle(child->handle))
					brain->health = 0;
				destruction_notify_item_damage(world, *child, 1);
			}
		}
	}
	// The addeweap emplacement children (items.def addeweap*, a different list
	// from the refNum peers above) keep the earlier stand-in kill until the
	// world.cpp orphan cascade alone carries them (destruction_test pins it).
	if (ctx.world != nullptr) {
		World &world = *ctx.world;
		const Entity *parent = world.registry.get(e.handle);
		if (parent != nullptr) {
			std::vector<EntityHandle> children;
			world.registry.for_each_in_pool(1, [&](const Entity &child) {
				if (child.emplacement_parent == e.handle &&
						child.emplacement_parent_spawn_id == parent->registry_spawn_id &&
						child.health > 0)
					children.push_back(child.handle);
			});
			for (EntityHandle handle : children) {
				Entity *child = world.registry.get(handle);
				if (child == nullptr)
					continue;
				child->health = 0;
				child->last_attacker = {};
				if (AiEntity *brain = world.ai.for_handle(handle))
					brain->health = 0;
				destruction_notify_item_damage(world, *child, 1);
			}
		}
	}
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
	if (ctx.world != nullptr) {
		if (Entity *vehicle = ctx.world->registry.get(e.handle))
			entity_process_falling_death(*ctx.world, *vehicle, ctx.world->tables.terrain,
					float(from_fixed(ctx.world->env.water_z)));
	}
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
        // brain+0x220 IS the mover's commanded-speed register (the wire's
        // forward-speed word): the port's split keeps the motor copy in step.
        // [orig: `mov [esi+220h],ebx` @0x467D83]
        b.f[136] = 0;
        if (ctx.world != nullptr)
            if (Entity *vehicle = ctx.world->registry.get(e.handle))
                vehicle->veh.cmd_speed = 0;
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
            if ((ent->engine_flags & kEntityFlagHusk) == 0) { // [orig: the Flags&4 gate]
                entity_update_death_transforms(*ctx.world, *ent, /*silent=*/false);
                // [orig: AI_TransitionToDestroyed_Vehicle @0x467E40..0x467E55 ->
                //  Server_SendEntityStatePacket(entity, 0)]
                emit_item_state(*ctx.world, *ent, 0);
            }
			ent->veh.stuck_ticks = 0; // [orig: moveTimer +0x148 = 0 @0x467E22]
			ent->team = 0; // [orig: entity+354 = 0 @0x467e2c — a wreck goes teamless
						   //  and drops out of ordinary target scans]
			if (ent->death_tick == 0) // [orig: +0x1AC first write wins @0x467e4a]
                ent->death_tick = ctx.world->logic_tick;
        }
		ctx.sys->clear_entity_references(*ctx.world, e.handle);
		if (b.f[AiBrain::kTargetSlot] != 0)
			ctx.sys->ai_set_target(*ctx.world, e, EntityHandle{}); // [orig: @0x467e86]
    }
    e.team = 0; // the motor-side copy the candidate scan reads
    b.f[AiBrain::kStep] = 62; // [orig: ai_data[7] = 62 @0x467e8e]
}

// [orig: AI_TickState_VehicleDead @0x467EA0]
void h_vehicle_dead_tick(AiThinkCtx &ctx) {
	if (ctx.world != nullptr) {
		if (Entity *vehicle = ctx.world->registry.get(ctx.self->handle))
			ctx.world->vehicles.tick_dead(*vehicle, *ctx.self);
	}
}

// [orig: AI_HandleEvent_ConsumeAll @0x458080] state-23 event: swallow everything —
// dead entities ignore commands, damage, and further death events.
void h_vehicle_dead_event(AiThinkCtx &) {}

// Ground clearance for the aircraft death states: lift 1, search down 48,
// then the intact/dead model's authored height offset.
// [orig: @0x4668DA..0x46693C; @0x45791F..0x457972]
int32_t aircraft_death_ground(World &world, Entity &entity) {
	const int32_t pos[3] = { to_fixed(entity.position.x), to_fixed(entity.position.y),
		to_fixed(entity.position.z) };
	int32_t ground = INT32_MIN;
	if (world.ai.collision != nullptr)
		ground = world.ai.collision->raycast_ground(
				world, entity.handle, pos, 0, 0, 0x10000, 0x300000, &entity.ground_target);
	else if (world.tables.terrain != nullptr)
		ground = calc_average_ground_height(*world.tables.terrain, pos, 0, GroundClearance{});
	if (entity.primary_occupant.valid())
		ground = std::max(ground, world.env.water_z);
	int32_t offset = entity.veh.air_probe_z_off;
	if (entity.health <= 0 || ((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0)
		if (const auto *t = world.tables.item_death_traits.get(entity.item_id))
			if (t->husk_model_loaded)
				offset = to_fixed(std::abs(t->husk_rest_min_z));
	return ground == INT32_MIN ? ground : io::bam_add(ground, offset);
}

// [orig: AI_InitDeathState @0x457690; AI_InitGroundHeight @0x4577D0]
void h_enter_aircraft_land(AiThinkCtx &ctx) {
	auto &ai = *ctx.self;
	auto &b = ai.brain;
	ai.slot.bytes()[AiSlot::kAlertByte] = 0;
	b.f[AiBrain::kPrevAlert] = b.f[AiBrain::kAlert] = 0;
	b.f[AiBrain::kStep] = 8;
	Entity *entity = ctx.world != nullptr ? ctx.world->registry.get(ai.handle) : nullptr;
	if (entity == nullptr)
		return;
	const int32_t ground = aircraft_death_ground(*ctx.world, *entity);
	b.f[138] = ai.profile.patrol_climb >> 1;
	b.f[AiBrain::kWorkPosX] = to_fixed(entity->position.x);
	b.f[AiBrain::kWorkPosY] = to_fixed(entity->position.y);
	b.f[AiBrain::kWorkPosZ] = ground;
	b.f[AiBrain::kWorkHeading] = entity->veh.yaw_seeded ? entity->veh.yaw_bam : ai.heading;
	b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
	b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kTargetRef] = 0;
	if (io::bam_sub(ground, 16384) > to_fixed(entity->position.z))
		queue_destroy_event(ctx, ai);
}
void h_aircraft_land_tick(AiThinkCtx &ctx) {
	if (ctx.world == nullptr)
		return;
	auto &ai = *ctx.self;
	auto &b = ai.brain;
	Entity *entity = ctx.world->registry.get(ai.handle);
	if (entity == nullptr)
		return;
	const int32_t ground = aircraft_death_ground(*ctx.world, *entity);
	const int32_t z = to_fixed(entity->position.z);
	b.f[AiBrain::kWorkPosZ] = ground;
	b.f[AiBrain::kOutSpeed] = 0;
	if (ground < z)
		b.f[138] = io::bam_sub(z, ground) >= 327680 ? ai.profile.patrol_climb >> 1 : 2114;
	else {
		entity->position.z = float(from_fixed(ground));
		ai.pos[2] = ground;
		b.f[138] = 0;
		b.set_pend(14);
	}
}

// [orig: AI_HandleEvent_VehicleGeneric @0x465EF0; damage siblings
// @0x466770/@0x4663D0/@0x4662A0/@0x466810]
void h_aircraft_event(AiThinkCtx &ctx) {
	if (ctx.event == nullptr || ctx.sys->ai_handle_command(*ctx.world, *ctx.self, *ctx.event))
		return;
	auto &ai = *ctx.self;
	auto &b = ai.brain;
	if (ctx.event->type() == 1) {
		b.f[AiBrain::kDamageInfo] = ctx.event->f[3];
		if (b.f[AiBrain::kCurState] != 6 && b.f[AiBrain::kCurState] != 13 &&
				b.f[AiBrain::kPendState] != 13 && (ai.profile.flags96 & 2) == 0)
			b.set_pend(10);
	} else if (ctx.event->type() == 3)
		b.set_pend(13);
	else if (ctx.event->type() == 4)
		b.set_pend(15);
}
void queue_aircraft_death(AiThinkCtx &ctx) {
	if (ctx.world == nullptr)
		return;
	Entity *entity = ctx.world->registry.get(ctx.self->handle);
	if (entity == nullptr)
		return;
	AiEventEntry event{};
	event.f[0] = aircraft_death_ground(*ctx.world, *entity) <= to_fixed(entity->position.z) ? 3 : 4;
	event.f[1] = ctx.sys->index_of(*ctx.self) << 16;
	ctx.sys->events.queue(event);
}
void aircraft_flare_timer(AiThinkCtx &ctx) {
	auto &ai = *ctx.self;
	auto &b = ai.brain;
	if ((ai.profile.flags96 & 0x10) != 0 && b.f[AiBrain::kFireTimer] > 0 && ctx.world != nullptr)
		if (Entity *entity = ctx.world->registry.get(ai.handle))
			ctx.world->vehicles.release_flares(*entity);
	b.f[AiBrain::kFireTimer] = b.f[AiBrain::kFireTimer] <= 0
			? 0
			: io::bam_sub(b.f[AiBrain::kFireTimer], b.f[AiBrain::kStep]);
}
// [orig: AI_HandleEvent_VehicleWithDamageC @0x466460; formation thunk @0x466800]
void h_aircraft_followwp_tick(AiThinkCtx &ctx) {
	if (ctx.world == nullptr)
		return;
	auto &ai = *ctx.self;
	const Entity *entity = ctx.world->registry.get(ai.handle);
	if ((entity != nullptr ? entity->health : ai.health) <= 0) {
		queue_aircraft_death(ctx);
		return;
	}
	AiTarget target{};
	const bool acquired =
			(ai.profile.flags100 & 2) == 0 && ctx.sys->acquire_target(*ctx.world, ai, target);
	aircraft_flare_timer(ctx);
	if (acquired)
		ctx.sys->engage_target(*ctx.world, ai, target, true);
	else
		ctx.sys->update_aircraft_waypoint_movement(ai, *ctx.world);
}

void h_enter_aircraft_combat(AiThinkCtx &ctx) {
	if (ctx.world != nullptr)
		ctx.sys->enter_aircraft_combat(*ctx.self, *ctx.world);
}
void h_enter_aircraft_evade(AiThinkCtx &ctx) {
	if (ctx.world != nullptr)
		ctx.sys->enter_aircraft_evade(*ctx.self, *ctx.world);
}
void h_aircraft_evade_tick(AiThinkCtx &ctx) {
	if (ctx.world != nullptr)
		ctx.sys->aircraft_evade_tick(*ctx.self, *ctx.world);
}
void h_aircraft_combat_tick(AiThinkCtx &ctx) {
	if (ctx.world == nullptr)
		return;
	const Entity *entity = ctx.world->registry.get(ctx.self->handle);
	if ((entity != nullptr ? entity->health : ctx.self->health) <= 0)
		queue_aircraft_death(ctx);
	else
		ctx.sys->aircraft_combat_tick(*ctx.self, *ctx.world);
}

// [orig: AI_TransitionToDeath_Vehicle @0x4668A0]
void h_enter_aircraft_dying(AiThinkCtx &ctx) {
	auto &ai = *ctx.self;
	if (ctx.world != nullptr) {
		World &world = *ctx.world;
		if (Entity *entity = world.registry.get(ai.handle)) {
			world.out.scars.clear_entity(entity->handle);
			const int cannon_index = world.tables.ammo.index_of("20mm");
			const bool cannon = cannon_index >= 0 && ai.slot.f[24] == cannon_index;
			if (!cannon && ((entity->flags | entity->engine_flags) & kEntityFlagHusk) == 0 &&
					aircraft_death_ground(world, *entity) < to_fixed(entity->position.z)) {
				entity_init_aircraft_death(world, *entity, true);
				// [orig: AI_TransitionToDeath_Vehicle @0x46694E..0x466963 ->
				//  Server_SendEntityStatePacket(entity, 0)]
				emit_item_state(world, *entity, 0);
			}
		}
	}
	death_alert_block(ctx, ai);
	ai.brain.f[AiBrain::kStep] = 16;
	int32_t ground = INT32_MIN;
	Entity *entity = ctx.world != nullptr ? ctx.world->registry.get(ai.handle) : nullptr;
	if (entity != nullptr)
		ground = aircraft_death_ground(*ctx.world, *entity);
	ai.brain.f[138] = 1336;
	if (entity != nullptr && ground > to_fixed(entity->position.z))
		queue_destroy_event(ctx, ai);
}

// [orig: AI_CheckLethalDamage @0x457910]
void h_aircraft_dying_tick(AiThinkCtx &ctx) {
	if (ctx.world == nullptr)
		return;
	auto &ai = *ctx.self;
	Entity *entity = ctx.world->registry.get(ai.handle);
	if (entity == nullptr)
		return;
	const int32_t pos[3] = { to_fixed(entity->position.x), to_fixed(entity->position.y),
		to_fixed(entity->position.z) };
	const bool still = entity->saved_live_valid && entity->saved_live_pos[0] == pos[0] &&
			entity->saved_live_pos[1] == pos[1] && entity->saved_live_pos[2] == pos[2];
	if (aircraft_death_ground(*ctx.world, *entity) >= pos[2] || still) {
		entity->veh.slide_z = 0;
		queue_destroy_event(ctx, ai);
	} else {
		auto &b = ai.brain;
		b.f[138] = io::bam_add(b.f[138], 1336);
		b.f[AiBrain::kWorkPosZ] = -1000;
		b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
		b.f[AiBrain::kWorkHeading] = io::bam_add(entity->veh.yaw_bam, INT32_MAX);
		b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedB];
	}
}

// [orig: Entity_ProcessVehicleDestruction @0x466A80]
void h_enter_aircraft_dead(AiThinkCtx &ctx) {
	auto &ai = *ctx.self;
	if (ctx.world != nullptr) {
		World &world = *ctx.world;
		if (Entity *entity = world.registry.get(ai.handle)) {
			entity->veh.stuck_ticks = 0;
			entity->team = 0;
			if (((entity->flags | entity->engine_flags) & kEntityFlagHusk) == 0) {
				if (entity->ref_num != 0)
					world.vehicles.cleanup_destroyed_ref_group(*entity);
				const int32_t ground = aircraft_death_ground(world, *entity);
				entity_init_aircraft_death(world, *entity,
						ground == INT32_MIN ||
								io::bam_sub(ground, 0x4000) <= to_fixed(entity->position.z));
				// [orig: Entity_ProcessVehicleDestruction @0x466B2C..0x466B41 ->
				//  Server_SendEntityStatePacket(entity, 0)]
				emit_item_state(world, *entity, 0);
			}
			entity->flags |= 6;
			entity->engine_flags |= 6;
			entity->alive = false;
			if (!entity->death_tick)
				entity->death_tick = world.logic_tick;
		}
		ctx.sys->clear_entity_references(world, ai.handle);
		if (ai.brain.f[AiBrain::kTargetSlot] != 0) {
			if ((ai.profile.flags100 & 8) == 0) {
				const EntityHandle target{ uint16_t(ai.brain.f[AiBrain::kTargetSlot] - 1) };
				if (Entity *other = world.registry.get(target))
					other->ai_target_refcount = int16_t(uint16_t(other->ai_target_refcount) - 1u);
			}
			ctx.sys->ai_set_target(world, ai, EntityHandle{});
		}
	}
	death_alert_block(ctx, ai);
	ai.team = 0;
	ai.brain.f[AiBrain::kStep] = 62;
}

// [orig: AI_CheckAliveOrDead @0x4580C0; AI_QueueDeathSoundEvent @0x457AA0]
void h_pretty_tick(AiThinkCtx &ctx) {
	auto &ai = *ctx.self;
	const Entity *entity = ctx.world != nullptr ? ctx.world->registry.get(ai.handle) : nullptr;
	const int32_t health = entity != nullptr ? entity->health : ai.health;
	const bool boat = ai.brain.f[AiBrain::kCurState] == 22 && ai.profile.subtype == 1;
	if (health > 0) {
		if (boat && entity != nullptr) {
			int32_t pose[6];
			carrier_pose_fixed(*entity, pose, pose[3], pose[4], pose[5]);
			std::copy_n(pose, 6, &ai.brain.f[AiBrain::kWorkPosX]);
			ai.brain.f[AiBrain::kWorkPosZ] = ctx.world->env.water_z;
		}
		return;
	}
	AiEventEntry event{};
	event.f[0] = boat ? 3 : 5;
	event.f[1] = ctx.sys->index_of(ai) << 16;
	ctx.sys->events.queue(event);
}

// [orig: AI_HandleEvent_HelicopterDestroyOnly @0x468040;
// AI_HandleEvent_VehicleDestroyOnly @0x466BE0]
void h_pretty_event(AiThinkCtx &ctx) {
	if (ctx.event == nullptr || ctx.sys->ai_handle_command(*ctx.world, *ctx.self, *ctx.event))
		return;
	const bool ground = ctx.self->brain.f[AiBrain::kCurState] == 22;
	if (ground && ctx.event->type() == 3)
		ctx.self->brain.set_pend(21);
	if (ctx.event->type() == 5)
		ctx.self->brain.set_pend(ground ? 23 : 15);
}

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
	/* 0  */ { _, _, _, _ },
	/* 1  */ { _, _, _, _ },
	/* 2  */ { _, _, _, _ },
	/* 3  */ { _, _, _, _ },
	/* 4  */ { _, _, _, _ },
	/* 5  */ { _, _, _, _ },
	/* 6  HELO_LAND        */ { h_enter_aircraft_land, h_aircraft_land_tick, _, h_aircraft_event },
	/* 7  HELO_FOLLOWWP    */ { h_set_state_idle, h_aircraft_followwp_tick, _, h_aircraft_event },
	/* 8  HELO_COMBAT      */
	{ h_enter_aircraft_combat, h_aircraft_combat_tick, h_clear_target_and_bone_flag,
			h_aircraft_event },
	/* 9                   */ { _, _, _, _ },
	/* 10 HELO_EVADE       */
	{ h_enter_aircraft_evade, h_aircraft_evade_tick, h_clear_target_ref, h_aircraft_event },
	/* 11 HELO_FORMATION   */ { h_reset_to_idle, h_aircraft_followwp_tick, _, h_aircraft_event },
	/* 12                  */ { _, _, _, _ },
	/* 13 HELO_DYING       */
	{ h_enter_aircraft_dying, h_aircraft_dying_tick, _, h_handle_alert_event },
	/* 14 HELO_PRETTY      */ { h_reset_to_patrol, h_pretty_tick, _, h_pretty_event },
	/* 15 HELO_DEAD        */
	{ h_enter_aircraft_dead, h_vehicle_dead_tick, _, h_vehicle_dead_event },
	/* 16 GROUND_FOLLOWWP  */ { h_set_state_idle, h_ground_followwp_tick, _, h_combat_event },
	/* 17 GROUND_COMBAT    */
	{ h_enter_ground_combat, h_ground_combat_tick, h_clear_bone_flag, h_combat_event },
	/* 18 GROUND_EVADE     */ { h_enter_ground_evade, h_patrol_tick, _, h_combat_event },
	/* 19 GROUND_FORMATION */ { h_full_reset_to_idle, h_ground_followwp_tick, _, h_combat_event },
	/* 20 GROUND_RETURNTOBASE */ { _, _, _, _ },
	/* 21 GROUND_DYING     */
	{ h_enter_vehicle_dying, h_vehicle_dying_tick, _, h_vehicle_dying_event },
	/* 22 GROUND_PRETTY    */ { h_full_reset_to_patrol, h_pretty_tick, _, h_pretty_event },
	/* 23 GROUND_DEAD      */
	{ h_enter_vehicle_dead, h_vehicle_dead_tick, _, h_vehicle_dead_event },
};

const StateRow kDefaultRow = {h_noop, h_noop, h_noop, h_noop};

} // namespace

const StateRow &AiSystem::row(int32_t state) const {
    if (state < 0 || state >= kAiStateCount) return kDefaultRow;
    return kTable[state];
}

} // namespace opennova::world
