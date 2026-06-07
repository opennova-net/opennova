// Entity AI subsystem — byte-exact foundation port. See world/ai.h + the RE map
// notes/world/ai_movement.md. Each handler cites its Jointops.exe origin; heavy
// per-state behaviors route to not_yet_ported until their phase lands.
#include "world/ai.h"

#include <cmath>

namespace opennova::world {

namespace {

// radians -> 32-bit binary angle. [orig: dbl_7C19D8 = 0x41C45F306DC9C883.]
constexpr double kBamPerRadian = 683565275.5764316; // 2^32 / (2*pi)

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

} // namespace

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
        double sp = std::sqrt(static_cast<double>(e.vel_x) * e.vel_x +
                              static_cast<double>(e.vel_z) * e.vel_z);
        if (sp > kDeathSpeedClamp) sp = kDeathSpeedClamp; // flt_7C19E0 min-clamp
        int32_t isp = static_cast<int32_t>(sp);
        AiEventEntry ev{};
        ev.f[0] = (isp >= 1057) ? 3 : 4;          // crash/ragdoll vs still death
        int ai_index = static_cast<int>(&e - ctx.sys->at(0));
        ev.f[1] = (ai_index << 16);                // channel 0 | entity index
        ev.set_timer(0.0f);
        ctx.sys->events.queue(ev);
        return;
    }

    // Target acquisition is skipped when profile+100 & 2 (use-fallback).
    bool has_target = ((e.profile.flags100 & 2) != 0) ? false : ctx.sys->acquire_target(e);

    if ((e.profile.flags96 & 0x10) != 0) { // can-fire
        if (b.f[AiBrain::kFireTimer] <= 0)
            b.f[AiBrain::kFireTimer] = 0;
        else
            ++ctx.sys->unported_calls; // [orig: Entity_ComputeWeaponFirePositions @0x455ef0] -> P2
    }
    int32_t ft = b.f[AiBrain::kFireTimer];
    if (ft <= 0)
        b.f[AiBrain::kFireTimer] = 0;
    else
        b.f[AiBrain::kFireTimer] = ft - b.f[AiBrain::kStep];

    if (has_target) {
        // [orig: set relation matrices + Entity_SetAITarget + pending=17] -> P2 combat.
        ++ctx.sys->unported_calls;
        b.set_pend(kAiGroundCombat); // 17
    } else {
        ctx.sys->update_waypoint_movement(e); // [orig: AI_UpdateWaypointMovement @0x457bd0]
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
    /* 16 GROUND_FOLLOWWP  */ {h_set_state_idle, h_ground_followwp_tick, _, U},
    /* 17 GROUND_COMBAT    */ {U, U, h_clear_bone_flag, U},
    /* 18 GROUND_EVADE     */ {U, U, _, U},
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
int AiSystem::attach(EntityHandle h) {
    AiEntity e;
    e.handle = h;
    e.brain.f[AiBrain::kOwner] = 1; // nonzero = live slot
    entities_.push_back(e);
    return static_cast<int>(entities_.size()) - 1;
}

AiEntity *AiSystem::at(int ai_index) {
    if (ai_index < 0 || ai_index >= static_cast<int>(entities_.size())) return nullptr;
    return &entities_[ai_index];
}

AiEntity *AiSystem::for_handle(EntityHandle h) {
    for (AiEntity &e : entities_)
        if (e.handle == h) return &e;
    return nullptr;
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
        finish();
        return;
    }
    if (event == 1) { // spawn
        AiEventEntry ev{};
        ev.f[0] = 1;
        ev.f[1] = 9 | (ai_index << 16); // channel 9 | entity index
        ev.set_timer(0.0f);
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
    is_authority = ctx.is_authority;
    scheduler.budget = 0; // per-frame budget reset (the staggering accumulator)
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        if (begin_update(e))
            process_infantry_state_machine(e, world, 0);
    }
    events.process_timed(*this, world);
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

// [orig: AI_FindBestTargetB @0x466f60] -> P2 (combat). Stub: never finds a target so
// GROUND_FOLLOWWP always walks its path; counts calls for coverage visibility.
bool AiSystem::acquire_target(AiEntity &) {
    ++find_target_calls;
    return false;
}

} // namespace opennova::world
