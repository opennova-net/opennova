// Entity AI subsystem — byte-exact foundation port. See world/ai.h + the RE map
// notes/world/ai_movement.md. Each handler cites its Jointops.exe origin; heavy
// per-state behaviors route to not_yet_ported until their phase lands.
#include "world/ai.h"

#include "terrain/height_field.h"
#include "world/angle.h"
#include "world/body_anim.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <io/bam.h>

namespace opennova::world {

namespace {

// Minimal port of Entity_UpdateInfantryAI @0x4b9910's anim selection: pick a body-anim slot
// from the brain state + movement so 3rd-person NPCs walk/idle instead of sliding at rest.
// Writes the world Entity's body_anim_slot (what the present snapshot reads). Full fidelity
// (randomized idle variants, jog/run thresholds, attack/death clips) is a grill follow-up.
void update_body_anim_slot(AiEntity &e, World &world) {
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;
    if (!ent->alive || ent->health <= 0) return; // dead: present pass hides it; leave the slot
    const AiBrain &b = e.brain;
    const bool moving = b.f[AiBrain::kOutSpeed] > 0;
    int32_t slot;
    if (moving) {
        slot = (b.f[AiBrain::kAlert] >= 2) ? kBodyAnimRunForward : kBodyAnimWalkForward;
    } else {
        slot = kBodyAnimIdle;
    }
    ent->body_anim_slot = slot;
}

int mounted_anim_state_for_seat(const Entity &target, const Seat &seat, const InfantryState &inf,
                                const IRootMotionSource *root_motion) {
    if (seat.type == SeatType::Gunner) {
        const int variant = target.emplaced_config_valid
                                ? std::clamp<int>(target.emplaced_config, 0, 8)
                                : 0;
        if (variant > 0) {
            const int candidate = anim_state::kEmplaced + variant;
            if (root_motion != nullptr && root_motion->has_clip(inf.adm_id, candidate)) {
                return candidate;
            }
        }
        return anim_state::kEmplaced;
    }

    // The original derives this from the mounted seat bone name: sitexNN/ctrlxNN/drvrxNN
    // becomes anim_sit + NN. The dynamic sit_24 driver lean states need vehicle control fields
    // that are not modeled in this port yet, so this resolver intentionally stops at base sit_N.
    return anim_state::kSit + std::clamp<int>(seat.pose_index, 0, 30);
}

// radians -> 32-bit binary angle. [orig: dbl_7C19D8 = 0x41C45F306DC9C883.]
constexpr double kBamPerRadian = 683565275.5764316; // 2^32 / (2*pi)

// mission yaw degrees -> 32-bit binary angle (entity+16). [orig: AI_HandleCommand cmd 0x16
// @0x4659fa; matches promote.cpp's seed.] Used to mirror a posed mount transform into the brain.
constexpr int64_t kBamPerDegreeInt = 11930464; // trunc(2^32/360) — the original multiplier

// Saturation clamp on the death-velocity magnitude. [orig: flt_7C19E0 = 0x4EFFFE00.]
constexpr double kDeathSpeedClamp = 2147418112.0;

// Byte-exact integer abs (cdq/xor/sub idiom; INT_MIN -> INT_MIN like the orig).
inline int32_t iabs32(int32_t v) {
    int32_t s = v >> 31;
    return (v ^ s) - s;
}

// Shared distance + bearing math for both waypoint types. [orig: AIWaypoint_UpdateTarget
// @0x457380.] Approx 3D distance = larger-axis + ((5*(other-axis + |dy|)) >> 16), all in
// wrapping 32-bit; bearing = trunc(atan2(dz, dx) * 2^32/2pi). dx/dz/dy follow the decomp
// naming (dx<-pos+4, dz<-pos+8, dy<-pos+12).
void wp_dist_bearing(int32_t dx, int32_t dz, int32_t dy, int32_t &dist, int32_t &bearing) {
    int32_t adx = iabs32(dx), adz = iabs32(dz), ady = iabs32(dy);
    int32_t base, cross;
    if (adx <= adz) { base = adz; cross = static_cast<int32_t>(static_cast<uint32_t>(adx) + static_cast<uint32_t>(ady)); }
    else            { base = adx; cross = static_cast<int32_t>(static_cast<uint32_t>(adz) + static_cast<uint32_t>(ady)); }
    int32_t term = static_cast<int32_t>(static_cast<uint32_t>(cross) * 5u) >> 16; // x5 wrap, arithmetic >>16
    dist = static_cast<int32_t>(static_cast<uint32_t>(base) + static_cast<uint32_t>(term));
    bearing = static_cast<int32_t>(static_cast<int64_t>(
        std::atan2(static_cast<double>(dz), static_cast<double>(dx)) * kBamPerRadian)); // chop toward zero
}

// 32-bit rotate-left. [orig: __ROL4__.]
inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// One step of the shared rotate-LCG. [orig: __ROL4__(s + __ROL4__(s,11), 4) ^ 1.] Returns the
// new state; the low 16 bits are PRNG_Next16's value and the engagement jitter source.
inline uint32_t prng_step(uint32_t &s) {
    uint32_t r = rotl32(s + rotl32(s, 11), 4);
    s = r ^ 1u;
    return s;
}

// Bearing to a point in 32-bit binary angle. [orig: AI_FindBestTargetB @0x4671a0 fpatan path —
// atan2(candidate.Y - self.Y, candidate.X - self.X), x87 chop toward zero.] dY/dX follow the
// mover's convention (atan2(dz, dx)).
inline int32_t bearing_bam(int32_t dY, int32_t dX) {
    return static_cast<int32_t>(static_cast<int64_t>(
        std::atan2(static_cast<double>(dY), static_cast<double>(dX)) * kBamPerRadian));
}

// 3D distance in world units. [orig: AI_FindBestTargetB @0x4671e8 sqrt of dx^2+dy^2+dz^2,
// flt_7C19E0 min-clamp, ftol2_sse (chop), then >> 16.]
inline int32_t dist3d_units(const int32_t a[3], const int32_t b[3]) {
    double dx = static_cast<double>(a[0] - b[0]);
    double dy = static_cast<double>(a[1] - b[1]);
    double dz = static_cast<double>(a[2] - b[2]);
    double m = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (m > kDeathSpeedClamp) m = kDeathSpeedClamp; // flt_7C19E0 min-clamp idiom
    return static_cast<int32_t>(m) >> 16;           // ftol2 chop, then >>16 to world units
}

} // namespace

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

// ----------------------------------------------------------------------------
// AiEventQueue. [orig: AIEvent_QueueEntry @0x455da0 / AIEvent_ProcessTimedEntries @0x455df0.]
// ----------------------------------------------------------------------------
void AiEventQueue::queue(const AiEventEntry &e) {
    if (count_ < kMax) buf_[count_++] = e;
}

void AiEventQueue::process_timed(AiSystem &sys, World &world) {
    int index = 0;
    while (index < count_) {
        AiEventEntry &entry = buf_[index];
        float remaining = entry.timer() - kFrameDt;
        entry.set_timer(remaining);
        if (remaining <= 0.0f) {
            AiEntity *e = sys.at(entry.entity_index());
            if (e && e->brain.f[AiBrain::kOwner]) {
                AiThinkCtx ctx{&sys, e, &world, &entry};
                sys.row(e->brain.f[AiBrain::kCurState]).event(ctx); // off_815244
                int32_t pend = e->brain.f[AiBrain::kPendState];
                if (pend) {
                    int32_t cur = e->brain.f[AiBrain::kCurState];
                    if (pend != cur) {
                        sys.row(cur).exit(ctx);   // off_815240
                        sys.row(pend).enter(ctx); // off_815238
                        // Re-read: the evade enter re-routes by rewriting the pending
                        // state and tail-calling the routed enter [orig: @0x467400 —
                        // the original reads the brain field, not a saved copy].
                        e->brain.f[AiBrain::kCurState] = e->brain.f[AiBrain::kPendState];
                    }
                }
            }
            // Compact: swap-with-last (faithful to the orig array compaction).
            buf_[index] = buf_[count_ - 1];
            --count_;
            --index;
        }
        ++index;
    }
}

// ----------------------------------------------------------------------------
// AiSystem.
// ----------------------------------------------------------------------------
AiSystem::AiSystem() {
    clear_handle_index();
}

void AiSystem::clear_handle_index() {
    handle_to_ai_index_.assign(65536, -1);
}

void AiSystem::rebuild_handle_index() {
    clear_handle_index();
    for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
        const EntityHandle h = entities_[i].handle;
        if (h.valid()) handle_to_ai_index_[h.packed] = i;
    }
}

int AiSystem::attach(EntityHandle h) {
    AiEntity e;
    e.handle = h;
    e.brain.f[AiBrain::kOwner] = 1; // nonzero = live slot
    entities_.push_back(e);
    const int index = static_cast<int>(entities_.size()) - 1;
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

AiEntity *AiSystem::at(int ai_index) {
    if (ai_index < 0 || ai_index >= static_cast<int>(entities_.size())) return nullptr;
    return &entities_[ai_index];
}

AiEntity *AiSystem::for_handle(EntityHandle h) {
    if (!h.valid()) return nullptr;
    if (handle_to_ai_index_.size() != 65536) return nullptr;
    const int index = handle_to_ai_index_[h.packed];
    if (index < 0 || index >= static_cast<int>(entities_.size())) return nullptr;
    AiEntity &e = entities_[index];
    return e.handle == h ? &e : nullptr;
}

void AiSystem::set_entity_muzzle(EntityHandle h, const int32_t pos[3], uint32_t logic_tick) {
    AiEntity *e = for_handle(h);
    if (e == nullptr || pos == nullptr) return;
    e->muzzle_world[0] = pos[0];
    e->muzzle_world[1] = pos[1];
    e->muzzle_world[2] = pos[2];
    e->muzzle_tick = logic_tick;
    e->muzzle_valid = true;
}

// [orig: AI_BeginUpdate @0x457b40] copy working fields, then the shared-budget gate.
bool AiSystem::begin_update(AiEntity &e) {
    AiBrain &b = e.brain;
    b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA]; // [128]=[49]
    b.f[138] = e.profile.field220;
    b.f[134] = 0;
    b.f[133] = 0;
    b.f[131] = b.f[51] + e.profile.field216;
    b.f[132] = e.heading;
    int32_t budget_used = scheduler.budget;
    if (budget_used > AiScheduler::kBudgetCap) {
        if ((e.profile.flags100 & 2) == 0)
            b.f[AiBrain::kPendState] = 8;
        else
            b.f[AiBrain::kPendState] = b.f[AiBrain::kFallback];
        return false;
    }
    scheduler.budget = budget_used + b.f[AiBrain::kStep];
    return true;
}

// [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event 0=update,1=spawn,4=death.
void AiSystem::process_infantry_state_machine(AiEntity &e, World &world, int event) {
    AiBrain &b = e.brain;

    // "no target" idle gate.
    if ((b.f[53] == 0 && b.f[54] == 0) || (!e.profile.has_src148 && !e.profile.has_src180)) {
        if (b.f[AiBrain::kGuard] == 0)
            b.f[AiBrain::kNoTargetIdle] = 1;
    }

    int32_t alert = b.f[AiBrain::kAlert];
    if (is_authority && e.has_physics && (e.physics_flags & 0x100) == 0 &&
        b.f[AiBrain::kPrevAlert] != alert) {
        alert = 2;
        if ((e.profile.flags96 & 2) == 0 && b.f[AiBrain::kCurState] != 14)
            b.f[AiBrain::kPendState] = 10;
    }
    b.f[AiBrain::kPrevAlert] = alert;

    AiThinkCtx ctx{this, &e, &world, nullptr};
    const int ai_index = static_cast<int>(&e - at(0));

    // LABEL_27/28/31: authority applies the pending transition; clients apply only a
    // restricted subset (pending in {7} or 12<pending<=15).
    auto finish = [&]() {
        if (is_authority) {
            apply_transition(e, world);
        } else {
            int32_t pend = b.f[AiBrain::kPendState];
            if (pend == 7 || (pend > 12 && pend <= 15))
                apply_transition(e, world);
        }
    };

    if (event == 0) {
        if (is_authority || b.f[AiBrain::kCurState] == 13 || b.f[AiBrain::kCurState] == 15)
            row(b.f[AiBrain::kCurState]).tick(ctx);
        ++b.f[AiBrain::kTick];
        update_body_anim_slot(e, world); // pick walk/idle from state+movement for the present pass
        finish();
        return;
    }
    if (event == 1) { // spawn
        AiEventEntry ev{};
        ev.f[0] = 1;
        ev.f[1] = 9 | (ai_index << 16); // channel 9 | entity index
        ev.set_timer(0.0f);
        // [orig: ev.f[3] = sub_4E7000()[17] @0x458326] spawn payload from an unmodeled accessor;
        // left 0 (TODO: model sub_4E7000). If a spawn event later reaches a ground combat-event
        // handler (cur_state in {16,17,18}), h_combat_event reads f[3] into brain[39] (kDamageInfo).
        events.queue(ev);
        finish();
        return;
    }
    if (event == 4) { // death
        if (is_authority) {
            apply_transition(e, world);
            return;
        }
        if (is_in_session) {
            e.health = 0; // [orig: *(int16*)(entity+286) = 0 @0x45827f, before the death tick] so the
                          // dispatched tick takes its death path (not the alive path) on a death event
            row(b.f[AiBrain::kCurState]).tick(ctx);
            AiEventEntry ev{};
            ev.f[0] = 20;
            ev.f[1] = 9 | (ai_index << 16);
            ev.set_timer(0.0f);
            events.queue(ev);
            finish();
            return;
        }
        int32_t pend = b.f[AiBrain::kPendState];
        if (pend == 7 || (pend > 12 && pend <= 15))
            apply_transition(e, world);
        return;
    }
}

void AiSystem::apply_transition(AiEntity &e, World &world) {
    AiBrain &b = e.brain;
    int32_t cur = b.f[AiBrain::kCurState];
    if (b.f[AiBrain::kPendState] != cur) {
        AiThinkCtx ctx{this, &e, &world, nullptr};
        row(cur).exit(ctx);                       // off_815240
        row(b.f[AiBrain::kPendState]).enter(ctx); // off_815238
        b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState];
    }
}

void AiSystem::tick(World &world, const TickContext &ctx) {
    // AI does not run during the BMS pre-mission script pass: that invocation only
    // settles initial scripted state (EventFlags PreMission), it does not step brains.
    if (ctx.pre_mission) return;
    is_authority = ctx.is_authority;
    scheduler.budget = 0; // per-frame budget reset (the staggering accumulator)
    // Drain the round sim's processed hits into the AI reaction stamps BEFORE any brain
    // updates: infantry get wasHit/lastAttacker (consumed by the §17.1 scan + §17.3 hit
    // reactions), SM brains get the type-1 damage AIEvent (h_combat_event -> evade).
    // [orig: the damage chain writes the victim entity + queues the event inline —
    // Projectile_ProcessDamageOnTarget @0x4e7fb0; our sim/AI split drains a record.]
    for (const RoundHit &hit : world.round_sim.hits) {
        AiEntity *victim = for_handle(hit.victim);
        if (victim == nullptr) continue;
        if (victim->inf.active) {
            // Every ordinary damage callback alerts an NPC and its trigger group,
            // before lethal/nonlethal handling. The player flag skips this AI leg.
            // [orig: Entity_HandleDamageTrigger @0x4073c8..0x4073ea]
            const Entity *victim_entity = world.registry.get(victim->handle);
            if (victim_entity != nullptr &&
                (victim_entity->engine_flags & 0x100u) == 0) {
                victim->slot.bytes()[AiSlot::kMoveFlagByte] = 2;
                world.relations.group(victim_entity->group_id).alert =
                        TriggerRelations::kAlertRed;
            }
            // Self-damage does not stamp a reaction or attacker. Retail tests the
            // timer against 25, then adds 10 without clamping (24 becomes 34).
            // [orig: Entity_OnDamageReceived @0x4af859..0x4af878]
            if (hit.shooter != victim->handle) {
                victim->inf.was_hit = true;
                if (victim->inf.damage_timer < 25) victim->inf.damage_timer += 10;
                victim->inf.last_attacker = hit.shooter;
            }
        } else {
            AiEventEntry ev{};
            ev.f[0] = 1; // damage
            ev.f[1] = (index_of(*victim) << 16);
            ev.f[3] = hit.damage; // -> brain[39] kDamageInfo via h_combat_event
            ev.set_timer(0.0f);
            events.queue(ev);
        }
    }
    world.round_sim.hits.clear();
    // Rebuild the collision proximity tables once per tick, before any entity update.
    // [orig: Entity_UpdateAllEntities @0x4c2100 -> Entity_BuildAllProximityLists
    // @0x4c20f0 (pool-2 statics + pool-0/1 snapshots) + Entity_BuildProximityListsFromPools
    // @0x4b8eb0 (per-entity candidate slices)]
    // Pool-0 person publication does not depend on any entity having a 3DI
    // collision instance.  RoundSim still queries this snapshot on missions
    // containing only organic entities, so always rebuild the pool tables when
    // a CollisionWorld is installed.  Model-backed blink work remains gated on
    // instance_count below.
    const bool collision_active = collision != nullptr && collision->instance_count() != 0;
    if (collision != nullptr) {
        collision->local_player = world.cached.local_player; // blink accumulation target
        collision->build_tick_tables(world);
    }
    // The loop runs on a JOINER (client, !is_authority) too: tick_infantry's §5.38
    // entity==g_local_player branch (line below, no authority guard) motor-sims the
    // joiner's own player from input, while NPC think/select stays authority-gated, so
    // local-promote copies of remote entities just hold (idle). The joiner renders every
    // REMOTE entity's pose from the host's S2C 0x0A (present reads ClientState, not these
    // local copies), so their idle ticking is harmless. [orig: the client also runs the
    // per-entity AI tick; Entity_UpdateInfantryAI @0x4b9910 simulate-when entity==local.]
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        if (e.inf.active) {
            // Net-snapped peers do not locally simulate their body, but retain the
            // existing seat-follow presentation phase. Simulated infantry enters the
            // full retail body tick; its mounted return suppresses locomotion only.
            if (e.net_is_remote_peer && pose_if_mounted(e, world)) {
                advance_part_anim(e);
                continue;
            }
            // Net-snapped remote peers skip the movement motor, so their blink/indoors
            // state comes from the position-only refresh instead. [orig: remote persons
            // refresh via the net position/create handlers — NapiNPClientMsg_0x00F
            // @0x42e442, NetPacket_HandleEntityCreate @0x42f227; the @0x4c229c per-tick
            // walk is pool-2 statics on an 8-per-tick stagger, not persons]
            if (collision_active && e.net_is_remote_peer) {
                if (Entity *ent = world.registry.get(e.handle))
                    collision->refresh_blink(world, *ent);
            }
            // org1-class soldier: the infantry motor replaces the vehicle SM + kinematic
            // locomotion for this entity. [orig: g_EntityClassPhysicsTable row "org1" ->
            // Entity_UpdateInfantryAI @0x4b9910]
            tick_infantry(e, world, ctx.logic_tick);
            continue;
        }
        // Non-infantry mounted controllers retain the seat-follow shortcut.
        if (pose_if_mounted(e, world)) {
            advance_part_anim(e);
            continue;
        }
        if (begin_update(e)) {
            process_infantry_state_machine(e, world, 0);
            // Motor-driven vehicles (items.def physics selector non-zero) integrate through
            // tick_vehicle_motor below — the SM stays their decision layer (waypoints,
            // visited bits, states) but the kinematic locomotion model retires for them
            // [orig: one entity update — the SM never integrates ground vehicles, the
            // physics does; Entity_DispatchPhysics_cveh @0x48efc0].
            const Entity *ent = world.registry.get(e.handle);
            const VehicleTraits *vt =
                    ent != nullptr ? world.vehicle_traits.get(ent->item_id) : nullptr;
            const bool motor_driven = vt != nullptr && vt->physics != 0;
            if (locomotion_enabled && !motor_driven) {
                apply_locomotion(e);   // horizontal: advance pos[0]/pos[1] toward the node
                apply_ground_clamp(e); // vertical: snap pos[2] onto the terrain (no-op if unwired)
            }
        }
        advance_part_anim(e); // part-anim channels integrate independent of the AI budget gate
    }
    // Ground-vehicle motor pass: every pool-1 entity with vehicle traits (items.def
    // `physics` selector non-zero) runs the drive core — consuming a mounted ctrl/drvr
    // player's replicated input on the authority. AUTHORITY-ONLY here: a joiner's local
    // copies are wire-posed (the vehicle compact record read side), and the driver's
    // client-side prediction leg is the retail client's concern, not this host loop's.
    // [orig: the per-class tick from Entity_UpdateAllEntities -> Entity_DispatchPhysics_cveh
    // @0x48efc0 -> Entity_UpdateVehiclePhysics @0x48af00; authority drive gate @0x48b0ff]
    if (is_authority && !world.vehicle_traits.empty()) {
        vehicle_pass_handles_.clear();
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != 1) return;
            if (world.vehicle_traits.get(e.item_id) == nullptr) return;
            vehicle_pass_handles_.push_back(e.handle);
        });
        for (const EntityHandle h : vehicle_pass_handles_) {
            Entity *veh = world.registry.get(h);
            if (veh == nullptr) continue;
            const VehicleTraits *traits = world.vehicle_traits.get(veh->item_id);
            if (traits == nullptr) continue;
            // Stage the drive input class the motor will consume: a live PLAYER controller
            // keeps the occupant leg; an AI controller (or none) routes through the brain
            // (state stamps + the witnessed steer/speed leg). [orig: the occupant class
            // switch inside Entity_UpdateVehiclePhysics @0x48b949-0x48c034]
            VehicleDriveCmd cmd;
            if (traits->player_control) {
                Entity *ctrl = resolve_vehicle_controller(world, *veh);
                // A DEAD controller parks the vehicle. The infantry death edge detaches
                // first; this guard preserves the same result if the vehicle pass happens
                // to observe the controller earlier in the frame.
                // [orig: infantry death detach @0x4b9c57..0x4b9c60]
                const bool ctrl_alive =
                        ctrl != nullptr && ctrl->alive && ctrl->health > 0;
                const bool player_ctrl = ctrl_alive && ctrl->handle.pool() == 0 &&
                                         ctrl->player_class != 0;
                if (player_ctrl) {
                    // A player drive freezes the SM mover exactly like the parked leg —
                    // the route never advances under a human driver [orig: the player
                    // leg forces SM state 22 too @0x48b993].
                    if (AiEntity *ve = for_handle(h)) {
                        ve->brain.f[AiBrain::kCurState] = 22;
                        ve->brain.f[AiBrain::kPendState] = 22;
                    }
                } else {
                    vehicle_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr, *traits,
                                     cmd);
                }
            }
            tick_vehicle_motor(world, *veh, *traits, &cmd);
            // Mirror the integrated transform back into the brain entity — one struct in
            // the original; the SM mover and the present snapshot read pos[]/heading.
            if (AiEntity *ve = for_handle(h)) {
                ve->pos[0] = to_fixed(veh->position.x);
                ve->pos[1] = to_fixed(veh->position.y);
                ve->pos[2] = to_fixed(veh->position.z);
                ve->heading = veh->veh.yaw_seeded
                        ? veh->veh.yaw_bam
                        : bam_heading_from_mission_yaw_deg(static_cast<double>(veh->yaw));
            }
        }
    }
    events.process_timed(*this, world);
}

void AiSystem::pump_mounted_weapon_slots(World &world, uint32_t logic_tick) {
    mounted_weapon_handles_.clear();
    world.registry.for_each([&](const Entity &entity) {
        if (entity.handle.pool() == 1 && entity.primary_weapon_owner.valid())
            mounted_weapon_handles_.push_back(entity.handle);
    });

    for (const EntityHandle mount_handle : mounted_weapon_handles_) {
        Entity *mount = world.registry.get(mount_handle);
        if (mount == nullptr) continue;
        Entity *owner = world.registry.get(mount->primary_weapon_owner);
        if (owner == nullptr || owner->health <= 0 || !owner->mounted ||
            owner->mount_type != SeatType::Gunner ||
            owner->mount_target != mount_handle) {
            mount->primary_weapon_owner = EntityHandle{};
            continue;
        }
        // L's borrowed parent slot is pumped by NovaSimulation with the live
        // trigger/reload/scope inputs and first-person event sink. Advancing it
        // here as well would run one slot twice per frame. Remote players and
        // NPC gunners remain owned by this global world pump.
        // [orig: one WeaponAction_ProcessAllEntities walk @0x542690]
        if (world.external_local_mounted_weapon_pump &&
            owner->handle == world.cached.local_player)
            continue;
        AiEntity *gunner = for_handle(owner->handle);
        if (gunner == nullptr || mount->primary_weapon_slot_adm == 0xFF) continue;
        const WeaponTableEntry *weapon =
                world.weapons.by_index(mount->primary_weapon_slot_adm);
        if (weapon == nullptr || weapon->ammo_index < 0) continue;

        WeaponFsmInputs inputs;
        inputs.is_local = owner->handle == world.cached.local_player;
        inputs.is_authority = is_authority;
        inputs.auto_reload = true;
        // The heat window is derived from the tick, so the pump needs it. AI gunners
        // sit on the emplaced guns that actually author heat, so this is the path
        // that overheats in practice. [orig: current_tick @ 0x24C1968]
        inputs.current_tick = static_cast<int32_t>(logic_tick);
        WeaponFsmEvents weapon_events;
        weapon_fsm_tick(weapon->action_fsm, mount->primary_weapon_slot,
                        inputs, weapon_events);
        if (!weapon_events.fired || !is_authority) continue;

        // The slot owner is the gunner, while its def/ammo live on the parent.
        // Use the live chased look and the freshest posed muzzle available; the
        // fallback is the mounted occupant's chest/seat origin.
        // [orig: slot owner path in WeaponAction_Fire @0x542b10;
        //  Entity_CalcWeaponFirePosition parentSlot 3]
        int32_t origin[3] = {gunner->pos[0], gunner->pos[1],
                             gunner->pos[2] + 0xE666};
        if (mount->posed_muzzle_valid &&
            logic_tick - mount->posed_muzzle_tick <= 4u) {
            origin[0] = mount->posed_muzzle_world[0];
            origin[1] = mount->posed_muzzle_world[1];
            origin[2] = mount->posed_muzzle_world[2];
        } else if (gunner->muzzle_valid &&
                   logic_tick - gunner->muzzle_tick <= 4u) {
            origin[0] = gunner->muzzle_world[0];
            origin[1] = gunner->muzzle_world[1];
            origin[2] = gunner->muzzle_world[2];
        }
        if (fire_ai_round(world, *gunner, origin, gunner->heading,
                          gunner->pitch, weapon->ammo_index))
            gunner->inf.aim_ref0 = gunner->inf.combat_target;
    }
}

bool AiSystem::pose_if_mounted(AiEntity &e, World &world) {
    Entity *occ = world.registry.get(e.handle);
    // A dead occupant cannot enter the live mounted-pose path: it must reach the
    // infantry death edge, detach, then consume its staged death animation.
    // [orig: Entity_UpdateInfantryAI @0x4b9960..0x4b9983]
    if (occ == nullptr || !occ->mounted || occ->health <= 0) return false;
    Entity *veh = world.registry.get(occ->mount_target);
    if (veh == nullptr) {              // vehicle gone -> auto-dismount, resume normal AI
        entity_detach_from_vehicle(world, e.handle);
        return false;
    }
    if (occ->mount_seat < 0 || occ->mount_seat >= static_cast<int>(veh->seats.size())) return false;
    const Seat &seat = veh->seats[occ->mount_seat];
    // Local input owns LOOK before retail evaluates the parent UseGun bone. Our
    // split AiEntity keeps that input in the infantry latch until the mounted
    // branch, so expose it before the host asks for the live parent pose.
    if (e.inf.active && e.inf.is_local_player) {
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    }
    const int32_t saved_look_heading = e.heading;
    const int32_t saved_look_pitch = e.pitch;
    const auto apply_resolved_seat_frame = [&]() {
        pose_mounted_occupant(world, *occ, *veh, seat);
        // Capture the resolved seat orientation before an independent LOOK mirror
        // overwrites registry yaw. Keep the witnessed integer yaw conversion here:
        // the generic degree helper rounds differently at non-cardinal headings.
        const int16_t seat_yaw = occ->yaw;
        const int16_t seat_pitch = occ->pitch;
        const int16_t seat_roll = occ->roll;
        const int32_t resolved_heading = static_cast<int32_t>(
                static_cast<int64_t>(90 - seat_yaw) * kBamPerDegreeInt);
        if (e.inf.active) {
            // Mirror both the direct seat-frame writes and the carried-infantry leg chase
            // snap so render and per-section collision consume one coherent body frame.
            // [orig: seat carry @0x4b654e-0x4b6575; carried body/leg snap Flags & 0x100060]
            e.inf.body_heading = resolved_heading;
            e.inf.leg_yaw[0] = resolved_heading;
            e.inf.leg_yaw[1] = resolved_heading;
            e.inf.leg_target[0] = resolved_heading;
            e.inf.leg_target[1] = resolved_heading;
            e.body_pitch = bam_from_degrees_wrapped(static_cast<double>(seat_pitch));
            e.roll = bam_from_degrees_wrapped(static_cast<double>(seat_roll));
        }
        // Organics present from AiEntity.pos, not Entity.position.
        e.pos[0] = to_fixed(occ->position.x);
        e.pos[1] = to_fixed(occ->position.y);
        e.pos[2] = to_fixed(occ->position.z);
        return resolved_heading;
    };
    const int32_t seat_heading = apply_resolved_seat_frame();
    if (e.inf.active && e.inf.is_local_player) {
        // The mounted LOCAL player keeps the LOOK as its entity yaw: the witnessed mounted
        // carry writes bodyHeading/headLook from the seat bone but leaves entity->Yaw
        // player-owned — the drive motor reads it as the mouse-steer target
        // [orig: Entity_UpdateInfantryPlayerBody mounted leg (bodyHeading only) +
        //  Entity_UpdateVehiclePhysics steer source @0x48ba59/v61->Yaw].
        occ->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(e.inf.target_heading))));
        // Mirror the live inputs into the wire fields the motor consumes — tick_infantry's
        // per-tick mirror is skipped while mounted (one entity struct in the original; the
        // split is ours) [orig: the MoveOrder packer @0x4df68f-0x4df741].
        occ->net_move_input = static_cast<uint8_t>(
                (e.inf.player_move_dir_index & 7) | (e.inf.player_moving ? 8 : 0) |
                (e.inf.lean_left ? 0x40 : 0) | (e.inf.lean_right ? 0x80 : 0));
        occ->net_stance_bits = 0; // seated stance stays cleared [orig: @0x435c42]
        // Drop any pending jump: the input latch is set-only (its consumer is
        // tick_infantry's jump block, skipped for the whole ride) and the witnessed
        // mounted carry scrubs the move flags every tick — a seated player cannot
        // bank a jump for dismount [orig: the mounted-leg flag scrub &= 0xFF8F57DF].
        e.inf.jump_requested = false;
    }
    // Remote occupants present in the captured seat frame. The local LOOK override
    // below remains player-owned and must not rotate the carried body/collision pose.
    e.heading = seat_heading;
    if (e.inf.active && e.inf.is_local_player) {
        // The seated LOOK stays mouse-instant at FULL precision: retail drives entity
        // Yaw/Pitch straight from input regardless of mount (the mounted body leg
        // writes bodyHeading/headLook from the bone, never the look) — the camera
        // reads these mirrors, and the whole-degree occ->yaw roundtrip above (the
        // wire/motor mirror) must not quantize or freeze it. tick_infantry's own
        // mirrors (its lines `e.heading = inf.target_heading` / `e.pitch =
        // inf.look_pitch`) are skipped for the whole ride.
        // [orig: Input_HandleActionBinding_0 @0x4e1330 writes entity+0x10/+0x14;
        //  §23.5 — the entity Yaw is the LOOK, player-owned while seated]
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    } else if (e.inf.active && seat.type == SeatType::Gunner) {
        // Attachment writes the seat/base pose but restores the child's independent
        // live look. The look then chases the desired solution instead of snapping:
        // yaw quarter-step clamped to +/-0x02000000, pitch eighth-step. Most mount
        // configs also constrain look to +/-90 degrees around the attached base.
        // [orig: save/restore @0x546416..0x546664; chase @0x4bef57..0x4bef97;
        //  base-relative clamp @0x4bef9a..0x4beff0]
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        if (e.inf.aim_valid) {
            int32_t yaw_step = io::bam_sar(
                    io::bam_add(io::bam_sub(e.inf.aim_heading, e.heading), 2), 2);
            yaw_step = std::clamp(yaw_step, -0x02000000, 0x02000000);
            e.heading = io::bam_add(e.heading, yaw_step);
            const int32_t pitch_step = io::bam_sar(
                    io::bam_add(io::bam_sub(e.inf.aim_pitch, e.pitch), 4), 3);
            e.pitch = io::bam_add(e.pitch, pitch_step);
        }
        const int cfg = veh->emplaced_config;
        const bool wide_mount = veh->emplaced_config_valid &&
                (cfg == 3 || cfg == 4 || cfg == 5 || cfg == 7);
        if (!wide_mount) {
            const int32_t rel = io::bam_sub(e.heading, seat_heading);
            if (rel > 0x40000000)
                e.heading = io::bam_add(seat_heading, 0x40000000);
            else if (rel < -0x40000000)
                e.heading = io::bam_sub(seat_heading, 0x40000000);
        }
        // The chase above changes the semantic EWEAP controls consumed by a
        // model-aware provider. Resolve once more so the NPC root/body and the
        // parent gun presented after this tick use the same Hn+1 control phase.
        // Keep the first frame as the existing clamp base and preserve LOOK as
        // the child's independent heading/pitch after the second seat resolve.
        if (e.heading != saved_look_heading || e.pitch != saved_look_pitch) {
            const int32_t chased_look_heading = e.heading;
            const int32_t chased_look_pitch = e.pitch;
            apply_resolved_seat_frame();
            e.heading = chased_look_heading;
            e.pitch = chased_look_pitch;
        }
        occ->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(e.heading))));
    }
    if (e.inf.active) {
        const int mounted_state = mounted_anim_state_for_seat(*veh, seat, e.inf, root_motion);
        if (e.inf.anim_state != mounted_state) {
            e.inf.anim_prev = e.inf.anim_state;
            e.inf.anim_state = mounted_state;
            e.inf.clip_phase = 0;
        }
        e.inf.anim_pending = 0;
        e.inf.move_mode = 0;
        e.inf.target_dist = 0;
        occ->body_anim_slot = body_anim_slot_from_state(e.inf.anim_state);
        mirror_wire_anim(e, world); // seat-state anim bytes reach the 0x0A player record too
    }
    return true;
}

void AiSystem::advance_part_anim(AiEntity &e) {
    AiBrain &b = e.brain;
    for (int slot = 0; slot < 2; ++slot) {
        int32_t dir = b.f[AiBrain::kPartAnimDir0 + slot];
        int32_t rate = b.f[AiBrain::kPartAnimRate0 + slot];
        if (dir == 0 || rate == 0) continue; // play_type 0 (stop) freezes the sweep
        int64_t phase = static_cast<int64_t>(b.f[AiBrain::kPartAnimPhase0 + slot]) +
                        static_cast<int64_t>(rate) * dir;
        if (phase < 0) phase = 0;
        if (phase > 65535) phase = 65535; // clamp endpoints (one-shot; wrap is a def-idle case)
        b.f[AiBrain::kPartAnimPhase0 + slot] = static_cast<int32_t>(phase);
    }
}

// [orig: Entity_ApplyCommand @0x43ab60] See the header. Only case 0x22 (PLAYPARTANIM) is ported.
void ai_apply_command(AiBrain &comp, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    switch (sub_type) {
        case 0x22: { // PLAYPARTANIM: p2=ANIMNUM(channel), p3=ANIMPLAYTYPE, p4=ANIMTIME(16.16 s)
            const int channel = p2;
            if (channel != 1 && channel != 2) return;              // only channels 1,2 act
            const int play_type = p3;
            if (static_cast<unsigned>(play_type + 1) > 2u) return; // play_type in {-1,0,1}
            const int slot = channel - 1;
            // rate = (0.016 / seconds) * 65536 phase-units/tick, min 1. [flt_7C3310=1/65536,
            // flt_7C3B40=0.016, flt_7C32BC=65536.] The original computes `base` unconditionally, so
            // ANIMTIME==0 -> base 0.0 -> 0.016/0.0 = +inf, and the x87 ftol of infinity is the
            // integer-indefinite 0x80000000 (INT_MIN) -- nonzero, so the min-1 guard does NOT fire.
            // That makes ANIMTIME==0 saturate the part to a clamp endpoint on the first tick (matching
            // the GDScript host's 1e9 instant-saturation). Replicated here rather than short-circuited.
            const double seconds = static_cast<double>(p4) / 65536.0; // base; p4==0 -> 0.0
            const double rate_f = (0.016 / seconds) * 65536.0;        // +inf when p4==0
            int32_t rate;
            if (rate_f != rate_f || rate_f >= 2147483648.0 || rate_f < -2147483648.0) {
                rate = static_cast<int32_t>(0x80000000); // ftol integer-indefinite (inf/NaN/overflow)
            } else {
                rate = static_cast<int32_t>(rate_f);      // truncate toward zero
            }
            if (rate == 0) rate = 1;                      // min-1 guard (does NOT fire for INT_MIN)
            comp.f[AiBrain::kPartAnimDir0 + slot] = play_type;  // comp+436+4*slot (direction)
            comp.f[AiBrain::kPartAnimRate0 + slot] = rate;      // comp+444+4*slot (rate)
            break;
        }
        case 29:   // COMBATSPEED -> kSpeedA (brain +196)
        case 30: { // PATROLSPEED -> kSpeedB (brain +200)
            // [orig: Entity_ApplyCommand @0x43ab60 cases 0x1D/0x1E queue AIEvent types
            // 10/11 -> AI_HandleCommand @0x465770 cases 0xA/0xB — km/h to 16.16 u/tick:
            // fild(value) (+2^32 when negative = the unsigned reinterpret) * 1000
            // * (1/225000) * 65536 = x65536/225 (the exact 62.5 Hz conversion; the
            // items.def parse's x293 is its integer approximation).]
            double v = static_cast<double>(p2);
            if (v < 0.0) v += 4294967296.0; // flt_7C3288 add on negative [orig: @0x465974]
            const int32_t scaled =
                    static_cast<int32_t>(v * 1000.0 * 4.444444584805751e-06 * 65536.0);
            comp.f[sub_type == 29 ? AiBrain::kSpeedA : AiBrain::kSpeedB] = scaled;
            break;
        }
        default:
            // Tracked-TODO: alert(5/6/0x16), accuracy(8), AISETSTATE(0x1C),
            // etc. (notes/mission/anim-ai-grill-2026-06-07.md). No-op so an unported sub-type
            // can't corrupt the AI component.
            break;
    }
}

void AiSystem::capture_spawn_baseline() {
    spawn_baseline_ = entities_;
    baseline_captured_ = true;
}

// Per-mission re-init seam: World::restore() calls load_systems() on an editor Play->Stop,
// which reaches this. Rewind every brain (position/heading/state/timers) to the captured
// spawn baseline and drop the transient queues, so a simulate/stop cycle leaves the authored
// mission clean. The nav table is read-only path data and is left intact.
void AiSystem::on_load(World &) {
    if (baseline_captured_) entities_ = spawn_baseline_;
    rebuild_handle_index();
    events.clear();
    scheduler.budget = 0;
    relmat_calls.clear();
    rel_ops.clear();
    target_set_calls.clear();
    scan_candidates_.clear();
    unported_calls = 0;
    find_target_calls = 0;
    fire_shot_seq = 0;
}

// [orig: Entity_CalcAverageGroundHeight @0x457230] 5-tap weighted ground height (16.16 fixed).
// The taps sample the bilinear terrain column at center + N/S/E/W at sample_radius; the original
// per-tap sampler is the hi-res down-raycast @0x60e710 (its near-vertical result == this column
// height — tracked deviation). Engine ground plane is (X,Y) = (pos[0],pos[1]); the renderer-loaded
// atlas is sampled in Godot coords (x, -y), and the N/S/E/W taps are symmetric so the Y sign only
// renames which tap is "north". Height comes back in world units; ground_16.16 = units * 65536.
int32_t calc_average_ground_height(const terrain::TerrainHeightField &field, const int32_t pos[3],
                                   int32_t sample_radius, const GroundClearance &clearance) {
    if (!field.valid()) return INT32_MIN; // [orig: terrain assumed present; null -> no ground]

    auto sample = [&](int32_t x16, int32_t y16) -> int32_t {
        const float wx = static_cast<float>(x16) / 65536.0f;
        const float wz = -static_cast<float>(y16) / 65536.0f; // engine Y -> Godot z
        const float h = terrain::height_field_height_world_bilinear(field, wx, wz);
        return static_cast<int32_t>(h * 65536.0f);
    };

    int32_t result;
    int32_t center;
    if (sample_radius) {
        int32_t maxHeight = 0;                                       // [orig: maxHeight = 0]
        const int32_t north = sample(pos[0], pos[1] + sample_radius); // sub_4142C0(e,0,+r)
        if (north > 0) maxHeight = north;                           // [orig: if(north>0) max=north]
        const int32_t south = sample(pos[0], pos[1] - sample_radius); // sub_4142C0(e,0,-r)
        if (south > maxHeight) maxHeight = south;
        const int32_t east = sample(pos[0] + sample_radius, pos[1]);  // sub_414320(e,+r,0)
        if (east > maxHeight) maxHeight = east;
        const int32_t west = sample(pos[0] - sample_radius, pos[1]);  // sub_4142C0(e,-r,0)
        if (west > maxHeight) maxHeight = west;
        center = sample(pos[0], pos[1]);                             // sub_414320(e,0,0)
        if (center > maxHeight) maxHeight = center;
        result = (north + south + east + west + 2 * (center + 2 * maxHeight)) / 10;
        if (result < center) result = center;                        // [orig: clamp >= center]
    } else {
        result = sample(pos[0], pos[1]);                             // [orig: radius 0 -> center only]
    }

    // [orig: if (*(entity+368) && (int)worldY > result) result = worldY] water-surface clamp.
    if (clearance.has_physics && field.has_water && field.water_y > result) {
        result = field.water_y;
    }
    // [orig: def offset: dead path uses def+0x30, else def+0x2C; gated by entityDef != 0.]
    result += clearance.use_dead ? clearance.dead_offset : clearance.alive_offset;
    return result;
}

// Drive the vertical off the terrain sampler. See the header. Snap model for the un-reversed
// vertical driver: SET kWorkPosZ + the entity's pos[2] to ground + ground_stand_offset.
void AiSystem::apply_ground_clamp(AiEntity &e) {
    if (terrain == nullptr) return;
    GroundClearance clearance = ground_clearance;
    clearance.has_physics = e.has_physics;                    // [orig: entity+368 gate]
    clearance.use_dead = (e.health <= 0);                     // [orig: health<=0 dead path]
    const int32_t ground = calc_average_ground_height(*terrain, e.pos, 0x50000, clearance);
    if (ground == INT32_MIN) return;                          // no terrain coverage -> leave Z
    const int32_t z = ground + ground_stand_offset;           // [orig: brain[131] = ground + 0x50000]
    e.brain.f[AiBrain::kWorkPosZ] = z;                        // mover output field stays faithful
    e.pos[2] = z;                                             // snap the entity onto the ground
}

// Kinematic locomotion over the mover output. The brain decides a target (kWorkPos*), a heading
// (kWorkHeading, BAM) and a speed (kOutSpeed); here we turn to that heading and advance the entity
// toward the target by kOutSpeed * loco_scale, clamped so we never overshoot. INTERIM MODEL being
// replaced: the original routes organics through the infantry motor [orig: Entity_UpdateInfantryAI
// @ 0x4b9910] (anim-driven root motion; ground vehicles consume this SM's output instead) — see
// docs/world/world-wac-ai-re.md §3 for the full spec. Out-speed 0 leaves the entity put.
void AiSystem::apply_locomotion(AiEntity &e) {
    AiBrain &b = e.brain;
    int32_t speed = b.f[AiBrain::kOutSpeed]; // brain[128]
    if (speed <= 0) return;
    e.heading = b.f[AiBrain::kWorkHeading];  // brain[132] (snap; turn-rate physics deferred)
    int64_t stepd = static_cast<int64_t>(speed) * loco_scale; // AI units -> 16.16 world delta
    int64_t dx = static_cast<int64_t>(b.f[AiBrain::kWorkPosX]) - e.pos[0]; // brain[129]
    int64_t dy = static_cast<int64_t>(b.f[AiBrain::kWorkPosY]) - e.pos[1]; // brain[130]
    double dist = std::sqrt(static_cast<double>(dx) * static_cast<double>(dx) +
                            static_cast<double>(dy) * static_cast<double>(dy));
    if (dist == 0.0 || dist <= static_cast<double>(stepd)) {
        e.pos[0] = b.f[AiBrain::kWorkPosX]; // arrive (clamp, no overshoot)
        e.pos[1] = b.f[AiBrain::kWorkPosY];
    } else {
        double f = static_cast<double>(stepd) / dist;
        e.pos[0] += static_cast<int32_t>(static_cast<double>(dx) * f);
        e.pos[1] += static_cast<int32_t>(static_cast<double>(dy) * f);
    }
}

// ----------------------------------------------------------------------------
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
    const double a = static_cast<double>(bam) * (3.14159265358979323846 / 2147483648.0);
    return static_cast<int32_t>(std::cos(a) * 4194304.0);
}

// See ai.h — the vehicle-physics AI/parked input staging. [orig: Entity_UpdateVehiclePhysics
// @0x48af00: parked @0x48c002-0x48c02d, AI-driver leg @0x48bc12-0x48c034]
void AiSystem::vehicle_ai_drive(World &world, Entity &veh, const Entity *controller,
                                const VehicleTraits &traits, VehicleDriveCmd &out) {
    AiEntity *ve = for_handle(veh.handle);
    if (ve == nullptr) return; // no brain: the motor's own no-controller hold stands in
    AiBrain &b = ve->brain;

    const bool wrecked = veh.health <= 0 || !veh.alive;
    if (controller == nullptr || wrecked || (veh.flags & 0x2u) != 0) {
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

    // The pool-1 avoid BRAKE [orig: @0x48bd8f-0x48bf26]: for every pool-1
    // neighbor whose heading-aware footprint ellipse overlaps ours (+1.0 u)
    // AND that sits within ~30 deg of dead ahead, the command speed multiplies
    // by an id/frame-keyed factor in [0.25, 0.75) per tick — vehicles brake
    // behind obstacles; deflecting off them through the hull contact was never
    // the retail path-follow behavior.
    {
        const int32_t self_bound = to_fixed(veh.bound_radius);
        const int32_t sx = to_fixed(veh.position.x);
        const int32_t sy = to_fixed(veh.position.y);
        const int32_t sz = to_fixed(veh.position.z);
        const size_t cap = world.registry.pool_capacity(1);
        for (size_t si = 0; si < cap; ++si) {
            const Entity *o =
                    world.registry.get(EntityHandle::make(1, static_cast<int>(si)));
            if (o == nullptr || o->handle == veh.handle) continue; // [orig: @0x48be19]
            const int32_t ob = to_fixed(o->bound_radius);
            if (ob <= 0) continue; // [orig: the pool-walk live gate @0x48bdd9]
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
    }
    out.ai_drive = true;
    out.cmd_speed = cmd_speed;
    out.steer_target_bam = heading + delta + (delta >> 3); // [orig: @0x48bd7f]
    // The wait-for-boarders stop @0x48bf6f-0x48bff9 (a full stop while any live
    // unmounted pool-0 entity runs the boarding think toward THIS vehicle —
    // brain mode 125 + the vehicle id; moot until the AI boarding think lands),
    // the handbrake byte-973 latch @0x48c03a and the aim-lock stop @0x48c086
    // stay tracked deferrals (D-NET-161).
}

// ----------------------------------------------------------------------------
// P2: GROUND combat + targeting.
// ----------------------------------------------------------------------------

int AiSystem::index_of(const AiEntity &e) const {
    return static_cast<int>(&e - entities_.data());
}

// [orig: inline LCG on dword_31BFBB8] / [orig: PRNG_Next16 @0x6130a0 on dword_31BFBB0]. Both are
// the same rotate-LCG over different global state; the low 16 bits feed the %62 fire-delay jitter.
int32_t AiSystem::prng_step_a() { return static_cast<int32_t>(prng_step(prng_a)); }
int32_t AiSystem::prng_step16() { return static_cast<int32_t>(prng_step(prng16)); }

// [orig: the shared death-velocity event @0x467730/0x457d70/0x467400] queue a crash(3)/still(4)
// AIEvent by horizontal speed. Channel 0, entity index, timer 0 (the orig stores fldz to var_C).
void AiSystem::queue_death_event(AiEntity &e) {
    double sp = std::sqrt(static_cast<double>(e.vel_x) * e.vel_x +
                          static_cast<double>(e.vel_z) * e.vel_z);
    if (sp > kDeathSpeedClamp) sp = kDeathSpeedClamp; // flt_7C19E0 min-clamp
    int32_t isp = static_cast<int32_t>(sp);
    AiEventEntry ev{};
    ev.f[0] = (isp >= 1057) ? 3 : 4;     // crash/ragdoll vs still death
    ev.f[1] = (index_of(e) << 16);       // channel 0 | entity index
    ev.set_timer(0.0f);
    events.queue(ev);
}

// The candidate FEED (D-AI-1, witnessed world-wac-ai-re §16.2): build the eligible list
// from the registry pools with the witnessed gates, then run the byte-exact scoring core.
// [orig: AI_FindBestTargetB @0x466f60 iterates g_pool_list in-function per profile
// weapon-slot class (+40+4i -> pools 0/1/2 with the building sub-filter).] Tracked
// deviations (ledger D-AI-1): the class table comes from the unparsed ai.def profile, so
// pools 0 and 1 scan unconditionally; the priority target (profile+148) and the building
// class remain unmodeled; LOS = line_of_sight_clear (terrain leg, D-AI-7).
bool AiSystem::acquire_target(World &world, AiEntity &e, AiTarget &out) {
    scan_candidates_.clear();
    // The teamless entry gate lives in the scoring core (it is part of @0x466f60);
    // mirrored here only to skip the wasted pool walk.
    if (e.team == 0 && !e.see_all) return acquire_target_from(e, scan_candidates_, out);
    for (int pool = 0; pool <= 1; ++pool) {
        const size_t cap = world.registry.pool_capacity(pool);
        for (size_t s = 0; s < cap; ++s) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            const Entity *c = world.registry.get(h);
            if (c == nullptr) continue;                    // [orig: +28 in-use]
            if (h == e.handle) continue;                   // not self
            if (c->health <= 0) continue;                  // [orig: +286 > 0]
            // Team gate [orig: teamless candidates need the attacker's 0x200; same-team
            // skipped unless 0x200].
            if ((c->team == 0 || c->team == e.team) && !e.see_all) continue;
            AiCandidate cand;
            cand.handle = h;
            cand.pos[0] = static_cast<int32_t>(c->position.x * 65536.0f);
            cand.pos[1] = static_cast<int32_t>(c->position.y * 65536.0f);
            cand.pos[2] = static_cast<int32_t>(c->position.z * 65536.0f);
            cand.team = c->team;
            cand.flags = static_cast<int32_t>(c->engine_flags); // &2/&0x8000000 skip, &0x4000 prio x6
            cand.health = static_cast<int16_t>(std::min<int32_t>(c->health, INT16_MAX));
            cand.visibility = c->ai_target_refcount;       // [orig: +530 refcount saturation]
            // Per-candidate engage-range caps [orig: words +422/+420] — def fields, not yet
            // modeled on Entity: uncapped (the profile's own range still gates).
            cand.range_primary = INT32_MAX / 2;
            cand.range_secondary = INT32_MAX / 2;
            cand.relmat_id = c->group_id;                  // group key (+0x11C commandGroup)
            cand.net_id = c->net_id;                       // single key (+0x7C SSN)
            cand.has_controller = (c->owner_connection_id != 0);
            cand.is_priority = false;                      // [orig: profile+148 — unmodeled]
            cand.los_blocked = !line_of_sight_clear(world, e.pos, cand.pos, e.handle, h);
            scan_candidates_.push_back(cand);
        }
    }
    return acquire_target_from(e, scan_candidates_, out);
}

// [orig: AI_FindBestTargetB @0x466f60 scoring walk] over an explicit candidate list.
// Per-candidate perception gates (team, flags, health, self, refcount saturation) then
// FOV/range/stealth scoring + LOS; the priority target bypasses scoring on clear LOS.
bool AiSystem::acquire_target_from(AiEntity &e, const std::vector<AiCandidate> &candidates,
                                   AiTarget &out) {
    ++find_target_calls;
    // Entry gate [orig: 0x466f60 head]: teamless scanners need the 0x200 see-all flag.
    if (e.team == 0 && !e.see_all) return false;
    const int primary_fov = e.profile.fov_primary | 1;     // [orig: (def+75)|1]
    const int secondary_fov = e.profile.fov_secondary | 1; // [orig: (def+67)|1]
    const AiCandidate *best = nullptr;
    int best_score = 0;
    for (const AiCandidate &c : candidates) {
        if (c.handle == e.handle) continue;                       // candidate != self
        if ((c.flags & 2) != 0) continue;                         // [orig: flags & 2 -> skip]
        if (c.health <= 0) continue;                              // [orig: *(cand+286) > 0]
        // [orig: (cand+530 <= 16 || def+100 & 8)] stealth pre-gate
        if (!(c.visibility <= 16 || (e.profile.flags100 & 8) != 0)) continue;
        if ((c.flags & 0x8000000) != 0) continue;                 // [orig: flags & 0x8000000 -> skip]
        // [orig: team check with self aiSlot+4 & 0x200 see-all]
        bool team_ok = (c.team != 0 || e.see_all) && (c.team != e.team || e.see_all);
        if (!team_ok) continue;

        int32_t bearing = bearing_bam(c.pos[1] - e.pos[1], c.pos[0] - e.pos[0]);
        int angle_diff = static_cast<int>(static_cast<uint32_t>(bearing - e.heading) >> 24);
        if (angle_diff >= 0x80) angle_diff = 256 - angle_diff;    // fold to [0,128]
        int dist = dist3d_units(c.pos, e.pos);

        // [orig: 0x46722e..0x467291] FOV/range gate FIRST. A candidate outside both gates jumps to
        // LABEL_17 (next candidate) and never reaches the priority bypass (0x467350) or the best-of
        // compare. ai_score_target returns -1 on gate-fail.
        int score = ai_score_target(angle_diff, dist, primary_fov, secondary_fov,
                                    e.profile.range_primary, e.profile.range_secondary,
                                    c.range_primary, c.range_secondary, c.visibility, c.flags);
        if (score < 0) continue;

        if (c.is_priority) { // [orig: 0x467350 — reached only past the gate] priority -> LOS-only bypass
            if (!c.los_blocked) { // Entity_CheckMutualLineOfSight @0x539be0
                out = AiTarget{c.relmat_id, c.net_id, c.has_controller, c.handle};
                return true;
            }
            continue; // priority but LOS blocked -> skip (no best-of, matches the orig else-if)
        }
        if (score > best_score && !c.los_blocked) { // [orig: else if (score > best_score) + LOS]
            best_score = score;
            best = &c;
        }
    }
    if (best) {
        out = AiTarget{best->relmat_id, best->net_id, best->has_controller, best->handle};
        return true;
    }
    return false;
}

// The acquisition/fire relation quads, APPLIED (D-AI-3 closed 2026-07-16). Keys:
// group = commandGroup (entity+0x11C -> Entity::group_id), single = the authored SSN
// (entity+0x7C DcbId -> Entity::net_id). Witnessed order + arg orientation from the raw
// call block: sees {GG[selfG][tgtG], SG[selfSsn][tgtG], GS[tgtSsn][selfG] (transposed),
// SS[selfSsn][tgtSsn]} then targeted the same; setter->matrix identity pinned via the
// g_SeesMatrix*/g_TargetedMatrix* bases. [orig: @0x4677b3..0x4678b2 / @0x4b0a6f..0x4b0ae2;
// world-wac-ai-re §16.4/§17.2; matrix map docs/mission/bms-event-runtime-re.md §3a]
void AiSystem::apply_engage_relations(World &world, const Entity &self, const Entity &target) {
    TriggerRelations &rel = world.relations;
    const int sg = self.group_id, ss = self.net_id;
    const int tg = target.group_id, ts = target.net_id;
    rel.set_group_group(TriggerRelations::kSees, sg, tg);       // [orig: @0x452a40 GG]
    rel.set_single_group(TriggerRelations::kSees, ss, tg);      // [orig: @0x452ad0 SG]
    rel.set_group_single(TriggerRelations::kSees, sg, ts);      // [orig: @0x452b60 GS]
    rel.set_single_single(TriggerRelations::kSees, ss, ts);     // [orig: @0x452bf0 SS]
    rel.set_group_group(TriggerRelations::kTargeted, sg, tg);   // [orig: @0x452aa0 GG]
    rel.set_single_group(TriggerRelations::kTargeted, ss, tg);  // [orig: @0x452b30 SG]
    rel.set_group_single(TriggerRelations::kTargeted, sg, ts);  // [orig: @0x452bc0 GS]
    rel.set_single_single(TriggerRelations::kTargeted, ss, ts); // [orig: @0x452c70 SS]
}

// [orig: Entity_SetAITarget @0x45d760] target -> brain[kTargetSlot] + AiSlot[3]; the OLD
// target's Entity::ai_target_refcount decrements (clamp >= 0), the NEW one's increments —
// the +530 anti-pile-on gate the scorer consumes (world-wac-ai-re §16.3).
void AiSystem::ai_set_target(World &world, AiEntity &e, EntityHandle target) {
    AiBrain &b = e.brain;
    const int32_t old_packed = b.f[AiBrain::kTargetSlot];
    if (old_packed != 0) {
        const EntityHandle old{static_cast<uint16_t>(old_packed - 1)};
        if (Entity *oe = world.registry.get(old)) {
            if (oe->ai_target_refcount > 0) --oe->ai_target_refcount; // dec, clamp >= 0
        }
    }
    if (target.valid()) {
        if (Entity *ne = world.registry.get(target)) {
            ++ne->ai_target_refcount;
            ne->ai_target = -1; // scripts read the victim side via relations; id mirror below
        }
        b.f[AiBrain::kTargetSlot] = static_cast<int32_t>(target.packed) + 1;
        e.slot.f[3] = static_cast<int32_t>(target.packed) + 1; // AiSlot[3] mirror
        if (Entity *se = world.registry.get(e.handle)) {
            if (const Entity *ne = world.registry.get(target)) se->ai_target = ne->net_id;
        }
    } else {
        b.f[AiBrain::kTargetSlot] = 0;
        e.slot.f[3] = 0;
        if (Entity *se = world.registry.get(e.handle)) se->ai_target = -1;
    }
}

// LOS between two 16.16 fire-origin points — true = clear. The terrain leg is the
// ported heightmap segment raycast; the sector leg clips the ray against pool-2 /
// pool-1 collision models (the D-AI-7 leg). [orig: Entity_CheckMutualLineOfSight
// @0x539be0 -> Physics_RaycastTerrainAndSectors @0x539910, ray radius 0, 1 = clear.]
// No terrain wired -> clear, the headless-test default; no collision world wired
// (terrain-only unit tests) -> terrain leg alone.
bool AiSystem::line_of_sight_clear(World &world, const int32_t a[3], const int32_t b[3],
                                   EntityHandle from, EntityHandle to) const {
    if (terrain == nullptr || !terrain->valid()) return true;
    // The original tests the segment between the two FIRE ORIGINS — Entity_CheckMutual-
    // LineOfSight @0x539be0 feeds two Entity_ComputeWeaponFireOrigin @0x43b4b0 results
    // into the ray — never the ground-level entity origins (feet-to-feet sampling
    // false-blocks on the very ground both stand on). Until the bone seam lands, lift
    // both endpoints by the same 0.9 u chest stand-in the fire pass uses (D-AI-6/-7).
    constexpr int32_t kChestLift = 0xE666; // 0.9 u [orig: the def+1350 muzzle bone stand-in]
    const int32_t la[3] = {a[0], a[1], a[2] + kChestLift};
    const int32_t lb[3] = {b[0], b[1], b[2] + kChestLift};
    if (collision != nullptr) return collision->raycast_clear(world, la, lb, from, to);
    return !los_terrain_blocked(*terrain, la, lb);
}

// [orig: Entity_AlertNearbyAllies @0x4654b0] same-team, alive, non-building entities
// within `radius` (16.16): own AiSlot+136 = 2 and each ally's brain alert = 2. Container
// rebase: the original scans pool 1; our brains live on the AiEntity list, so the walk
// covers every brained entity (pool 0 organics included) — values written are identical.
void AiSystem::alert_nearby_allies(World &world, AiEntity &e, int32_t radius) {
    (void)world;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 2;
    const int64_t r = radius;
    for (AiEntity &ally : entities_) {
        if (&ally == &e || ally.team != e.team || ally.health <= 0) continue;
        const int64_t ddx = static_cast<int64_t>(ally.pos[0]) - e.pos[0];
        const int64_t ddy = static_cast<int64_t>(ally.pos[1]) - e.pos[1];
        if (ddx * ddx + ddy * ddy > r * r) continue;
        ally.brain.f[AiBrain::kAlert] = 2;
    }
}

// AI fire -> the same authoritative round pair the C2S 0x06 player path enters: ring
// append (the S2C 0x0A tag-2 fan-out every remote client re-simulates the round from)
// + the RoundSim spawn. [orig: WeaponSlot_FireAndSpawnEffects @0x53f440 ->
// Entity_FireWeaponAndSendPacket @0x42bd80 authority leg -> Server_ClientFiredRound
// @0x50baa0 (RoundData_AddRound @0x4fdb40 + RoundData_SpawnRound @0x4ec0d0); §5.60.
// The ammo id rides the ring's adm byte as in the retail fire_cmd; fire sound/muzzle
// effect (g_ammoDefTable +64/+68) are host-presentation, deferred.]
bool AiSystem::fire_ai_round(World &world, AiEntity &e, const int32_t origin[3],
                             int32_t yaw_bam, int32_t pitch_bam, int32_t ammo_index) {
    if (world.ammo.by_index(ammo_index) == nullptr) return false;
    ++fire_shot_seq; // [orig: word_B7C670 round-trips into the ring +28 word]

    RoundEvent ev;
    ev.shooter_handle = e.handle.packed;
    ev.origin_x = origin[0];
    ev.origin_y = origin[1];
    ev.origin_z = origin[2];
    ev.dir_yaw = yaw_bam;
    ev.dir_pitch = pitch_bam;
    ev.shot_seq = fire_shot_seq;
    ev.adm_index = static_cast<uint8_t>(ammo_index & 0xFF);
    world.rounds.add(ev);

    RoundSpawnParams rp;
    rp.owner = e.handle;
    rp.shooter_handle = e.handle.packed;
    rp.origin.x = static_cast<float>(origin[0]) / 65536.0f;
    rp.origin.y = static_cast<float>(origin[1]) / 65536.0f;
    rp.origin.z = static_cast<float>(origin[2]) / 65536.0f;
    rp.dir_yaw_bam = yaw_bam;
    rp.dir_pitch_bam = pitch_bam;
    rp.ammo_index = ammo_index;
    rp.adm_index = static_cast<uint8_t>(ammo_index & 0xFF);
    rp.shot_seq = fire_shot_seq;
    world.round_sim.spawn(world, rp);

    // Firing marks the shooter a priority target until the next perception scan clears
    // it [orig: Flags |= 0x4000 after every fire @0x4bf370; the §16.2 x6 scoring flag].
    if (Entity *se = world.registry.get(e.handle)) se->engine_flags |= 0x4000u;
    return true;
}

// The engagement block [orig: @0x4677b3..0x4678b2]: the sees quad, Entity_SetAITarget,
// reset the combat timer, set the fire-delay with the exact PRNG jitter (the
// has_controller branch guards the jitter by base-delay and uses the inline LCG; the
// other branch always jitters via PRNG_Next16), pending = 17, then the targeted quad.
// The rel_ops trace keeps the recorded shape the tests pin; the APPLY is D-AI-3.
void AiSystem::engage_target(World &world, AiEntity &e, const AiTarget &t) {
    AiBrain &b = e.brain;
    const int32_t self_rm = static_cast<int32_t>(static_cast<int16_t>(e.relmat_id)); // movsx entity+284
    const int32_t tgt_rm = static_cast<int32_t>(static_cast<int16_t>(t.relmat_id));  // movsx target+284

    // [orig: 0x4677b3..0x4677e2] the sees quad before the controller branch.
    rel_ops.push_back({kRelEventSpecial, self_rm, tgt_rm});
    rel_ops.push_back({kRelSharedMem, e.net_id, tgt_rm});
    rel_ops.push_back({kRelProximity, self_rm, t.net_id});
    rel_ops.push_back({kRelEnemy, e.net_id, t.net_id});
    {
        const Entity *se = world.registry.get(e.handle);
        const Entity *te = t.handle.valid() ? world.registry.get(t.handle) : nullptr;
        if (se != nullptr && te != nullptr) apply_engage_relations(world, *se, *te);
    }

    target_set_calls.push_back(t.net_id); // [orig: Entity_SetAITarget(entity, best_target) @0x45d760]
    ai_set_target(world, e, t.handle);
    b.f[AiBrain::kCombatTimer] = 0;        // [orig: ai_comp[40] = 0]
    b.f[AiBrain::kFireDelay] = e.profile.field104; // [orig: ai_comp[41] = *(profile+104)]
    if (t.has_controller) {
        // [orig: branch A — jitter only if base_delay != 0; inline LCG on dword_31BFBB8]
        if (e.profile.field104 != 0)
            b.f[AiBrain::kFireDelay] += static_cast<int32_t>(static_cast<uint16_t>(prng_step_a()) % 62);
    } else {
        // [orig: branch B — always jitter; PRNG_Next16 on dword_31BFBB0]
        b.f[AiBrain::kFireDelay] += static_cast<int32_t>(static_cast<uint16_t>(prng_step16()) % 62);
    }
    b.set_pend(kAiGroundCombat); // [orig: ai_comp[5] = 17]

    // [orig: 0x467883..0x4678b2] the targeted quad after pending = 17.
    rel_ops.push_back({kRelAllied, self_rm, tgt_rm});
    rel_ops.push_back({kRel452B30, e.net_id, tgt_rm});
    rel_ops.push_back({kRelDamaged, self_rm, t.net_id});
    rel_ops.push_back({kRelSpotted, e.net_id, t.net_id});
}

// [orig: AI_HandleCommand @0x465770] command dispatcher (cases 6..0x16). Deferred to the
// AI-command phase. The combat event types (1/3/4) are not commands, so the original returns 0
// for them and the event switch proceeds; this faithfully returns false.
bool AiSystem::ai_handle_command(AiEntity &, const AiEventEntry &ev) {
    int32_t t = ev.type();
    if (t >= 6 && t <= 0x16) ++unported_calls; // a real command would be handled by the AI-command phase
    return false;
}

} // namespace opennova::world
