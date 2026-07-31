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

#include "world/collision.h"
#include "world/entity.h"
#include "world/infantry.h"
#include "world/world.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

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

    // Named dword indices (confirmed from the decomp; see notes §1/§4).
    enum Idx : int {
        kOwner = 0,        // back-ref to entity (nonzero = slot live)
        kCurState = 4,     // current AI state  [byte +16]
        kPendState = 5,    // pending state (applied when != kCurState) [byte +20]
        kFallback = 6,     // fallback state [byte +24]
        kStep = 7,         // move step / per-frame budget cost (idle 16 / patrol 64) [byte +28]
        kFireTimer = 9,    // fire countdown (decrements by kStep) [byte +36]
        kTick = 10,        // ++ each SM update [byte +40]
        // ---- waypoint sub-struct (passed to AIWaypoint_UpdateTarget as brain+52) ----
        kWpType = 13,      // waypoint type: 1 nav-node, 3 literal coord [byte +52]
        kWpChannel = 14,   // nav/anim channel id (= navMeshId)        [byte +56]
        kWpNode = 15,      // current node index within the channel path [byte +60]
        kWpResolved = 16,  // resolved pool-3 node (orig: ptr; we store index) [byte +64]
        kWpCoordX = 17,    // type-3 literal target X [byte +68]
        kWpCoordY = 18,    // type-3 literal target Y [byte +72]
        kWpCoordZ = 19,    // type-3 literal target Z [byte +76]
        kWpCoordSrc = 20,  // type-3 -> copied into kWpNodeVal [byte +80]
        kWpBearing = 21,   // BAM bearing to target (atan2 * 2^32/2pi) [byte +84]
        kWpDistance = 22,  // approx Manhattan distance to target [byte +88]
        kWpNodeVal = 23,   // node payload f[0] (type1) / kWpCoordSrc (type3) [byte +92]
        kWpExtra = 24,     // node payload f[4] (type1) / 0 (type3) [byte +96]
        kAnimFlag = 32,    // cleared on node advance [byte +128]
        kStoredKeyTime = 35, // stored node-val on advance [byte +140]
        kTargetSlot = 38,  // current target (orig: entity ptr; container rebase stores
                           // EntityHandle.packed + 1 so 0 keeps the orig null meaning) [byte +152]
        kDamageInfo = 39,  // damage source/info copied from a damage event's extra [byte +156]
        kCombatTimer = 40, // no-target / combat re-acquire timer (>620 re-acquire) [byte +160]
        kFireDelay = 41,   // fire-delay countdown set on engagement [byte +164]
        kRetargetTimer = 42, // += 16 per processed state-17 tick; >248 rescans [byte +168]
        kAccuracy = 43,    // scatter accuracy 0..5 (modulus 6 - acc) [byte +172]
        kAlert = 46,       // alert level [byte +184]
        kPrevAlert = 47,   // previous alert level (edge) [byte +188]
        kNoTargetIdle = 48,// set 1 when no target + profile not combat [byte +192]
        kSpeedA = 49,      // move speed A [byte +196]
        kSpeedB = 50,      // move speed B (state 16 GROUND_FOLLOWWP) [byte +200]
        // ---- the state-17 tick working set (world-wac-ai-re §17.6) ----
        kTickAccum = 8,    // accumulates kStep; >=16 -> one processed tick [byte +32]
        kCooldownPair = 52,// PACKED u16 pair (weapon cooldowns A/B): += 0x10001*step per
                           // tick — one add advances both words, low-word carry included
                           // [orig: brain[52] += 65537*deltaTime @0x472e00; bytes +208/+210]
        kAmmoA = 53,       // primary ammo count [byte +212]
        kAmmoB = 54,       // secondary ammo count [byte +216]
        kLastWeapon = 106, // 1/2 = which weapon the continuation branches re-fire [byte +424]
        kSweepPhase = 180, // sweep-fire lateral phase, -196608..196608 step 10918 [byte +720]
        kBurstWindow = 181,// burst window: armed to 1 on fire, += step while <= 186 [byte +724]
        // ---- part-anim channels (vehicle/emplacement parts; PLAYPARTANIM, 2 channels) ----
        // [orig: Entity_ApplyCommand @0x43ab60 case 0x22 writes comp+436 (direction) /
        // comp+444 (rate). Def defaults: Entity_CopyVehicleDefToAIComp @0x45ddf9 copies
        // def+764..784 -> comp+436..456, so the phase pair (comp+452/+456) is def-seeded.]
        kPartAnimDir0 = 109,   // comp+436 channel-1 sweep direction (-1/0/+1)
        kPartAnimDir1 = 110,   // comp+440 channel-2 sweep direction
        kPartAnimRate0 = 111,  // comp+444 channel-1 rate (16.16 phase units / tick)
        kPartAnimRate1 = 112,  // comp+448 channel-2 rate
        kPartAnimPhase0 = 113, // comp+452 signed phase dword (ordinary range 0..0x10000)
        kPartAnimPhase1 = 114, // comp+456 channel-2 phase
        kTargetRef = 127,  // primary target ref [byte +508]
        kOutSpeed = 128,   // mover output speed [byte +512]
        kWorkPosX = 129,   // working target transform X [byte +516]
        kWorkPosY = 130,   // working target transform Y [byte +520]
        kWorkPosZ = 131,   // working target transform Z [byte +524]
        kWorkHeading = 132,// working target heading [byte +528]
        kWorkPitch = 133,  // working target pitch [byte +532]
        kWorkRoll = 134,   // working target roll [byte +536]
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
    // +100 mode dword (modeled as the low byte): bit0 (&1) move-while-fighting, bit1 (&2)
    // use-fallback-state, bit2 (&4) hold-heading, bit3 (&8) ignore-refcount-saturation,
    // bit5 (&0x20) sweep fire, bit6 (&0x40) burst fire, bit7 (&0x80) stationary fire.
    // [orig: AIEntity_ProcessWeaponFire @0x472e00 mode dispatch; world-wac-ai-re §17.6]
    uint8_t flags100 = 0;
    bool has_src148 = false; // +148 target-source gate
    bool has_src180 = false; // +180 target-source gate
    int32_t field216 = 0;  // +216: added into brain working field [131]
    int32_t field220 = 0;  // +220: copied into brain working field [138]
    // ---- combat / targeting (P2) ----
    int32_t field104 = 0;       // +104: base fire delay (engagement); 0 -> no jitter (branch A)
    uint8_t fov_primary = 0;    // +75:  primary FOV arc byte (OR'd with 1 before use)
    uint8_t fov_secondary = 0;  // +67:  secondary FOV / turret arc byte (OR'd with 1)
    int16_t range_primary = 0;  // +78:  primary-FOV max engage range (world units, signed i16)
    int16_t range_secondary = 0;// +70:  secondary-FOV max engage range (world units, signed i16)
    // ---- the SM state-17 tick (vehicles/emplacements; world-wac-ai-re §17.6) ----
    int32_t fire_interval_a = 0;  // +124: primary fire interval (cooldown word +208 gate)
    int32_t fire_interval_b = 0;  // +156: secondary fire interval (word +210 gate)
    uint8_t weapon_a = 0;         // +148 byte: primary ammo-def id
    uint8_t weapon_b = 0;         // +180 byte: secondary ammo-def id
    int32_t accuracy = 0;         // brain[43] seed: scatter modulus = 6 - accuracy (0..5)
                                  // [orig: the ai.def copy block seeds brain[43]; source
                                  // field unwitnessed — part of Entity_CopyVehicleDefToAIComp]
    int32_t approach_cap = 0;     // +76: chase range cap (16.16)
    int32_t min_range = 0;        // +188: minimum engage range (16.16)
    int32_t match_speed_range = 0;// +184: follow-at-target-speed range (16.16)
    // ---- the infantry combat pass (org1 riflemen; world-wac-ai-re §17.4, D-AI-5) ----
    // One ammo id + clip stands in for the four anim-fire weapon bytes (+0x358..0x35B —
    // JO riflemen author all four = the rifle round) until the block-copy writer is
    // witnessed. -1 = unarmed (the pass never fires).
    int32_t ammo_primary = -1;    // world.ammo index [orig: items.def ammo_closeattack family]
    int32_t clip_size = 0;        // items.def clipsize (magazine reseed)
    // Index into world.sound_profiles (the def's sound_profile, resolved at the
    // host's item-traits sweep; -1 = unresolved -> the table's "default"
    // fallback at emit). The female-variant select (def+2152 via the character
    // entity's female byte [orig: @ 0x52831c]) is unmodeled — primary always.
    int16_t sound_profile = -1;
};

// AiScheduler — brain[2], the shared per-frame budget accumulator (the +16 field).
struct AiScheduler {
    int32_t budget = 0; // [orig: scheduler+16]
    static constexpr int32_t kBudgetCap = 496; // [orig: AI_BeginUpdate @0x457b40]
};

// ----------------------------------------------------------------------------
// Nav / anim node table. [orig: globals at 0xA71DD0 (Buffer) / 0xA71DD4
// (dword_A71DD4) / 0xA71DD8 (entryIndex) — three aliases into ONE array of
// 34-dword (136-byte) records.] Each record is a path/channel; entries[k] is a
// pool-3 (waypoint-marker) entry index resolved via Pool_GetEntryUnchecked(3,..).
// Tracked deviation: rebased from the three overlapping absolute globals to plain
// containers; values resolved are byte-identical.
// ----------------------------------------------------------------------------

// Pool-3 waypoint/nav marker entry. The mover/solver read f[1]/f[2]/f[3] as world
// X/Y/Z and f[0]/f[4] as node payload. (Real pool-3 stride may exceed 20 bytes; we
// only model the 5 dwords the AI touches.)
struct NavEntry {
    // {arrival_radius, x, y, z, facing}. A nav node in the original IS the pool-3 marker
    // entity; these mirror the fields the movers read: f[0] = arrival radius [orig: marker
    // dword[0], seeded from BMS wp_distance<<16, default 0x8000 @0x40e9f0], f[1..3] =
    // position, f[4] = facing [orig: marker+16 = spawn yaw]. The vehicle mover reads f[0]
    // as its advance threshold ("animTime") -- same field, same meaning.
    int32_t f[5] = {};
    // Marker hold time in ticks [orig: marker+328 = 62 * BMS u16@+62]; the infantry think
    // converts to think-ticks as (wait+8)>>4 on arrival. Extra field of our container
    // rebase (the original reads it straight off the marker entity).
    int32_t wait_ticks = 0;
};

// One channel record (34 dwords). loopflag bit0 set = terminate at path end (else
// wrap to node 0). count = node count. entries = pool-3 indices, one per node.
struct NavChannel {
    int32_t loopflag = 0;     // [orig: Buffer[34*ch]]        bit0 = one-shot
    int32_t count = 0;        // [orig: dword_A71DD4[34*ch]]  node count
    int32_t entries[32] = {}; // [orig: entryIndex[34*ch + k]] pool-3 node indices
};
static_assert(sizeof(NavChannel) == 136, "NavChannel must match the 34-dword stride");

// The channel table + pool-3 node store. Populated by mission->world promotion (P6).
class NavNodeTable {
public:
    std::vector<NavChannel> channels;
    std::vector<NavEntry> nodes; // pool 3

    const NavChannel *channel(int ch) const {
        if (ch < 0 || ch >= static_cast<int>(channels.size())) return nullptr;
        return &channels[ch];
    }
    const NavEntry *entry(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(nodes.size())) return nullptr;
        return &nodes[idx];
    }
};

// One AI-controlled entity's complete brain state. (The original splits brain /
// slot / profile across three arrays; we colocate, keyed by the world handle.)
// The geometry fields mirror the original entity record offsets the AI reads/writes
// (faithful units: 32-bit fixed-point position, 32-bit binary-angle heading).
struct AiEntity {
    EntityHandle handle;
    AiBrain brain;
    AiSlot slot;
    AiProfile profile;
    bool has_physics = true;   // entity+368 present
    uint32_t physics_flags = 0;// entity+368 +36 (bit 0x100 = airborne)
    int32_t pos[3] = {};       // entity+4/+8/+12 (position X/Y/Z, 32-bit fixed)
    int32_t heading = 0;       // entity+16 (32-bit binary angle); copied to brain[132]
    int32_t pitch = 0;         // entity+20
    int32_t roll = 0;          // entity+24
    // The slope-conform body pitch (entity+0x90): the slope pass chases it toward the
    // fore-aft ground slope for conforming (prone-family / flagged / dead) bodies and
    // decays it to level otherwise; the §14 overlay body-pitch term reads it.
    // [orig: org1 @0x4ba320 / org2 @0x4b6fc1-0x4b6fda]
    int32_t body_pitch = 0;
    // The entity DEFINITION's attrib dword (def = entity+0x20, attrib at def+0x54).
    // Bit 0x200 forces slope-conform in the slope pass. JO infantry defs leave it
    // clear; host wiring is deferred until a consumer needs the other bits.
    // [orig: selector @0x4ba10f / @0x4b6d99]
    uint32_t def_attrib = 0;

    // --- embedder-fed muzzle seam (D-AI-6) ---
    // The posed muzzle world position (16.16 fixed, mission frame), pushed once per
    // frame by the embedder's present layer: the model's gun-flash userpoint (US01
    // "GFlash01", SASBODY1 "MFlash01"/"bullet") transformed by the ANIMATED skeleton.
    // [orig: the anim-event fire transforms the fire-bone userpoint by the live pose —
    // Entity_GetAttachmentWorldPosition @0x4b2670 (userpoint local pos x posed bone
    // matrix, model userpoint table @model+0xC0) from the fire block @0x4bf326..0x4bf425;
    // libs/world carries no skeletal pose, so the embedder feeds the result back.]
    // muzzle_tick stamps the world logic tick of the push; the fire pass consumes the
    // value only while FRESH and otherwise falls back to the chest-lift stand-in.
    int32_t muzzle_world[3] = {};
    uint32_t muzzle_tick = 0;
    bool muzzle_valid = false;

    // --- network receive-apply: a remote peer's pose, read-applied on the host ---
    // The host stages a joiner's reported 0x0C pose into these engine-frame slots
    // [orig: NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9]. net_is_remote_peer
    // is the entity+0x24-bit0 "network-snapped" flag: when set the host SNAPS the live pose
    // to the report and the infantry motor SKIPS the entity (no re-simulation)
    // [orig: Entity_UpdateInfantryAI @0x4b9a03]. net_smooth_target/heading/pitch are staged
    // for the CLIENT-side interpolation smoothing (motor fall-through @0x4b9a8c, is_authority==0)
    // — a deferred, client-only concern; the authority host never interpolates. NEVER set for
    // the host's own player (ADR-0012 amendment / §5.38).
    bool    net_is_remote_peer = false;  // entity+0x24 bit0 (net-snap + motor-skip)
    int32_t net_smooth_target[3] = {};   // entity+0x234/+0x238/+0x23C (16.16 world)
    int32_t net_smooth_heading = 0;      // entity+0x240 (BAM32; also mirrored to heading/+0x10)
    int32_t net_smooth_pitch = 0;        // entity+0x244 (BAM32; also mirrored to pitch/+0x14)
    int16_t net_interp_progress = 0;     // entity+0x27C (reset to 0 on each read-apply)
    // Deferred client-interp bookkeeping (consumed by the @0x4b9a8c fall-through only):
    int32_t net_saved_live_pose[3] = {}; // entity+0x80/+0x84/+0x88 (interp delta basis)
    int16_t net_interp_steps = 0;        // entity+0x27E (2..16; buckets {3,4,5,8,16})

    int32_t vel_x = 0;         // entity+152
    int32_t vel_z = 0;         // entity+156
    int16_t health = 100;      // entity+286 (<=0 -> death path)
    int32_t net_id = 0;        // entity+124 (RelationMatrix_SetBitA key / DcbId)
    uint16_t relmat_id = 0;    // entity+284 (RelationMatrix_SetBitB key)
    uint8_t team = 0;          // entity+354 (team id; 0 = neutral)
    bool see_all = false;      // entity+104 aiSlot[4] & 0x200 (targets any team)
    int32_t arrival_prox = 0;  // entity+32 def +2340 (heading arrival proximity, patrol)
    // brain[2] "scheduler" per-entity patrol state. Tracked deviation: the original keeps the
    // frame budget (+16) and the patrol triple (+0/+4/+8) in one struct reached via brain[2];
    // we keep the budget shared (AiSystem::scheduler, the foundation's frame-stagger model) and
    // model the per-entity patrol fields here. Whether brain[2] is global or per-entity/per-group
    // is an open RE TODO (notes §9); reconciling would move the budget here too.
    int32_t patrol_f0 = 0;     // scheduler +0
    int32_t patrol_delta = 0;  // scheduler +4 (patrol heading delta)
    int32_t patrol_goal = 0;   // scheduler +8 (patrol goal active -> still en route)

    // Infantry motor state (org1-class soldiers). When inf.active the entity is driven
    // by AiSystem::tick_infantry [orig: Entity_UpdateInfantryAI @0x4b9910] instead of the
    // vehicle state machine; promote routes BMS organics here. See world/infantry.h.
    InfantryState inf;

    // Per-entity collision-resolver state (prev-position gating + the idle skip
    // throttle). [orig: entity savedLivePose + pad_370[3]; collision.h]
    CollisionWorld::ResolveState collide_state;
};

// A resolved AI target — the fields the engagement bookkeeping reads off the target entity.
// [orig: best_target in AI_HandleEvent_HelicopterCombatD @0x467730.]
struct AiTarget {
    int32_t relmat_id = 0;       // target+284 (pad6_pre[24], i16) — relation-matrix key
    int32_t net_id = 0;          // target+124 (DcbId)
    bool has_controller = false; // target pad3_pre[48] nonzero -> PRNG branch A (inline) vs B
    EntityHandle handle;         // container rebase: the world handle (orig: the entity ptr)
};

// A perception candidate for acquire_target. [orig: AI_FindBestTargetB @0x466f60 scans the
// g_pool_list pools; we scan an injected eligible-candidate list — the weapon-slot pool
// selection + vehicle/building sub-filter (profile+40 slot config) and the relation-matrix
// team filter are deferred to the weapon/relation phase. The per-candidate perception gates +
// FOV/range/stealth scoring + LOS + priority bypass are faithful.]
struct AiCandidate {
    EntityHandle handle;
    int32_t pos[3] = {};        // candidate+4/+8/+12 (X/Y/Z, 16.16 fixed)
    uint8_t team = 0;           // candidate+354
    int32_t flags = 0;          // candidate[9] (+36): &2 ignore, &0x4000 priority, &0x8000000 skip
    int16_t health = 100;       // candidate+286 (*143)
    int32_t visibility = 0;     // candidate+530 (*265): stealth counter (0 = fully visible)
    int32_t range_primary = 0;  // candidate+422 (*211): max primary-FOV engage range
    int32_t range_secondary = 0;// candidate+420 (*210): max secondary-FOV engage range
    int32_t relmat_id = 0;      // candidate+284
    int32_t net_id = 0;         // candidate+124 (DcbId)
    bool has_controller = false;// candidate pad3_pre[48]
    bool is_priority = false;   // == profile+148 priority target -> LOS-only bypass
    bool los_blocked = false;   // Entity_CheckMutualLineOfSight @0x539be0 (default: clear)
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

// [orig: AIWaypoint_UpdateTarget @0x457380] Resolve the brain's current waypoint
// target, writing bearing (kWpBearing, BAM), distance (kWpDistance), and node
// payload into the waypoint sub-struct. `pos` is the entity's X/Y/Z (entity+4/+8/+12).
// Returns 0 on success / type-2 / type-3; returns -1 when a type-1 nav target can't
// be resolved (channel 0, missing count) or the type is unknown.
int ai_waypoint_update_target(AiBrain &b, const int32_t pos[3], const NavNodeTable &nav);

// [orig: Entity_ApplyCommand @0x43ab60] Apply a BMS AI-change command sub-type to an AI
// component in-engine (the original mutates entity[25]; we mutate the brain directly). Only
// PLAYPARTANIM (sub-type 0x22) is ported: it writes the part-anim channel's sweep direction
// (comp+436) and rate (comp+444), computed from ANIMTIME via the verified FPU constants
// (1/65536, 0.016, 65536). p2=channel(1/2), p3=play_type(-1/0/+1), p4=time(16.16 seconds).
// Other sub-types (alert/accuracy/state/speed/...) are tracked-TODO no-ops; see
// notes/mission/anim-ai-grill-2026-06-07.md.
void ai_apply_command(AiBrain &comp, int sub_type, int32_t p2, int32_t p3, int32_t p4);

// [orig: Entity_CalcAverageGroundHeight @0x457230] The entity-def height offsets
// (def+0x2C alive / def+0x30 dead) modeled as named members — a tracked deviation,
// those def fields are not yet RE'd (default 0 = origin sits at ground). The stand
// clearance the movers add on top (brain[131] = ground + 0x50000) is NOT here; it
// lives in AiSystem::apply_ground_clamp.
struct GroundClearance {
    int32_t alive_offset = 0;  // [orig: def+0x2C] added to ground when alive
    int32_t dead_offset = 0;   // [orig: def+0x30] added when dead / flagged
    bool use_dead = false;     // [orig: (entity+36 & 2) || health<=0, gated by entity+52]
    bool has_physics = false;  // [orig: entity+368 != 0] enables the worldY water clamp
};

// [orig: Entity_CalcAverageGroundHeight @0x457230] 5-tap weighted ground height,
// 16.16 fixed. `pos` is the entity X/Y/Z (16.16); `sample_radius` is 16.16 (the AI
// movers pass 0x50000 = 5.0). Samples the bilinear terrain column at center + N/S/E/W:
//   result = (N + S + E + W + 2*(C + 2*max)) / 10, clamped >= C, then the worldY water
//   clamp (when has_physics) and the def alive/dead offset.
// Tracked deviation: the original per-tap sampler is the hi-res down-raycast
// raycast_entity_collision -> Terrain_RaycastHeightmapHiRes_0 @0x60e710; we use the
// renderer-accurate bilinear column height (the near-vertical raycast's result),
// deferring the sub-cell along-ray refinement. Returns INT32_MIN when the field is
// invalid (faithful "no terrain" fallback — leave the entity's Z untouched).
int32_t calc_average_ground_height(const terrain::TerrainHeightField &field,
                                   const int32_t pos[3], int32_t sample_radius,
                                   const GroundClearance &clearance);

// A recorded RelationMatrix_SetBitA/B side effect (net-replication bookkeeping the
// mover emits per node advance). The live TriggerRelations matrices are updated alongside
// this diagnostic trace by the route-arrival seam.
struct RelMatCall {
    int which;     // 0 = SetBitA (net_id key), 1 = SetBitB (relmat_id key)
    int32_t key;   // entity+124 (A) or entity+284 (B)
    int32_t channel;
    int32_t node;
};

// The 8 relation-matrix bit-set ops emitted on target engagement, in call order.
// [orig: AI_HandleEvent_HelicopterCombatD @0x4677b3..0x4678b2.] The matrices live in the
// net/relation layer; we record the calls (op + the two keys) rather than apply them.
enum RelOp {
    kRelEventSpecial = 0, // EventMatrix_SetSpecialBit   @0x452a40 (self relmat, target relmat)
    kRelSharedMem = 1,    // SharedMem_Init              @0x452ad0 (self net,    target relmat)
    kRelProximity = 2,    // EntityMatrix_SetProximityBit@0x452b60 (self relmat, target net)
    kRelEnemy = 3,        // TeamMatrix_SetEnemy         @0x452bf0 (self net,    target net)
    kRelAllied = 4,       // TeamMatrix_SetAllied        @0x452aa0 (self relmat, target relmat)
    kRel452B30 = 5,       // sub_452B30                  @0x452b30 (self net,    target relmat)
    kRelDamaged = 6,      // EntityMatrix_SetDamagedBit  @0x452bc0 (self relmat, target net)
    kRelSpotted = 7,      // TeamMatrix_SetSpottedBy     @0x452c70 (self net,    target net)
};
struct RelOpCall { int op; int32_t a; int32_t b; };

// [orig: AI_FindBestTargetB @0x466f60 scoring core] Combined FOV/range/stealth/priority score
// for a candidate (16.16 fixed point, 64-bit intermediates, +0x8000 rounding). Returns -1 when
// the candidate is outside both FOV/range gates (the orig's LABEL_17 skip) — distinct from a
// legitimate in-gate score of 0 (a fully-stealthed target), which the caller needs to honor the
// priority-bypass ordering. `angle_diff` is the folded BAM heading delta in [0,128]; `distance`
// is ftol2(dist3d) >> 16 (world units). primary/secondary_fov are the profile arc bytes already
// OR'd with 1; the *_max args are the engage-range caps.
int32_t ai_score_target(int angle_diff, int distance, int primary_fov, int secondary_fov,
                        int primary_max, int secondary_max, int cand_primary_max,
                        int cand_secondary_max, int visibility, int cand_flags);

// The AI subsystem: a world::ISystem ticking all AI brains on the shared world.
class AiSystem : public ISystem {
public:
    AiSystem();

    const char *name() const override { return "ai"; }
    void tick(World &world, const TickContext &ctx) override;

    // Re-seed every brain to the captured spawn baseline + clear the transient queues.
    // [Drives World::restore: load_systems() calls on_load on Play->Stop, so the AI
    // rewinds alongside the registry/vars/env the World snapshot restores. No-op until
    // capture_spawn_baseline() has run.]
    void on_load(World &world) override;

    // Capture the current AI state as the restore baseline. The host calls this once at
    // play start (after promote + the pre-mission pass), when it snapshots the World.
    void capture_spawn_baseline();

    // Attach a brain to a world entity; returns its AI index (faithful to the
    // unk_AED380 array index used by AIEvent entity_index).
    int attach(EntityHandle h);
    AiEntity *at(int ai_index);
    AiEntity *for_handle(EntityHandle h);
    int count() const { return static_cast<int>(entities_.size()); }

    // Embedder muzzle seam (D-AI-6): stamp an entity's posed muzzle world position
    // (16.16 fixed, mission frame) for the fire pass to consume while fresh.
    // The embedder's present layer pushes this each frame from the model's gun-flash
    // userpoint x the animated skeleton. [orig: Entity_GetAttachmentWorldPosition
    // @0x4b2670 — computed inline in the original's fire block; ours is embedder-fed
    // because libs/world carries no skeletal pose.]
    void set_entity_muzzle(EntityHandle h, const int32_t pos[3], uint32_t logic_tick);

    AiScheduler scheduler;
    AiEventQueue events;
    NavNodeTable nav;         // channel/node table the waypoint mover walks
    bool is_authority = true; // [orig: g_napi_np_ctx.is_authority]
    bool is_in_session = false;

    // Locomotion: apply the mover output (kOutSpeed/kWorkPos*/kWorkHeading) to the entity
    // transform each tick. The AI brain is byte-exact (P1/P2); the entity-movement physics
    // (the unanalyzed driver near 0x462120 + collision/terrain) is NOT reversed, so this is a
    // clean kinematic integrator: turn to the mover heading + advance toward the target. The
    // AI-speed -> world-units factor lives in that physics; loco_scale models it (a visual
    // default until the driver is RE'd). Disable to tick AI decisions without moving entities.
    bool locomotion_enabled = true;
    int32_t loco_scale = 32768; // out_speed (AI units) * loco_scale = 16.16 world-units / tick

    // Terrain grounding. When `terrain` is wired (NovaSimulation::set_terrain_height_field),
    // apply_ground_clamp drives the entity's vertical (pos[2], engine Z = up) off the real
    // terrain sampler each tick so promoted entities hug the ground instead of floating. Null
    // (the default) leaves Z at the authored spawn value — the headless AI unit tests run
    // terrain-free. [orig: the movers recompute brain[131] from Entity_CalcAverageGroundHeight
    // every step; see apply_ground_clamp.]
    const terrain::TerrainHeightField *terrain = nullptr;
    GroundClearance ground_clearance{};
    // World-object collision (host-wired like `terrain`; null = terrain-only motor).
    // When set, the tick rebuilds the proximity tables [orig: Entity_UpdateAllEntities
    // @0x4c2100 -> Entity_BuildAllProximityLists @0x4c20f0] and the infantry vertical
    // resolve routes through CollisionWorld::resolve_entity (D-INF-3 burn-down).
    CollisionWorld *collision = nullptr;
    // [orig: the +0x50000 the movers add after grounding — AI_ProcessMovementStep @0x466db0
    // brain[131] = ground + 0x50000; AI_UpdateMovementTarget @0x460e40 adds def heightOffset.]
    // NOTE: the INFANTRY motor (tick_infantry — player AND AI) does NOT use this. It settles
    // pos[2] to ground + the anim frame's capsule_bottom (origin->feet) per the witnessed
    // collision capsule [orig: movement collision resolver @0x4b2bd0; D-INF-6];
    // +0x50000 is only the id-3 death-fall mover's vertical target slot. Still used by the
    // vehicle/SM path (apply_ground_clamp).
    int32_t ground_stand_offset = 0x50000; // 5.0 in 16.16 (vehicle/SM ground clamp only)

    // ---- Infantry motor (org1 soldiers; docs/world/world-wac-ai-re.md §3) ----
    // Root-motion provider; injected like `terrain`. Null = no clips: every state is
    // unavailable, the selector idles, and infantry entities stand still (no model fallback —
    // motion comes from clips, as in the original).
    IRootMotionSource *root_motion = nullptr;
    // Fall-damage scale [orig: dword_C6EAE4, a runtime config (0 in the image; writer
    // @0x4301bc unread). Damage when landing with vel_z <= -1057*scale: health -= excess>>4
    // @0x4b9910 dump 5152]. 0 disables (the image default until the config source is RE'd).
    int32_t fall_damage_scale = 0;
    int unported_calls = 0;   // coverage counter for not_yet_ported handlers
    int find_target_calls = 0;// coverage: target-acquisition invocations
    std::vector<RelMatCall> relmat_calls; // diagnostic trace of the applied mover side effects

    // ---- P2: GROUND combat + targeting ----
    std::vector<RelOpCall> rel_ops;        // recorded engagement relation-matrix ops (trace;
                                           // the APPLY now writes world.relations — D-AI-3)
    std::vector<int32_t> target_set_calls; // recorded Entity_SetAITarget net-ids (@0x45d760)
    uint32_t prng_a = 0;    // [orig: dword_31BFBB8] engagement fire-delay jitter stream
    uint32_t prng16 = 0;    // [orig: dword_31BFBB0] PRNG_Next16 stream
    uint16_t fire_shot_seq = 0; // per-shot sequence word [orig: word_B7C670]

    // The shared 32-bit rotate LCG: s = rotl(s + rotl(s,11), 4) ^ 1; returns the new state.
    int32_t prng_step_a();  // [orig: inline LCG on dword_31BFBB8]
    int32_t prng_step16();  // [orig: PRNG_Next16 @0x6130a0, dword_31BFBB0]

    int index_of(const AiEntity &e) const; // AI index (= AIEvent entity_index)

    // [orig: the shared death-velocity event @0x467730/0x457d70/0x467400] queue a crash(3) or
    // still(4) AIEvent by horizontal speed (sqrt(vx^2+vz^2), >=1057 -> 3 else 4; channel 0).
    void queue_death_event(AiEntity &e);

    // [orig: AI_FindBestTargetB @0x466f60] the candidate FEED (D-AI-1): scan the registry
    // pools with the witnessed gates (team/see-all, flags 2/0x8000000, health, +530 refcount
    // saturation, per-candidate range caps) into a scratch list, then run the scoring core.
    // Deviations tracked in the ledger: the profile weapon-slot class table (+40+4i, ai.def)
    // is unparsed -> pools 0 and 1 scan unconditionally; LOS = line_of_sight_clear.
    bool acquire_target(World &world, AiEntity &e, AiTarget &out);

    // The byte-exact scoring core over an explicit candidate list (the P2 port; tests pin
    // it directly). [orig: AI_FindBestTargetB @0x466f60 scoring walk]
    bool acquire_target_from(AiEntity &e, const std::vector<AiCandidate> &candidates,
                             AiTarget &out);

    // [orig: the engagement block @0x4677b3..0x4678b2] APPLY the sees+targeted quads to
    // world.relations (D-AI-3 closed) + record the trace, Entity_SetAITarget, reset the
    // combat timer, set the fire-delay (exact PRNG jitter; the has_controller branch is
    // guarded by base-delay, the other is unconditional), pending = 17.
    void engage_target(World &world, AiEntity &e, const AiTarget &t);

    // [orig: Entity_SetAITarget @0x45d760] brain[kTargetSlot] + AiSlot[3] = handle;
    // maintain the OLD/NEW targets' Entity::ai_target_refcount (dec clamp >=0 / inc).
    void ai_set_target(World &world, AiEntity &e, EntityHandle target);

    // The 8 relation-matrix writes of acquisition/fire: the sees quad + the targeted quad,
    // in the witnessed order/keys (group = Entity::group_id +0x11C, single = net_id +0x7C).
    // [orig: @0x4677b3..0x4678b2 / @0x4b0a6f..0x4b0ae2; matrix identity via the setter
    // bases g_SeesMatrix*/g_TargetedMatrix* — world-wac-ai-re §16.4/§17.2]
    void apply_engage_relations(World &world, const Entity &self, const Entity &target);

    // LOS between two 16.16 fire-origin points, true = clear: the terrain leg (ported
    // heightmap raycast) + the sector leg (pool-2/pool-1 collision-model clip via
    // CollisionWorld::raycast_clear — the D-AI-7 leg). `from`/`to` are the sighting
    // pair, excluded from the sector walk with anything standing on them. No terrain
    // wired = clear (the headless-test default); no collision world wired = terrain
    // leg only. [orig: Entity_CheckMutualLineOfSight @0x539be0 ->
    // Physics_RaycastTerrainAndSectors @0x539910, ray radius 0, 1 = clear]
    bool line_of_sight_clear(World &world, const int32_t a[3], const int32_t b[3],
                             EntityHandle from, EntityHandle to) const;

    // [orig: Entity_AlertNearbyAllies @0x4654b0] pool-1 (rebase: + pool-0 organics with
    // brains) same-team, alive, non-building entities within `radius_units` (16.16):
    // own AiSlot+136 = 2 and each ally's brain alert = 2.
    void alert_nearby_allies(World &world, AiEntity &e, int32_t radius);

    // AI fire -> the authoritative round path (the Entity_FireWeaponAndSendPacket @0x42bd80
    // authority leg): ring append (the S2C 0x0A tag-2 fan-out) + RoundSim spawn — the same
    // pair the C2S 0x06 player path enters. Host-only (callers are authority-gated).
    // [orig: WeaponSlot_FireAndSpawnEffects @0x53f440 -> Server_ClientFiredRound @0x50baa0
    // ring append + RoundData_SpawnRound @0x4ec0d0; net-re §5.60]
    bool fire_ai_round(World &world, AiEntity &e, const int32_t origin[3], int32_t yaw_bam,
                       int32_t pitch_bam, int32_t ammo_index);

    // [orig: AI_HandleCommand @0x465770] AI command dispatcher (cases 6..0x16). Deferred to the
    // AI-command phase; for damage/death/destroy events (1/3/4) the original returns 0, so this
    // returns false and the combat event switch proceeds faithfully.
    bool ai_handle_command(AiEntity &e, const AiEventEntry &ev);

    // [orig: AI_BeginUpdate @0x457b40] budget gate. Returns false (skip this frame)
    // when the shared scheduler budget exceeds the cap; forces idle/fallback.
    bool begin_update(AiEntity &e);

    // [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event: 0=update,1=spawn,4=death.
    void process_infantry_state_machine(AiEntity &e, World &world, int event);

    // The shared pending-state transition (exit current, enter pending, commit).
    void apply_transition(AiEntity &e, World &world);

    // [orig: AI_UpdateWaypointMovement @0x457bd0] advance the brain along its path via
    // the nav table; writes the working target transform (kWorkPos*/kWorkHeading) and
    // out-speed (kOutSpeed). Applies the per-advance visited marks that BMS
    // SingleAtWaypoint/GroupAtWaypoint consume, and retains a diagnostic trace.
    int update_waypoint_movement(AiEntity &e, World &world);

    // Apply the mover output to the entity transform (turn to kWorkHeading, advance pos toward
    // the kWorkPos* target by kOutSpeed * loco_scale, clamped to not overshoot). See loco_scale.
    void apply_locomotion(AiEntity &e);

    // [orig: AI_ProcessMovementStep @0x466db0 brain[131] = ground + 0x50000 /
    // AI_UpdateMovementTarget @0x460e40 brain[131] = max(targetZ, ground)] Drive the vertical
    // off the terrain sampler: ground = calc_average_ground_height(...), then SET kWorkPosZ +
    // snap the entity's pos[2] to ground + ground_stand_offset. SET (not max) because our lean
    // waypoint mover (update_waypoint_movement) leaves kWorkPosZ stale, so a max would strand a
    // floating spawn. No-op when `terrain` is null or the column has no terrain coverage.
    void apply_ground_clamp(AiEntity &e);

    // Seat-follow phase for a LIVE mounted occupant. Infantry callers keep running
    // death, perception, combat, and animation around it and suppress only ordinary
    // locomotion; non-infantry callers may use the result as a full SM shortcut.
    // Dead occupants return false so the infantry death edge detaches first.
    // [orig: Entity_UpdateInfantryAI parent/health gate @0x4b9960..0x4b9983,
    //  mounted pose @0x4bec23..0x4bed3f, death detach @0x4b9c57..0x4b9c60.]
    bool pose_if_mounted(AiEntity &e, World &world);

    // The brain half of a waypoint REDIRECT (RedirectGroupTo/RedirectSingleTo): mode 1 +
    // list + node (nearest of the list when node < 0) + the per-leg turn-budget seed.
    // [orig: Entity_SetWaypointByTeam @0x43cdb4; nearest = Entity_FindNearestTriggerByType
    // @0x407ea0]
    void apply_route_order(AiEntity &e, int32_t list, int32_t node);

    // The vehicle-physics input staging for a PlayerControl vehicle without a live PLAYER
    // controller [orig: Entity_UpdateVehiclePhysics @0x48af00 — the parked stamp
    // @0x48c002-0x48c02d and the AI-driver leg @0x48bc12-0x48c034 (2026-07-16 witness)]:
    //  - controller == nullptr (or dead vehicle): hold heading + zero speed and stamp the
    //    brain into state 22 (the parked/player-mode state);
    //  - an AI controller: hand state 22 back to 16, cmd speed = min(brain outSpeed,
    //    player_speed), re-resolve the waypoint target when the per-leg turn budget
    //    (brain[32]) is spent, clamp the bearing delta to the budget, damp speed 0.75x
    //    per ~30/60 deg of residual turn when turn_rate2<<6 < budget, steer = heading +
    //    delta + delta/8, and fill `out` (ai_drive = true).
    // Tracked deferrals (D-NET-161): the minAI crew health clamp, the pool-1
    // collision-avoid damping, the wait-for-boarders stop, the handbrake byte-973 latch
    // and the aim-lock stop.
    void vehicle_ai_drive(World &world, Entity &veh, const Entity *controller,
                          const VehicleTraits &traits, VehicleDriveCmd &out);

    // Integrate part-anim phase dwords with retail's wrapping ADD for dir==1
    // and wrapping SUB for every other nonzero direction. Clamp/stop only on
    // strict upper/negative overshoot; an exact endpoint remains active.
    // PLAYPARTANIM writes only direction + rate (ai_apply_command case 0x22).
    // Runs regardless of the AI budget gate. [orig: integrator @ 0x456710]
    void advance_part_anim(AiEntity &e);

    // ---- Infantry motor [orig: Entity_UpdateInfantryAI @0x4b9910] ----
    // Per-tick update for inf.active entities (replaces the vehicle SM path for them).
    // Order: anim root advance -> death edge -> ground resample (every 8) -> think +
    // state selection (every 16, authority) -> body-heading turn -> slope slide (every 8)
    // -> rotate root delta by heading -> integrate + gravity/ground (every 2).
    void tick_infantry(AiEntity &e, World &world, uint32_t logic_tick);
    // The infantry combat pass (org1 riflemen; world-wac-ai-re §17.1-17.3/17.5, D-AI-4):
    // 32-tick staged perception -> target commit, then per-tick reactions (the attack
    // anims), move modes, and the lead+error aim solution. Authority + alive only.
    void infantry_combat_think(AiEntity &e, World &world, uint32_t key);
    // The anim-event sound pass (§17.4 sounds): the six SSAudio foley bits
    // (0x20..0x400 -> slots 24-29) then the two footstep bits (0x1/0x2 -> the
    // surface-picked slots 17-23, position dipped to foot level by the frame's
    // capsule bottom). NPC body on ODD ticks, player body on EVEN — the two
    // updaters run opposite halves of the 62 Hz tick.
    // [orig: Entity_UpdateInfantryAI @0x4bf169-0x4bf2b0 (gate @0x4bf144);
    //  Entity_UpdateInfantryPlayerBody @0x4b76f1-0x4b78a8 (gate @0x4b76e6)]
    void infantry_anim_sound_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                  int32_t capsule_bottom);
    // Resolve the entity's sound profile slot to its authored set name and queue
    // the world SoundSlotEvent (empty slot = the id-0 no-op, nothing queued).
    // [orig: Entity_GetProfileSlotSound @0x528300 ->
    //  Entity_PlaySound3D_FullVolume @0x528e20]
    void emit_slot_sound(World &world, const AiEntity &e, int slot, const int32_t pos[3]);
    // The infantry fire pass (§17.4): consume the .bad anim-event trigger bits
    // (inf.last_events, odd ticks) + the walking-fire latch -> fire_ai_round; magazine
    // decrement + the reload trigger. Runs AFTER the anim advance refreshed last_events.
    void infantry_fire_pass(AiEntity &e, World &world, uint32_t logic_tick);
    // Dedicated UseGun request after the ordinary anim-event block: resolve the parent
    // emplacement weapon, then require target/range/alignment before authoritative fire.
    // The occupant's personal profile ammo is never used.
    // [orig: Entity_AttachToUseGunSlot @0x546b80; request @0x4bf4bb..0x4bf59e.]
    void infantry_mounted_fire_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                    uint32_t key);
    // The post-entity global action pump for occupied emplacement MountSlots. The
    // infantry request above only writes next=FIRE; this phase advances the authored
    // weapon FSM and emits the round with the NPC gunner as owner.
    // [orig: frame order @0x52674b/@0x526786; WeaponAction_ProcessAllEntities @0x542690.]
    void pump_mounted_weapon_slots(World &world, uint32_t logic_tick);
    // Reconcile the split AiEntity/registry stores, wire animation, and part channels
    // at either the mounted return or the ordinary end of the infantry tick.
    void finish_infantry_tick(AiEntity &e, World &world);
    // AUTHORITY body-anim selection for a net-snapped REMOTE player. The movement motor must
    // not re-simulate a wire-snapped peer (tick_infantry skips it), but the retail authority
    // still runs the player-body ANIM selection for every player, consuming the REPLICATED
    // MoveOrder byte (bits 0-2 dir, bit 3 moving) + stance bits (C2S 0x1D -> MoveOrder bits
    // 8-9) every 4th tick, and the selected state/ratio feed that player's 0x0A record bytes
    // 14/15. Also mirrors the wire-anim fields onto the world Entity. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b40e0 — local-or-authority gate @0x4b70a3-0x4b70b2,
    // 4th-tick gate @0x4b70ce, selection @0x4b7183-0x4b729d, commit @0x4b7356-96]
    void remote_player_body_anim(AiEntity &e, World &world, uint32_t logic_tick);
    // Mirror the selected body-anim state + channel phase (and, for the local player, the
    // packed MoveOrder low byte) onto the world Entity the 0x0A snapshot reads.
    void mirror_wire_anim(AiEntity &e, World &world);
    // The 16-tick navigation think: waypoint channel walk (arrival, relmat marks, marker
    // wait + facing, one-shot end), commands 123..127. Writes inf.move_* + target_heading.
    void infantry_think(AiEntity &e, World &world);
    // Map the movement order to an anim state (walk/run/jog/turn/stop/wounded + availability
    // fallbacks) and commit it under the lock/emote rules.
    void infantry_select(AiEntity &e);
    // The witnessed org2 PLAYER-BODY selection, shared by the local player and the
    // authority-side remote-player path (the original runs ONE function for both):
    // moving base 1/11/19 + direction offset; idle 48 / 45 (46 idle_mortar for
    // ForceCrouch weapons) / 43-then-44; the forward-walk run promotion (run_2/run_3
    // by 2 + run_anim, scope-suppressed); prone lean rolls 41/42; airborne jump_loop;
    // commit via the flag-table arbitration. Callers gate it to every 4th tick.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b7183-0x4b7396; 4th-tick gate @0x4b70ce]
    void player_body_select(AiEntity &e);
    // The lean-angle producer, every body tick: decay lean -= (lean+8)>>4, then the
    // on-foot ramp -0x3000000 (left) / +0x3000000 (right) per held lean bit, gated
    // alive + not prone. [orig: decay @0x4b5c97; ramp @0x4b7dbf/@0x4b7dd6; the seated
    // (+0x168==1) +-0x1400000 variant and the Flags 0x20/0x100000 gate legs unported]
    void infantry_lean_tick(AiEntity &e);
    // The torso-roll producer (entity+0x2DC), every body tick: prone idle 48 decays it
    // toward level (torso -= (torso+8)>>4); the combat rolls 41/42 RAMP it
    // -/+0x4000000 (5.625 deg) per tick — the FP barrel-roll view; otherwise it
    // chases the entity's slope roll (+0x18) a sixteenth-step per tick with the LAG
    // clamped to roll +-0x0E38E380 (20 deg), which also snaps the wrapped post-roll
    // value back once the clip ends. Consumers: the FP camera roll = torsoRoll +
    // lean/4 and the section-14 head/spine roll overlay terms.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b5cff-0x4b5d6d + @0x4b700c-0x4b7025]
    void infantry_torso_roll_tick(AiEntity &e);
    // The upper-body weapon channel (the entity's SECONDARY AnimMap channel): per-tick
    // desired-state selection + the locked/emote commit rule + the clip-end deferred
    // promotion + the playhead advance. Local-player slice: reload 65 via the 80-tick
    // window; hold poses 50-61 / binoculars 64 / reload2 ride the AdmDef kind dwords
    // (unparsed) so the rifle default (mirror the primary) applies. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b5cab..0x4b5ea9 + AnimMap_UpdateDualChannels
    // @0x40b8c0; witness world-wac-ai-re.md §14.8.4/.5]
    // The per-tick half: the arms-dip/pitch-kick, the reload window, the deferred
    // promotion and the playhead advance. Runs the selection half below only on the
    // witnessed 16-tick slow-pass phase.
    void infantry_weapon_channel(AiEntity &e, World &world, uint32_t logic_tick);
    // The selection + commit half [orig: @0x4b5dad..0x4b5ea3]. Split out because the
    // original gates it to `(current_tick & 0xF) == 0` while the advance around it runs
    // every tick from AnimMap_UpdateDualChannels @0x40b8c0.
    void infantry_weapon_channel_select(AiEntity &e);
    // Availability resolution against root_motion->has_clip with the cited fallback chains.
    int infantry_resolve_state(int adm_id, int state) const;
    // The slope pass: 4 ground probes around the entity feeding the slide impulse and
    // the body_pitch/roll slope-conform chase — but ONLY for conforming bodies:
    // def attrib 0x200, an anim state with flag bit 2 (the prone family — crawl 19-26,
    // rolls 41/42, prone idle 48), or a grounded corpse. Every other body DECAYS
    // body_pitch/roll back to level 1/16-step — a live standing/crouched soldier
    // neither slope-leans nor slope-slides, which is what keeps the standing FP camera
    // level on hillsides (torso_roll chases roll; fp_roll = torsoRoll + lean/4).
    // Two witnessed legs: the org1 NPC leg every 8th tick (shifted small-angle slopes,
    // eighth-step chase, 2048 slide) and the org2 player leg every tick (decay) with
    // probes/chase every 2nd tick (atan2 slopes, quarter-step chase, 512 slide,
    // 60-deg live / 48-deg dead slide threshold, roll write skipped during 41/42).
    // [orig: Entity_UpdateInfantryAI @0x4ba10f-0x4ba34c;
    //  Entity_UpdateInfantryPlayerBody @0x4b6d95-0x4b6ff4]
    void infantry_slope_pass(AiEntity &e, uint32_t logic_tick, uint32_t key);

    const StateRow &row(int32_t state) const;

private:
    // Apply the paired SetBitB(group)/SetBitA(SSN) waypoint-arrival writes to the
    // shared trigger relations, retaining relmat_calls as a diagnostic trace.
    // [orig: AI_UpdateWaypointMovement @0x457c6d..0x457c88]
    void mark_waypoint_visited(AiEntity &e, World &world, int32_t list, int32_t node);
    void clear_handle_index();
    void rebuild_handle_index();

    std::vector<AiEntity> entities_;       // pool-relative; index == AIEvent entity_index
    std::vector<AiEntity> spawn_baseline_; // on_load restore target (editor Play->Stop)
    std::vector<int> handle_to_ai_index_;
    std::vector<EntityHandle> vehicle_pass_handles_; // per-tick scratch for the vehicle
                                                     // motor pass (reused, no realloc)
    std::vector<EntityHandle> mounted_weapon_handles_; // global UseGun pump scratch
    std::vector<AiCandidate> scan_candidates_;       // acquire_target feed scratch (reused)
    bool baseline_captured_ = false;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_AI_H
