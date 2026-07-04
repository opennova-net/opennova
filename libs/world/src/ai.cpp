// Entity AI subsystem — byte-exact foundation port. See world/ai.h + the RE map
// notes/world/ai_movement.md. Each handler cites its Jointops.exe origin; heavy
// per-state behaviors route to not_yet_ported until their phase lands.
#include "world/ai.h"

#include "terrain/height_field.h"
#include "world/body_anim.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

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
        const int variant = std::clamp<int>(target.emplaced_pose_variant, 0, 8);
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
constexpr int64_t kBamPerDegree = 11930464;

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
    bool has_target = ((e.profile.flags100 & 2) != 0) ? false : ctx.sys->acquire_target(e, tgt);

    if ((e.profile.flags96 & 0x10) != 0) { // can-fire
        if (b.f[AiBrain::kFireTimer] <= 0)
            b.f[AiBrain::kFireTimer] = 0;
        else
            ++ctx.sys->unported_calls; // [orig: Entity_ComputeWeaponFirePositions @0x455ef0] -> weapon phase
    }
    int32_t ft = b.f[AiBrain::kFireTimer];
    if (ft <= 0)
        b.f[AiBrain::kFireTimer] = 0;
    else
        b.f[AiBrain::kFireTimer] = ft - b.f[AiBrain::kStep];

    if (has_target)
        ctx.sys->engage_target(e, tgt);       // [orig: relation matrices + SetAITarget + pending=17]
    else
        ctx.sys->update_waypoint_movement(e); // [orig: AI_UpdateWaypointMovement @0x457bd0]
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
            ++ctx.sys->unported_calls; // [orig: Entity_ComputeWeaponFirePositions @0x455ef0] -> weapon phase
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

// The 24-state dispatch table, mirroring off_815238/3C/40/44 @0x815238.
// Columns: {enter, tick, exit, event}. U = not-yet-ported (visible stub), _ = noop.
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
    /* 17 GROUND_COMBAT    */ {U, U, h_clear_bone_flag, h_combat_event},
    /* 18 GROUND_EVADE     */ {U, h_patrol_tick, _, h_combat_event},
    /* 19 GROUND_FORMATION */ {h_full_reset_to_idle, U, _, U},
    /* 20 GROUND_RETURNTOBASE */ {_, _, _, _},
    /* 21 (transition)     */ {U, U, _, U},
    /* 22 GROUND_PRETTY    */ {h_full_reset_to_patrol, U, _, U},
    /* 23 GROUND_DEAD      */ {U, U, _, U},
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
                        e->brain.f[AiBrain::kCurState] = pend;
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
    // The loop runs on a JOINER (client, !is_authority) too: tick_infantry's §5.38
    // entity==g_local_player branch (line below, no authority guard) motor-sims the
    // joiner's own player from input, while NPC think/select stays authority-gated, so
    // local-promote copies of remote entities just hold (idle). The joiner renders every
    // REMOTE entity's pose from the host's S2C 0x0A (present reads ClientState, not these
    // local copies), so their idle ticking is harmless. [orig: the client also runs the
    // per-entity AI tick; Entity_UpdateInfantryAI @0x4b9910 simulate-when entity==local.]
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        // A mounted occupant (manned gun/seat) follows its seat and never path-follows; skip the
        // state machine + locomotion for it.
        if (pose_if_mounted(e, world)) {
            advance_part_anim(e);
            continue;
        }
        if (e.inf.active) {
            // org1-class soldier: the infantry motor replaces the vehicle SM + kinematic
            // locomotion for this entity. [orig: g_EntityClassPhysicsTable row "org1" ->
            // Entity_UpdateInfantryAI @0x4b9910]
            tick_infantry(e, world, ctx.logic_tick);
            continue;
        }
        if (begin_update(e)) {
            process_infantry_state_machine(e, world, 0);
            if (locomotion_enabled) {
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
            tick_vehicle_motor(world, *veh, *traits);
        }
    }
    events.process_timed(*this, world);
}

bool AiSystem::pose_if_mounted(AiEntity &e, World &world) {
    Entity *occ = world.registry.get(e.handle);
    if (occ == nullptr || !occ->mounted) return false;
    Entity *veh = world.registry.get(occ->mount_target);
    if (veh == nullptr) {              // vehicle gone -> auto-dismount, resume normal AI
        world.commands.dismount(occ->net_id);
        return false;
    }
    if (occ->mount_seat < 0 || occ->mount_seat >= static_cast<int>(veh->seats.size())) return false;
    const Seat &seat = veh->seats[occ->mount_seat];
    pose_mounted_occupant(*occ, *veh, seat);
    // Mirror the world Entity transform into the AiEntity the present snapshot reads (organics are
    // presented from pos[]/heading, not Entity.position; promote seeds them the same way).
    e.pos[0] = to_fixed(occ->position.x);
    e.pos[1] = to_fixed(occ->position.y);
    e.pos[2] = to_fixed(occ->position.z);
    // Engine-frame heading (90 - mission yaw), matching the spawn seed + the mover; the present
    // converts back to mission yaw for the basis. [orig: entity heading = (90 - yaw) @0x40e9f0.]
    e.heading = static_cast<int32_t>(static_cast<int64_t>(90 - occ->yaw) * kBamPerDegree);
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
        default:
            // Tracked-TODO: alert(5/6/0x16), accuracy(8), AISETSTATE(0x1C), speed(0x1D/0x1E),
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
    unported_calls = 0;
    find_target_calls = 0;
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

// [orig: AI_UpdateWaypointMovement @0x457bd0] advance along the path; write the working
// target transform + out-speed. The return value is unused by the caller; we mirror the
// original's "freeze on path end / no waypoint" branches faithfully.
int AiSystem::update_waypoint_movement(AiEntity &e) {
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
        // [orig: SetBitB key = movsx word [entity+284] @0x457c6d -> sign-extend the u16]
        relmat_calls.push_back({1, static_cast<int32_t>(static_cast<int16_t>(e.relmat_id)), ch, kf});
        relmat_calls.push_back({0, e.net_id, ch, kf}); // SetBitA(entity+124) is a full dword load
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

// [orig: AI_FindBestTargetB @0x466f60] scan the injected candidate list. Per-candidate perception
// gates (team, flags, health, self, visibility) then FOV/range/stealth scoring + mutual LOS; the
// priority target (profile+148) bypasses scoring and returns immediately on LOS. Tracked deviation:
// the original iterates 4 weapon slots -> g_pool_list pools with a vehicle/building sub-filter;
// that pool selection + the relation-matrix team filter + Entity_CheckMutualLineOfSight are
// deferred (candidates are injected pre-filtered; LOS is the per-candidate los_blocked flag).
bool AiSystem::acquire_target(AiEntity &e, AiTarget &out) {
    ++find_target_calls;
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
                out = AiTarget{c.relmat_id, c.net_id, c.has_controller};
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
        out = AiTarget{best->relmat_id, best->net_id, best->has_controller};
        return true;
    }
    return false;
}

// [orig: the engagement block @0x4677b3..0x4678b2] 4 relation ops, Entity_SetAITarget, reset the
// combat timer, set the fire-delay with the exact PRNG jitter, pending = 17, then 4 more ops.
// The has_controller branch guards the jitter by base-delay and uses the inline LCG (prng_a); the
// other branch always jitters and uses PRNG_Next16 (prng16). Both record the same 8 ops + target.
void AiSystem::engage_target(AiEntity &e, const AiTarget &t) {
    AiBrain &b = e.brain;
    const int32_t self_rm = static_cast<int32_t>(static_cast<int16_t>(e.relmat_id)); // movsx entity+284
    const int32_t tgt_rm = static_cast<int32_t>(static_cast<int16_t>(t.relmat_id));  // movsx target+284

    // [orig: 0x4677b3..0x4677e2] the 4 ops before the controller branch.
    rel_ops.push_back({kRelEventSpecial, self_rm, tgt_rm});
    rel_ops.push_back({kRelSharedMem, e.net_id, tgt_rm});
    rel_ops.push_back({kRelProximity, self_rm, t.net_id});
    rel_ops.push_back({kRelEnemy, e.net_id, t.net_id});

    target_set_calls.push_back(t.net_id); // [orig: Entity_SetAITarget(entity, best_target) @0x45d760]
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

    // [orig: 0x467883..0x4678b2] the 4 ops after pending = 17.
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
