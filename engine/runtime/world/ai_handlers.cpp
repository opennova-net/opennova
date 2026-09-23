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
    // The arc tests compare unsigned against the arithmetic half-arc, so a
    // sign-extended (variant A) arc byte of 0x80 or more admits every bearing
    // while the unsigned divide below scores it 0 [orig: `or edx,2; sar edx,1;
    // cmp esi,edx; jnb` @0x467226..0x467230 / @0x46725A..0x467261; variant A
    // @0x465D16..0x465D20].
    if (static_cast<uint32_t>(angle_diff) >= static_cast<uint32_t>((primary_fov | 2) >> 1) ||
        distance > primary_max || distance > cand_primary_max) {
        if (static_cast<uint32_t>(angle_diff) >=
                    static_cast<uint32_t>((secondary_fov | 2) >> 1) ||
            distance > secondary_max || distance > cand_secondary_max)
            return -1; // [orig: `jnb loc_467097` @0x467261] outside both FOV/range gates. -1 (not 0) so the
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
// Handlers: every row of the retail state table @0x815238 is ported, the empty
// cells as the shared nullsubs.
// ----------------------------------------------------------------------------
namespace {

void h_noop(AiThinkCtx &) {} // [orig: nullsub_69/70/71]

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

    if (hull_health(ctx.world, e) <= 0) { // [orig: `cmp [edi+11Eh],bp` @0x467747]
        ctx.sys->queue_death_event(ctx.world, e);
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

    if (hull_health(ctx.world, e) <= 0) { // [orig: `cmp [edi+11Eh],bx` @0x457D87]
        ctx.sys->queue_death_event(ctx.world, e);
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

// The alert block the combat, evade and death-family enters share: the entity's own
// AiSlot alert byte (+0x88) goes red first and unconditionally, then the brain alert
// cur/prev = 2, the command group goes red and same-team allies within 100 u wake
// (the ally wake re-writes the own slot byte per ally, which changes nothing).
// [orig: the slot byte at the head of AI_EnterState_GroundCombat @0x467665,
//  AI_EnterState_GroundEvade @0x46741F, AI_TransitionToDeath_Vehicle @0x46696E,
//  Entity_ProcessVehicleDestruction @0x466B4C, AI_TransitionToDeath_GroundVehicle
//  @0x467BD8 and AI_TransitionToDestroyed_Vehicle @0x467DF1; each then calls
//  TriggerGroup_SetAlertRed @0x40d630 and Entity_AlertNearbyAllies @0x4654B0]
void alert_block(AiThinkCtx &ctx, AiEntity &e) {
    e.slot.bytes()[AiSlot::kAlertByte] = 2;
    AiBrain &b = e.brain;
    b.f[AiBrain::kAlert] = 2;
    b.f[AiBrain::kPrevAlert] = 2;
    if (ctx.world != nullptr) {
        if (const Entity *se = ctx.world->registry.get(e.handle))
            ctx.world->script.relations.group(se->group_id).alert = TriggerRelations::kAlertRed;
        ctx.sys->alert_nearby_allies(*ctx.world, e, 0x640000);
    }
}

// [orig: AI_EnterState_GroundCombat @0x467650] state-17 enter: the alert block, then
// brain moveStep = 1. (world-wac-ai-re §16.1)
void h_enter_ground_combat(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    alert_block(ctx, e);
    e.brain.f[AiBrain::kStep] = 1;
}

// [orig: AI_EnterState_GroundEvade @0x467400] state-18 enter: the same alert block, then
// route by the profile's EVADE_FLAGS (profile+96, aip.h token names; `test al,1`
// @0x467452, `test al,4` @0x467488): FOLLOW_WP (0x1) -> pending 17 when brain[38] holds
// a target else 16, TAIL-CALLING that state's enter (apply_transition re-reads kPendState
// after enter, so the re-route commits — the witnessed off_815238[4*state] tail call);
// FLEE (0x4) -> 17; else alive -> flee: turn about at combat speed (controller triple
// {1, 0x7FFFFFFF, 1}, moveStep 16); dead -> death event 3|4.
void h_enter_ground_evade(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    alert_block(ctx, e);
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
    if (hull_health(ctx.world, e) > 0) {       // neither flag, alive: flee [orig: @0x4674A5]
        // Turn about and drive at combat speed: the controller triple, the working
        // heading = entity heading + controller[1], working pitch/roll zeroed, the
        // mover output at brain[49], step 16; the working position is not written.
        // [orig: AI_EnterState_GroundEvade @0x46756D..0x4675B1]
        e.patrol_f0 = 1;
        e.patrol_delta = 0x7FFFFFFF;
        e.patrol_goal = 1;
        b.f[AiBrain::kWorkHeading] = io::bam_add(e.patrol_delta, e.heading);
        b.f[AiBrain::kWorkPitch] = 0;
        b.f[AiBrain::kWorkRoll] = 0;
        b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
        b.f[AiBrain::kStep] = 16;
        return;
    }
    ctx.sys->queue_death_event(ctx.world, e);  // dead: crash(3)/still(4) by |vel|
}

// The aim offset every mobile and continuation solve hands the solver through
// the `distance` global: the sweep phase under ATEAM (0x20), a fixed -3.0u
// under ATEAM_LOCK (0x40), else 0 [orig: AIEntity_ProcessWeaponFire
// @0x473364..0x473380, repeated ahead of each mobile/continuation solve].
static int32_t sm_aim_offset(const AiEntity &e) {
    if ((e.profile.flags100 & 0x20) != 0) return e.brain.f[AiBrain::kSweepPhase];
    if ((e.profile.flags100 & 0x40) != 0) return static_cast<int32_t>(0xFFFD0000u);
    return 0;
}

// One AIEntity_ProcessWeaponFire weapon block. which == 1 is the primary (the
// profile+0x78 def block, ammo brain[53], cooldown word +0xD0 against the
// +0x7C rate, saved deltas +0x2D8), 2 the secondary (+0x98, brain[54], word
// +0xD2 against +0x9C, deltas +0x2F0) [orig: primary @0x472F02..0x472F6E,
// secondary @0x472F7E..0x472FF8, the same pair at every other site].
struct SmWeapons {
    AiThinkCtx &ctx;
    AiEntity &e;

    const AiProfile::WeaponFire &block(int which) const {
        return which == 1 ? e.profile.fire_a : e.profile.fire_b;
    }
    int32_t &ammo(int which) const {
        return e.brain.f[which == 1 ? AiBrain::kAmmoA : AiBrain::kAmmoB];
    }
    // The packed cooldown pair: low u16 = primary (+0xD0), high = secondary (+0xD2).
    uint32_t cooldown(int which) const {
        const uint32_t pair = static_cast<uint32_t>(e.brain.f[AiBrain::kCooldownPair]);
        return which == 1 ? (pair & 0xFFFFu) : (pair >> 16);
    }
    uint32_t rate(int which) const {
        return static_cast<uint32_t>(which == 1 ? e.profile.fire_interval_a
                                                : e.profile.fire_interval_b);
    }
    // The ammo dword, then the zero-extended cooldown word against the rate
    // dword, unsigned [orig: `cmp [esi+0D4h],0; jz` @0x472F02..0x472F09,
    // `movzx edx,word [esi+0D0h]; cmp edx,[ebp+7Ch]; jb` @0x472F0B..0x472F15].
    bool ready(int which) const { return ammo(which) != 0 && cooldown(which) >= rate(which); }
    // Every ready block first writes its ammo byte into the hull's AdmDef byte
    // (+0x2B0), fired or not: the weapon call reads it back, and a rider's
    // mounted request copies it. [orig: primary @0x472F17..0x472F23, secondary
    // @0x472F9E..0x472FAA; the replay @0x47315F / @0x473225, the continuation
    // @0x473518 / @0x4737B4, the processed legs @0x473CA8 / @0x473D92; the read
    // `movzx eax,byte ptr [edi+2B0h]` @0x47301B]
    void stamp(int which) const {
        if (Entity *hull = ctx.world->registry.get(e.handle))
            hull->equipped_adm_index = block(which).ammo_byte();
    }
    void clear_cooldown(int which) const {
        const uint32_t pair = static_cast<uint32_t>(e.brain.f[AiBrain::kCooldownPair]);
        e.brain.f[AiBrain::kCooldownPair] =
                static_cast<int32_t>(which == 1 ? (pair & 0xFFFF0000u) : (pair & 0x0000FFFFu));
    }
    // The pre-seed every site copies into the out block: entity Position and
    // Yaw/Pitch/Roll [orig: @0x472F1D..0x472F4B].
    void seed(int32_t out[6]) const {
        out[0] = e.pos[0];
        out[1] = e.pos[1];
        out[2] = e.pos[2];
        out[3] = e.heading;
        out[4] = e.pitch;
        out[5] = e.roll;
    }
    // The solver reads the target from brain[38] itself [orig:
    // Entity_ComputeWeaponFireTransform_0 `mov edi,[esi+98h]` @0x4569A4].
    const Entity *target() const {
        const int32_t packed = e.brain.f[AiBrain::kTargetSlot];
        return packed != 0 ? ctx.world->registry.get(
                                     EntityHandle{static_cast<uint16_t>(packed - 1)})
                           : nullptr;
    }
    bool solve(int which, int32_t aim_offset, bool defer_los, int32_t out[6]) const {
        return ctx.sys->solve_weapon_fire_transform(*ctx.world, e, target(), block(which),
                                                    aim_offset, defer_los, out,
                                                    /*seeded=*/true);
    }
    // Weapon_FireProcess with the block's ammo byte, after the ammo decrement
    // that runs only on a positive count [orig: RC_FIRE primary
    // @0x473046..0x473067, secondary @0x473008..0x473029].
    void fire(int which, const int32_t out[6]) const {
        if (ammo(which) > 0) --ammo(which);
        (void)ctx.sys->fire_ai_round(*ctx.world, e, out, out[3], out[4], block(which).ammo_index);
    }
    // Under ATEAM_LOCK the solved pose is kept relative to the hull for the
    // locked burst [orig: primary @0x473F47..0x473F95, secondary
    // @0x473E67..0x473EB5].
    void save_delta(int which, const int32_t out[6]) const {
        if ((e.profile.flags100 & 0x40) == 0) return;
        const int base = which == 1 ? AiBrain::kSavedDeltaA : AiBrain::kSavedDeltaB;
        int32_t pose[6];
        seed(pose);
        for (int axis = 0; axis < 6; ++axis)
            e.brain.f[base + axis] = io::bam_sub(out[axis], pose[axis]);
    }
    // Two draws of the dword_31BFBB8 rol-LCG, yaw then pitch: (u16 % (6 -
    // brain[43])) * 8947848.0f truncated, added when odd, subtracted when
    // even [orig: secondary continuation @0x473640..0x473716, processed
    // primary @0x473F9B..0x474072; flt_7C6F60; the 0x80000001 parity fold].
    void scatter(int32_t out[6]) const {
        const int32_t mod = 6 - e.brain.f[AiBrain::kAccuracy];
        if (mod == 0) return;
        for (int angle = 3; angle <= 4; ++angle) {
            const uint16_t draw = static_cast<uint16_t>(ctx.sys->prng_step_a());
            const int32_t scaled = static_cast<int32_t>(
                    static_cast<double>(static_cast<int32_t>(draw) % mod) * 8947848.0f);
            out[angle] = (scaled & 1) != 0 ? io::bam_add(out[angle], scaled)
                                           : io::bam_sub(out[angle], scaled);
        }
    }
    // A mobile or continuation shot after its solve: deltas, scatter, fire,
    // then the ATEAM sweep steps 1/6u and past +3.0u wraps to -3.0u dropping
    // brain[38] by a direct write (AiSlot[3] and the refcount keep the old
    // target), else ATEAM_LOCK re-arms the burst window; only the primary
    // marks the bone byte; the cooldown word clears [orig: primary
    // @0x473F34..0x4740D6, secondary @0x473E54..0x4741B1; sweep/lock
    // @0x47408F..0x4740C3].
    void shoot_mobile(int which, int32_t out[6]) const {
        AiBrain &b = e.brain;
        save_delta(which, out);
        scatter(out);
        fire(which, out);
        if ((e.profile.flags100 & 0x20) != 0) {
            b.f[AiBrain::kSweepPhase] = io::bam_add(b.f[AiBrain::kSweepPhase], 0x2AA6);
            if (b.f[AiBrain::kSweepPhase] > 0x30000) {
                b.f[AiBrain::kSweepPhase] = static_cast<int32_t>(0xFFFD0000u);
                b.f[AiBrain::kTargetSlot] = 0;
            }
        } else if ((e.profile.flags100 & 0x40) != 0) {
            b.f[AiBrain::kBurstWindow] = 1;
        }
        if (which == 1) b.bytes()[AiBrain::kBoneFlagByte] |= 0x40;
        clear_cooldown(which);
    }
};

// The chase leg's match speed for a target without an SM brain: its planar
// velocity (entity +0x98/+0x9C) magnitude, clamped and truncated
// [orig: @0x473B83..0x473BCE].
static int32_t planar_speed(const AiSystem &sys, const Entity &target) {
    int32_t vx = target.veh.vel_x, vy = target.veh.vel_y;
    if (const AiEntity *body = sys.for_handle(target.handle); body && body->inf.active) {
        vx = body->inf.vel[0];
        vy = body->inf.vel[1];
    }
    double m = std::sqrt(static_cast<double>(vx) * vx + static_cast<double>(vy) * vy);
    if (m > kDeathSpeedClamp) m = kDeathSpeedClamp;
    return static_cast<int32_t>(m);
}

// [orig: AIEntity_ProcessWeaponFire @0x472E00] the state-17 GROUND_COMBAT tick,
// the vehicle/emplacement fire and chase routine (world-wac-ai-re §17.6) in the
// retail leg order: the stationary RC_FIRE leg, the ATEAM_LOCK replay, the
// no-target search, the between-tick continuation or turret staging, and the
// processed tick (retarget, chase, heading gate, processed fire). Its aircraft
// twin is AI_TickState_AircraftCombat @0x471710 (aircraft_combat_tick).
void h_ground_combat_tick(AiThinkCtx &ctx) {
    AiEntity &e = *ctx.self;
    AiBrain &b = e.brain;
    World &world = *ctx.world;
    const AiProfile &p = e.profile;

    // The death leg: event 3/4 by horizontal speed [orig: `cmp [edi+11Eh],bx`
    // @0x472E26 -> @0x4744B8; the +0x9C/+0x98 speed @0x4744B8..0x474508].
    if (hull_health(&world, e) <= 0) {
        ctx.sys->queue_death_event(&world, e);
        return;
    }

    const int32_t step = b.f[AiBrain::kStep];
    b.f[AiBrain::kTickAccum] += step; // [orig: @0x472E37..0x472E3A]
    // The PACKED cooldown pair: one add advances both u16 words (+208/+210) by step,
    // low-word carry included. [orig: brain[52] += 0x10001 * brain[7]: `imul
    // ecx,10001h` @0x472E42, `add [esi+0D0h],ecx` @0x472E48]
    b.f[AiBrain::kCooldownPair] = static_cast<int32_t>(
        static_cast<uint32_t>(b.f[AiBrain::kCooldownPair]) +
        0x10001u * static_cast<uint32_t>(step));
    // The burst window: += step while armed and <= 186, else reset.
    // [orig: brain[181] @0x472E4E..0x472E70]
    if ((p.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] != 0 &&
        static_cast<uint32_t>(b.f[AiBrain::kBurstWindow]) <= 0xBAu)
        b.f[AiBrain::kBurstWindow] += step;
    else
        b.f[AiBrain::kBurstWindow] = 0;

    // One processed tick per accumulated 16 [orig: @0x472E76..0x472ECD].
    bool processed = false;
    if (b.f[AiBrain::kTickAccum] >= 16) {
        b.f[AiBrain::kCombatTimer] += 16;
        b.f[AiBrain::kTickAccum] = 0;
        processed = true;
        if ((p.flags96 & 0x10) != 0 && b.f[AiBrain::kFireTimer] > 0)
			if (Entity *vehicle = world.registry.get(e.handle))
				world.vehicles.release_flares(*vehicle);
		b.f[AiBrain::kFireTimer] = std::max(0, b.f[AiBrain::kFireTimer] - 16);
		b.f[AiBrain::kFireDelay] = std::max(0, b.f[AiBrain::kFireDelay] - 16);
        b.f[AiBrain::kRetargetTimer] += 16; // the §16.3 retarget cadence incrementer
    }

    const SmWeapons weapons{ctx, e};
    uint8_t &bone = b.bytes()[AiBrain::kBoneFlagByte];
    // brain[48], the dispatcher's no-ammo latch, forces the processed leg every
    // tick, holds the stationary guns and the replay, and turns the chase into
    // the half-turn heading [orig: brain+0xC0 @0x472EEB, @0x4730FE, @0x473309,
    // @0x473AB1].
    const bool no_ammo = b.f[AiBrain::kNoTargetIdle] != 0;

    // RC_FIRE, the stationary leg [orig: `test al,al; jns` @0x472ED3..0x472ED8].
    if ((p.flags100 & 0x80) != 0) {
        if (b.bytes()[AiBrain::kGuardFireByte] != 0) { // [orig: @0x472EDE]
            // Weapons free keeps the give-up timer at zero [orig: @0x472EF2].
            b.f[AiBrain::kCombatTimer] = 0;
            if (!no_ammo) {
                // Primary, then secondary: no aim offset, LOS checked, no scatter
                // [orig: solves @0x472F6E / @0x472FF8, `push 0` @0x472F45 / @0x472FCC].
                int fired = 0;
                for (int which = 1; which <= 2 && fired == 0; ++which) {
                    int32_t out[6];
                    weapons.seed(out);
                    if (!weapons.ready(which)) continue;
                    weapons.stamp(which);
                    if (!weapons.solve(which, 0, false, out)) continue;
                    weapons.fire(which, out);
                    if (which == 1) bone |= 0x40; // [orig: @0x47306F]
                    weapons.clear_cooldown(which);
                    fired = which; // [orig: brain[106] @0x47303A / @0x47307F]
                }
                b.f[AiBrain::kLastWeapon] = fired;
                if (fired == 0) bone = 0; // [orig: @0x47308B..0x473092]
            }
        } else {
            bone = 0; // [orig: @0x4730A8]
            if (b.f[AiBrain::kCombatTimer] >= 620)
                b.set_pend(b.f[AiBrain::kFallback]); // [orig: `jl` @0x47309E..0x4730B4]
        }
        if (processed) {
            ctx.sys->ai_set_target(world, e, EntityHandle{}); // [orig: @0x4730C5]
            // The stationary leg walks through AI_UpdateMovementTarget, not the
            // waypoint mover [orig: `call AI_UpdateMovementTarget` @0x4730D9].
            if ((p.flags100 & 1) != 0)
                ctx.sys->update_movement_target(e, world);
        }
        return;
    }

    // ATEAM_LOCK: the locked burst refires the saved relative pose without a
    // solve or scatter [orig: @0x4730E9..0x4732D9].
    if ((p.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] != 0) {
        const int which = b.f[AiBrain::kLastWeapon];
        if (no_ammo || (which != 1 && which != 2)) {
            bone = 0; // [orig: @0x4730FE / @0x47311D -> @0x4732C3]
        } else if (weapons.ammo(which) != 0) {
            if (weapons.cooldown(which) >= weapons.rate(which)) {
                // [orig: secondary @0x473142..0x4731DD, primary @0x473208..0x4732AA]
                weapons.stamp(which);
                const int base = which == 1 ? AiBrain::kSavedDeltaA : AiBrain::kSavedDeltaB;
                int32_t out[6];
                weapons.seed(out);
                for (int axis = 0; axis < 6; ++axis)
                    out[axis] = io::bam_add(out[axis], b.f[base + axis]);
                weapons.fire(which, out);
                if (which == 1) bone |= 0x40; // [orig: @0x4732A3]
                weapons.clear_cooldown(which);
            } else {
                // Held more than a quarter of the PRIMARY rate: the bone byte
                // clears, for either weapon [orig: @0x4732B5..0x4732C3].
                const uint32_t rate_a = weapons.rate(1);
                if (weapons.cooldown(which) >= rate_a - (rate_a >> 2)) bone = 0;
            }
        }
        if (processed && (p.flags100 & 1) != 0)
            ctx.sys->update_waypoint_movement(e, world); // [orig: @0x4732CA -> @0x4741ED]
        return;
    }

    const Entity *tent = weapons.target(); // [orig: @0x4732DE]
    if (tent == nullptr) { // no target: search [orig: @0x4732EC -> @0x47420F]
        b.f[AiBrain::kSweepPhase] = -196608;    // [orig: @0x47420F]
        b.f[AiBrain::kWorkPosX] = 0;            // [orig: @0x474219]
        b.f[AiBrain::kWorkPosY] = 0;            // [orig: @0x47421F]
        if ((p.flags100 & 1) != 0) {
            ctx.sys->update_waypoint_movement(e, world); // [orig: @0x47422E]
        } else if ((p.flags100 & 4) != 0) {
            b.f[AiBrain::kWorkHeading] = e.heading; // [orig: @0x47425F..0x474266]
        } else {
            // The turret search sweep: yaw + ~90 deg outside the no-target timer's
            // 125..434 window [orig: @0x47426E..0x4742BF].
            const int32_t t = b.f[AiBrain::kCombatTimer];
            b.f[AiBrain::kWorkHeading] =
                (t <= 124 || t > 434) ? e.heading + 1073741760 : e.heading;
            b.f[AiBrain::kWorkPitch] = 0;
            b.f[AiBrain::kWorkRoll] = 0;
            b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedB];
            AiTarget found{};
            if (ctx.sys->acquire_target(world, e, found)) { // [orig: @0x4742C5]
                // This site's engage: both target-brain branches jitter via the
                // inline LCG guarded by base-delay != 0 [orig: @0x47431E..0x4744B7].
                const Entity *se = world.registry.get(e.handle);
                const Entity *te = found.handle.valid() ? world.registry.get(found.handle)
                                                        : nullptr;
                if (se != nullptr && te != nullptr)
                    ctx.sys->apply_engage_relations(world, *se, *te); // sees + targeted quads
                ctx.sys->ai_set_target(world, e, found.handle);
                ctx.sys->target_set_calls.push_back(found.net_id);
                b.f[AiBrain::kCombatTimer] = 0;
                b.f[AiBrain::kFireDelay] = p.field104;
                if (p.field104 != 0)
                    b.f[AiBrain::kFireDelay] += static_cast<int32_t>(
                        static_cast<uint16_t>(ctx.sys->prng_step_a()) % 62);
                return;
            }
        }
        if (b.f[AiBrain::kCombatTimer] > 620) { // give up [orig: @0x474236..0x474256]
            ctx.sys->ai_set_target(world, e, EntityHandle{});
            b.set_pend(b.f[AiBrain::kFallback]);
        }
        return;
    }

    if (tent->health <= 0) { // target dead [orig: @0x4732F2 -> @0x4741FD]
        ctx.sys->ai_set_target(world, e, EntityHandle{});
        return;
    }

    // Between processed ticks [orig: @0x4732FF / @0x473309].
    if (!processed && !no_ammo) {
        const int which = b.f[AiBrain::kLastWeapon]; // [orig: @0x473315]
        if (which == 1 || which == 2) {
            // The continuation keeps the last weapon's volley going, cooldown
            // gated, with LOS deferred (arg7 = 1) [orig: primary @0x473791..0x47386B,
            // `push 1` @0x473850; secondary @0x4734F2..0x4735C9, `push 1` @0x4735A0].
            if (!weapons.ready(which)) return;
            weapons.stamp(which);
            int32_t out[6];
            weapons.seed(out);
            if (which == 1) out[4] = 0; // the primary solves from a level pitch [orig: @0x473821]
            if (!weapons.solve(which, sm_aim_offset(e), true, out)) {
                bone = 0; // [orig: @0x473875 -> @0x4734E4]
                return;
            }
            weapons.shoot_mobile(which, out);
            return;
        }
        // No volley on record: a WEAPON_TURRET block re-solves every tick so its
        // turret slews, primary first, else the secondary, else nothing; no fire
        // [orig: @0x47332D `test byte [ebp+88h],1` -> @0x4733A4, deltas
        // @0x4733AC..0x4733FA, `mov [esi+310h],bl` @0x473402; @0x47340E
        // `test byte [ebp+0A8h],1` -> @0x473488..0x4734E6].
        const int slot = (p.fire_a.flags & 1) != 0 ? 1 : (p.fire_b.flags & 1) != 0 ? 2 : 0;
        if (slot == 0) return;
        int32_t out[6];
        weapons.seed(out);
        (void)weapons.solve(slot, sm_aim_offset(e), true, out);
        weapons.save_delta(slot, out);
        bone = 0;
        return;
    }

    // The processed tick (or the no-ammo latch). The engage fire delay holds
    // everything but the waypoint walk [orig: @0x473A2C -> @0x4741E3..0x4741ED].
    if (b.f[AiBrain::kFireDelay] != 0) {
        if ((p.flags100 & 1) != 0) ctx.sys->update_waypoint_movement(e, world);
        return;
    }

    // The retarget cadence: every rescan feeds Entity_SetAITarget, so a scan
    // that finds nothing clears brain[38]; this tick still bears on the old
    // target [orig: AIEntity_TryAcquireTarget @0x4716B0, `call Entity_SetAITarget`
    // @0x4716F0 unconditional; caller `jz` @0x473A41..0x473A45].
    if (b.f[AiBrain::kRetargetTimer] > 248) {
        b.f[AiBrain::kRetargetTimer] = 0;
        AiTarget fresh{};
        // A type-1 profile searches with variant A [orig: AIEntity_TryAcquireTarget
        // `cmp dword ptr [eax+10h],1` @0x4716D5 (the AI_FindBestTarget call @0x4716DD)].
        const EntityHandle found = ctx.sys->acquire_target(world, e, fresh, p.type == 1)
                                           ? fresh.handle
                                           : EntityHandle{};
        ctx.sys->ai_set_target(world, e, found);
        if (const Entity *next = found.valid() ? world.registry.get(found) : nullptr) tent = next;
    }

    // Bearing to the tracked target, truncated [orig: @0x473A49..0x473A90].
    const int32_t tpos[3] = {static_cast<int32_t>(tent->position.x * 65536.0f),
                             static_cast<int32_t>(tent->position.y * 65536.0f),
                             static_cast<int32_t>(tent->position.z * 65536.0f)};
    const int32_t dy = io::bam_sub(tpos[1], e.pos[1]);
    const int32_t dx = io::bam_sub(tpos[0], e.pos[0]);
    const int32_t bearing = bearing_bam(dy, dx);
    if ((p.flags100 & 1) != 0) {
        ctx.sys->update_waypoint_movement(e, world); // [orig: @0x473A9C]
    } else if ((p.flags100 & 4) != 0 || no_ammo) {
        b.f[AiBrain::kWorkHeading] = bearing + 2147483520; // [orig: @0x473C08 +0x7FFFFF80]
    } else {
        // The chase: a planar 16.16 distance against the approach cap (+0x4C);
        // beyond it the >620 give-up; inside it the timer rests only while
        // AI_IsTargetInSight @0x456860 validates the target; full combat
        // speed at or beyond max_chase (+0xBC) or min_chase (+0xB8), else the
        // target's own speed [orig: @0x473ABE..0x473C06].
        b.f[AiBrain::kWorkHeading] = bearing; // [orig: @0x473AC2]
        double planar = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
        if (planar > kDeathSpeedClamp) planar = kDeathSpeedClamp;
        const int32_t dist = static_cast<int32_t>(planar); // [orig: @0x473AEB]
        if (dist > p.approach_cap) { // [orig: @0x473AF3]
            if (b.f[AiBrain::kCombatTimer] > 620) { // [orig: @0x473AFC..0x473B15]
                b.set_pend(b.f[AiBrain::kFallback]);
                ctx.sys->ai_set_target(world, e, EntityHandle{});
                return;
            }
        } else if (dist >= p.max_chase) { // [orig: @0x473B29 -> @0x473BE4]
            if (ctx.sys->aircraft_target_in_sight(e, world)) b.f[AiBrain::kCombatTimer] = 0;
            b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA]; // [orig: @0x473BFA]
        } else {
            if (ctx.sys->aircraft_target_in_sight(e, world)) // [orig: @0x473B36..0x473B42]
                b.f[AiBrain::kCombatTimer] = 0;
            if (dist >= p.min_chase) { // [orig: @0x473B53 -> @0x473BD6]
                b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
            } else if (const AiEntity *other = ctx.sys->for_handle(tent->handle);
                       other != nullptr && !other->inf.active) {
                b.f[AiBrain::kOutSpeed] = other->brain.f[136]; // [orig: @0x473B72]
            } else {
                b.f[AiBrain::kOutSpeed] = planar_speed(*ctx.sys, *tent);
            }
        }
    }
    // The common tail: speed clamped to [0, speedA]; the work X/Y, pitch, roll
    // and brain[138] zeroed (the vehicle movers read only +0x200/+0x210)
    // [orig: @0x473C14..0x473C50].
    if (b.f[AiBrain::kOutSpeed] < 0)
        b.f[AiBrain::kOutSpeed] = 0;
    else if (b.f[AiBrain::kOutSpeed] > b.f[AiBrain::kSpeedA])
        b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
    b.f[AiBrain::kWorkPosX] = 0;
    b.f[AiBrain::kWorkPosY] = 0;
    b.f[AiBrain::kWorkPitch] = 0;
    b.f[AiBrain::kWorkRoll] = 0;
    b.f[138] = 0;

    // The heading gate: the bearing delta folded to 1/256 turns against the
    // secondary FOV byte [orig: `movzx` @0x472E1F; @0x473C56..0x473C7B `ja`].
    const uint32_t hd = static_cast<uint32_t>(io::bam_sub(bearing, e.heading)) >> 24;
    const uint32_t folded = hd >= 0x80 ? 256 - hd : hd;
    if (folded > static_cast<uint32_t>(((p.fov_secondary | 1) | 2) >> 1)) return;

    // The processed fire: primary, else secondary, each solved WITH LOS (arg7 =
    // 0), scattered and fired; brain[106] records the weapon [orig: primary
    // @0x473C81..0x473D57 `push 0` @0x473D3C, @0x4740DF; secondary
    // @0x473D67..0x473E44 `push 0` @0x473E26, @0x4741BA].
    for (int which = 1; which <= 2; ++which) {
        if (!weapons.ready(which)) continue;
        weapons.stamp(which);
        int32_t out[6];
        weapons.seed(out);
        if (!weapons.solve(which, sm_aim_offset(e), false, out)) continue;
        weapons.shoot_mobile(which, out);
        b.f[AiBrain::kLastWeapon] = which;
        return;
    }
    // Neither weapon fired: no volley to continue [orig: @0x4741CC..0x4741D6].
    b.f[AiBrain::kLastWeapon] = 0;
    bone = 0;
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
	// list the class init built (VehicleSystem::setup_gunner_attachments): for every
	// live child the global hit record's damage word is cleared, the child's
	// health word is zeroed and its class event callback runs with phase 1 on
	// that record, on clients too. The child's attacker (+0x178) is left alone.
	// [orig: AI_TransitionToDeath_GroundVehicle @0x467B6E (the +1352 byte); the
	//  loop @0x467B90..0x467BCC: the +286 > 0 gate @0x467B92..0x467B9A,
	//  Projectile_GetHitRecord @0x467B9C, health 0 @0x467BA5, the record's +0x30
	//  cleared @0x467BAD, child+0x1C8(child, 1, 0) @0x467BB0..0x467BBB]
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
				if (AiEntity *brain = world.ai.for_handle(child->handle))
					brain->health = 0;
				world.round_sim.hit_record.damage = 0;
				hit_record_class_event(world, *child, 1);
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
	alert_block(ctx, e);
	b.f[AiBrain::kStep] = 16;  // [orig: ai_data[7] = 16 @0x467c02]
    if (hull_death_speed(ctx.world, e) < 1057) // [orig: @0x467c51 — stopped -> destroy now]
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
	const bool stopped = hull_death_speed(ctx.world, e) < 1057; // [orig: @0x467CED..0x467D30]
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
    alert_block(ctx, e);
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
			// [orig: +0x1AC first write wins: `cmp [esi+1ACh],0` @0x467E5D ..
			//  `mov [esi+1ACh],eax` @0x467E6B]
			if (ent->death_tick == 0)
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
// then the brain's intact/husk floor (brain_ground_offset).
// [orig: AI_TransitionToDeath_Vehicle @0x4668D9..0x46693C; AI_CheckLethalDamage
//  @0x45791F..0x457970]
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
	return ground == INT32_MIN ? ground : io::bam_add(ground, brain_ground_offset(world, entity));
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
	// Variant A [orig: AI_HandleEvent_VehicleWithDamageC @0x466460 (the
	// AI_FindBestTarget call @0x46648F)].
	const bool acquired = (ai.profile.flags100 & 2) == 0 &&
			ctx.sys->acquire_target(*ctx.world, ai, target, /*variant_a=*/true);
	aircraft_flare_timer(ctx);
	if (acquired)
		ctx.sys->engage_target(*ctx.world, ai, target, true);
	else
		ctx.sys->update_movement_target(ai, *ctx.world);
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
	alert_block(ctx, ai);
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
	// The team byte clears at the head, so the ally wake below compares team 0
	// [orig: `mov byte ptr [esi+162h],0` @0x466A9F ahead of
	//  Entity_AlertNearbyAllies @0x466B77].
	ai.team = 0;
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
	alert_block(ctx, ai);
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
