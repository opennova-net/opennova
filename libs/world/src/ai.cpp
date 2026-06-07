// Entity AI subsystem — byte-exact foundation port. See world/ai.h + the RE map
// notes/world/ai_movement.md. Each handler cites its Jointops.exe origin; heavy
// per-state behaviors route to not_yet_ported until their phase lands.
#include "world/ai.h"

namespace opennova::world {

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
    /* 16 GROUND_FOLLOWWP  */ {h_set_state_idle, U, _, U},
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

} // namespace opennova::world
