// Entity AI subsystem — byte-exact port of the Jointops.exe infantry/vehicle AI
// brain (the third ticking system over the shared world, after WAC + BMS events).
//
// This is the FOUNDATION pass: the data structures + control skeleton reproduced
// faithfully from the binary, with the heavy per-state behavior handlers ported
// incrementally (unported ones route to a visible `not_yet_ported` stub so
// coverage is explicit). See notes/world/ai_movement.md for the full RE map.
//
// IDA anchors (Jointops.exe, imagebase 0x400000):
//   EntityAI_ProcessInfantryStateMachine @0x4581b0   (the dispatcher)
//   AI_BeginUpdate                        @0x457b40   (per-frame budget gate, cap 496)
//   AIEvent_QueueEntry                    @0x455da0   (1024 x 5-dword ring)
//   AIEvent_ProcessTimedEntries           @0x455df0   (timer -= 0.016/frame)
//   state-handler table                   @0x815238   (24 records x {enter,tick,exit,event})
//   Entity_LookupAIStateName              @0x455cc0   (the authoritative state enum)
//
// Tracked deviation (per feedback_ida_algorithmic_fidelity): the original keeps
// brains in the absolute global array unk_AED380 (812-byte stride), the profile at
// brain[1] and the scheduler at brain[2] as absolute pointers, and the handler
// dispatch in absolute function-pointer tables. We rebase those to pool-relative
// containers (a vector of AiEntity, a direct AiProfile/AiScheduler member, a static
// StateRow table). Struct bodies are modeled as int32 f[N] + named indices so the
// ported handlers index fields exactly as the decompiler does (b.f[4], b.f[5], ...).
#ifndef OPENNOVA_WORLD_AI_H
#define OPENNOVA_WORLD_AI_H

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "world/entity.h"
#include "world/world.h"

namespace opennova::world {

// ----------------------------------------------------------------------------
// AI state ids. [orig: Entity_LookupAIStateName @0x455cc0 string table.] States
// 0..5 are default/uninitialised (handlers are nullsubs); HELO_* are air units,
// GROUND_* are infantry + ground vehicles. ids 13 and 21 are transitional gaps.
// ----------------------------------------------------------------------------
enum AiState : int32_t {
    kAiDefault0 = 0, // 0..5 spawn/uninitialised, nullsub handlers
    kAiHeloLand = 6,
    kAiHeloFollowWp = 7,
    kAiHeloCombat = 8,
    kAiHeloHunt = 9,
    kAiHeloEvade = 10,
    kAiHeloFormation = 11,
    kAiHeloReturnToBase = 12,
    kAiHeloPretty = 14,
    kAiHeloDead = 15,
    kAiGroundFollowWp = 16,
    kAiGroundCombat = 17,
    kAiGroundEvade = 18,
    kAiGroundFormation = 19,
    kAiGroundReturnToBase = 20,
    kAiGroundPretty = 22,
    kAiGroundDead = 23,
    kAiStateCount = 24,
};

// Human-readable state name (returns "?" for the gaps/unknowns), faithful to
// Entity_LookupAIStateName @0x455cc0.
const char *ai_state_name(int32_t state);

// ----------------------------------------------------------------------------
// AiBrain — entity+100 / unk_AED380, 812 bytes (203 dwords). Modeled as a raw
// dword array so handlers index it exactly like the decomp (`b.f[4]`).
// ----------------------------------------------------------------------------
struct AiBrain {
    int32_t f[203] = {};

    // Named dword indices (confirmed from the decomp; see notes §1).
    enum Idx : int {
        kOwner = 0,        // back-ref to entity (nonzero = slot live)
        kCurState = 4,     // current AI state  [byte +16]
        kPendState = 5,    // pending state (applied when != kCurState) [byte +20]
        kFallback = 6,     // fallback state [byte +24]
        kStep = 7,         // move step / per-frame budget cost (idle 16 / patrol 64) [byte +28]
        kFireTimer = 9,    // fire countdown (decrements by kStep) [byte +36]
        kTick = 10,        // ++ each SM update [byte +40]
        kAlert = 46,       // alert level [byte +184]
        kPrevAlert = 47,   // previous alert level (edge) [byte +188]
        kNoTargetIdle = 48,// set 1 when no target + profile not combat [byte +192]
        kSpeedA = 49,      // move speed A [byte +196]
        kSpeedB = 50,      // move speed B (state 16) [byte +200]
        kTargetRef = 127,  // primary target ref [byte +508]
        kOutSpeed = 128,   // mover output speed [byte +512]
        kGuard = 144,      // guards kNoTargetIdle [byte +576]
        kBoneFlag = 196,   // bone-tracking flag (byte +784)
    };

    int32_t cur_state() const { return f[kCurState]; }
    int32_t pend_state() const { return f[kPendState]; }
    void set_pend(int32_t s) { f[kPendState] = s; }
};
static_assert(sizeof(AiBrain) == 812, "AiBrain must match unk_AED380 812-byte stride");

// AiSlot — entity+104 / unk_A34B90, 172 bytes (43 dwords). [orig: Entity_AllocateAISlot
// @0x40d2c0.] The reset helpers clear a movement flag byte at slot+136.
struct AiSlot {
    int32_t f[43] = {};
    uint8_t *bytes() { return reinterpret_cast<uint8_t *>(f); }
    enum { kMoveFlagByte = 136 };
};
static_assert(sizeof(AiSlot) == 172, "AiSlot must match unk_A34B90 172-byte stride");

// AiProfile — brain[1], the read-only AI definition. Modeled minimally: only the
// flag bytes the state machine reads. [orig: AIProfile_LoadOrFind @0x45fd80.]
struct AiProfile {
    uint8_t flags96 = 0;   // +96: bit1 (&2) combat-capable, bit4 (&0x10) can-fire
    uint8_t flags100 = 0;  // +100: bit1 (&2) use-fallback-state
    bool has_src148 = false; // +148 target-source gate
    bool has_src180 = false; // +180 target-source gate
    int32_t field216 = 0;  // +216: added into brain working field [131]
    int32_t field220 = 0;  // +220: copied into brain working field [138]
};

// AiScheduler — brain[2], the shared per-frame budget accumulator (the +16 field).
struct AiScheduler {
    int32_t budget = 0; // [orig: scheduler+16]
    static constexpr int32_t kBudgetCap = 496; // [orig: AI_BeginUpdate @0x457b40]
};

// One AI-controlled entity's complete brain state. (The original splits brain /
// slot / profile across three arrays; we colocate, keyed by the world handle.)
struct AiEntity {
    EntityHandle handle;
    AiBrain brain;
    AiSlot slot;
    AiProfile profile;
    bool has_physics = true;   // entity+368 present
    uint32_t physics_flags = 0;// entity+368 +36 (bit 0x100 = airborne)
    int32_t heading = 0;       // entity+16 (32-bit binary angle); copied to brain[132]
};

class AiSystem; // fwd

// AIEvent ring entry: 5 dwords. [orig: dword_AE0778 ring, AIEvent_QueueEntry.]
//   f[0]=type, f[1]=(channel:lo16 | entity_index:hi16), f[2]=timer(float bits),
//   f[3]=extra, f[4]=unused(caller fills 4, QueueEntry copies 5).
struct AiEventEntry {
    int32_t f[5] = {};
    int32_t type() const { return f[0]; }
    int32_t channel() const { return f[1] & 0xFFFF; }
    int32_t entity_index() const { return (f[1] >> 16) & 0xFFFF; }
    float timer() const { float v; std::memcpy(&v, &f[2], 4); return v; }
    void set_timer(float v) { std::memcpy(&f[2], &v, 4); }
};

// Fixed circular AIEvent queue. [orig: dword_AE0778 (base) + dword_AE5778 (count),
// max 1024; AIEvent_QueueEntry @0x455da0 / AIEvent_ProcessTimedEntries @0x455df0.]
class AiEventQueue {
public:
    static constexpr int kMax = 1024;
    static constexpr float kFrameDt = 0.016000001f; // [orig: timer -= 0.016 per frame]

    // [orig: AIEvent_QueueEntry] copies 5 dwords if count<1024.
    void queue(const AiEventEntry &e);

    // [orig: AIEvent_ProcessTimedEntries] decrement timers; on expiry dispatch the
    // state's event handler + apply any pending transition; compact swap-with-last.
    void process_timed(AiSystem &sys, World &world);

    int count() const { return count_; }
    const AiEventEntry &at(int i) const { return buf_[i]; }
    void clear() { count_ = 0; }

private:
    std::array<AiEventEntry, kMax> buf_{};
    int count_ = 0;
};

// Per-handler call context. The original passes the raw entity pointer; we pass the
// resolved AiEntity + world + (for event handlers) the firing AIEvent entry.
struct AiThinkCtx {
    AiSystem *sys = nullptr;
    AiEntity *self = nullptr;
    World *world = nullptr;
    const AiEventEntry *event = nullptr; // non-null only in event-handler dispatch
};

using AiHandler = void (*)(AiThinkCtx &);

// One state's 16-byte record: {enter, tick, exit, event}. [orig: off_815238/3C/40/44.]
struct StateRow {
    AiHandler enter;
    AiHandler tick;
    AiHandler exit;
    AiHandler event;
};

// The AI subsystem: a world::ISystem ticking all AI brains on the shared world.
class AiSystem : public ISystem {
public:
    const char *name() const override { return "ai"; }
    void tick(World &world, const TickContext &ctx) override;

    // Attach a brain to a world entity; returns its AI index (faithful to the
    // unk_AED380 array index used by AIEvent entity_index).
    int attach(EntityHandle h);
    AiEntity *at(int ai_index);
    AiEntity *for_handle(EntityHandle h);
    int count() const { return static_cast<int>(entities_.size()); }

    AiScheduler scheduler;
    AiEventQueue events;
    bool is_authority = true; // [orig: g_napi_np_ctx.is_authority]
    bool is_in_session = false;
    int unported_calls = 0;   // coverage counter for not_yet_ported handlers

    // [orig: AI_BeginUpdate @0x457b40] budget gate. Returns false (skip this frame)
    // when the shared scheduler budget exceeds the cap; forces idle/fallback.
    bool begin_update(AiEntity &e);

    // [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event: 0=update,1=spawn,4=death.
    void process_infantry_state_machine(AiEntity &e, World &world, int event);

    // The shared pending-state transition (exit current, enter pending, commit).
    void apply_transition(AiEntity &e, World &world);

    const StateRow &row(int32_t state) const;

private:
    std::vector<AiEntity> entities_; // pool-relative; index == AIEvent entity_index
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_AI_H
