// Infantry motor: the per-frame update for AI soldiers (entity class org1).
//
// [orig: Entity_UpdateInfantryAI @ 0x4b9910, dispatched per class via
//  g_EntityClassPhysicsTable @ 0x82abc8 row "org1"]. Locomotion is ANIM-DRIVEN:
// the think layer (every 16 ticks) picks a movement order; the anim state machine
// picks a clip; the clip's root-motion track moves the entity. Full RE record and
// per-mechanic addresses: docs/world/world-wac-ai-re.md §3.
//
// Root motion is injected through IRootMotionSource (mirrors the TerrainHeightField
// injection): unit tests feed synthetic clips; the runtime host evaluates real .bad
// root tracks resolved by state name through the model's .adm (the names in
// kInfantryAnimNames are exactly the .adm keys, "anim_<name>").

#ifndef OPENNOVA_WORLD_INFANTRY_H
#define OPENNOVA_WORLD_INFANTRY_H

#include <cstdint>

#include "world/body_anim.h"

namespace opennova::world {

// ---------------------------------------------------------------------------
// Anim-state tables, extracted from the binary (IDB-verified 2026-06-10).
// ---------------------------------------------------------------------------

inline constexpr int kInfantryAnimStateCount = 200;

// State id -> clip key (= .adm key without the "anim_" prefix).
// [orig: off_8135F0 name-pointer table, 200 entries]
extern const char *const kInfantryAnimNames[kInfantryAnimStateCount];

// Per-state behavior flags. [orig: dword_8139E8]
// Observed bit semantics (each pinned at its use site in 0x4b9910 / 0x40b5f0):
//   bit0  0x001  movement state (interrupts emotes; client re-derives from velocity)
//   bit1  0x002  low-to-ground (slope-slide participates @0x4b9910 every-8 block;
//                also "harder to hit" when the TARGET is in such a state)
//   bit2  0x004  locked/uninterruptible (state change requests queue in anim_pending)
//   bit3  0x008  can aim/fire in this state
//   bit5  0x020  emote (yields only to movement states)
//   bit10 0x400  slow blend (15 ticks vs 10) [orig: AnimMap_UpdateEntity @0x40b65b]
// Remaining bits (0x40, 0x100, 0x200, ...) are carried verbatim; semantics TBD.
extern const uint32_t kInfantryAnimFlags[kInfantryAnimStateCount];

namespace anim_state {
enum : int {
    kReset = 0,
    kWalkForward = 1,      // base patrol gait
    kRun2 = 9,
    kRun3 = 10,
    kJumpStart = 30,
    kJumpLoop = 31,        // forces forward delta 1024 [orig: 0x4b9910 @ dump 4756]
    kClimbIdle = 32,
    kSwimIdle = 36,
    kSwimForward = 37,
    kIdle = 43,            // base idle
    kIdle2 = 44,           // combat idle (no target)
    kIdle3 = 49,           // combat idle (has target)
    kReload = 65,
    kIdleLook = 125,
    kIdle2Look = 126,
    kDraggerIdle = 137,
    kDraggerWalk = 138,
    kGuard = 140,
    kGuardLook = 141,
    kWoundedWalk = 145,
    kWoundedRun = 146,
    kStop = 147,           // turn-in-place when heading error > 45 deg
    kJogForward = 148,     // final-node approach gait
    kRunForward = 149,     // alerted gait
    kPostAttack = 151,
    kOutOfGround = 153,
    kSwimAttack = 154,
    kCoverIdle = 163,
    kCoverRun = 164,
    kRunAttack = 167,
    kRunAway = 168,
    kRun2Crouch = 169,     // 169..172 = stance-transition clips
    kDeathFire = 173,
    kDeathPungi = 174,
    kDeathDrown = 175,
    // 176..179 death_grenade F/R/B/L; 180..199 death_bullet {hip,torso,head,
    // rshoulder,lshoulder} x {F,R,B,L} -- picked by hit bone-section + quadrant
    // [orig: Entity_ComputeAnimSlotIndex @0x43a690 mode 4].
    kDeathGrenadeBase = 176,
    kDeathBulletBase = 180,
};
} // namespace anim_state

// Map the infantry motor's selected clip state (anim_state, set by infantry_select — the
// witnessed per-tick anim selection) to the canonical present-pass BodyAnim slot the host
// resolves against the model's .adm (body_anim.h). The org1 infantry motor (player AND AI)
// bypasses the brain-state update_body_anim_slot (ai.cpp), so this is where a soldier's
// Entity.anim_slot is derived; without it every org1 soldier renders a static T-pose (the slot
// stays at its -1 default and the present pass no-ops). Gaits map to walk/jog/run; idles,
// transitions, and the not-yet-modeled attack/reload/death body slots fall back to idle for now
// (a follow-up, like update_body_anim_slot's own fidelity note). [orig: Entity_UpdateInfantryAI
// @0x4b9910 selects the body anim each tick]
inline int32_t body_anim_slot_from_state(int state) {
    switch (state) {
        case anim_state::kWalkForward:
        case anim_state::kSwimForward:
        case anim_state::kDraggerWalk:
        case anim_state::kWoundedWalk:
            return kBodyAnimWalkForward;
        case anim_state::kJogForward:
            return kBodyAnimJogForward;
        case anim_state::kRun2:
        case anim_state::kRun3:
        case anim_state::kRunForward:
        case anim_state::kWoundedRun:
        case anim_state::kCoverRun:
        case anim_state::kRunAway:
            return kBodyAnimRunForward;
        default:
            return kBodyAnimIdle;
    }
}

// Per-state flag helpers (bounds-safe: out-of-range states have no flags).
uint32_t infantry_anim_flags(int state);

// ---------------------------------------------------------------------------
// Root-motion injection.
// ---------------------------------------------------------------------------

// One tick of evaluated root motion, in ENTITY-LOCAL axes (pre-heading-rotation),
// 16.16 world units. dx = forward, dy = lateral, dz = vertical. events = the
// frame's .bad event bits ([orig: dword_A2ED08]: bit0/bit1 footstep L/R,
// 0x20..0x400 cloth/gear; consumed by the present/audio host, recorded here).
//
// Pinned source semantics [orig: AnimMap_UpdateEntity @0x40b5f0 tail, 0x40b82f..]:
// the clip's per-frame root record is the .bad "events" array {vel[3], capsule_bottom,
// capsule_top, trigger} (fence-post, frame_count+1 records, lerped pairwise):
//   dx = lerp(vel[2]) * 32768   [flt_7C32B4; 32768 = 65536/2 bakes the ~2-sim-ticks-
//                                per-30fps-frame ratio: walk ~0.066 -> ~2.0 u/s]
//   dy = lerp(vel[0]) * 32768
//   dz = delta(lerp(capsule_bottom) * 65536)  [flt_7C32BC; prev stored per entity,
//        reset on climb 32..35 / death_grenade 176..179 — ~0 in gaits, lifts in climbs]
//   events = trigger (lower keyframe, unlerped)
//   capsule_bottom = lerp(bottom) * 65536  ABSOLUTE origin->feet (entityRadius); the on-foot
//        ground settle floors the model-origin pos[2] to ground + capsule_bottom so the capsule
//        bottom rests on the terrain (a waist-origin soldier's feet land on the ground) [orig:
//        Entity_ProcessCollisionAndPlatformPhysics @0x4b2bd0 settles entity[3]=entityRadius+
//        ground @0x4b3da3; entityRadius = AnimMap_UpdateEntity @0x40b82f out_transform[3]=
//        bottom*65536; on-foot caller org1 @0x4bf7fa; docs/world/world-wac-ai-re.md D-INF-6].
//   capsule_top = lerp(top) * 65536 + 0x2000  [orig: out_transform[4]]
// Grilled against real clips in tests/anim/root_motion_test.cpp.
struct RootMotionFrame {
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t dz = 0;
    uint32_t events = 0;
    // Absolute collision-capsule extents for this frame, 16.16 (NOT deltas). The on-foot ground
    // settle uses capsule_bottom (origin->feet) as the floor; see the doc above and D-INF-6.
    int32_t capsule_bottom = 0;
    int32_t capsule_top = 0; // [orig: out_transform[4] = top*65536 + 0x2000]
};

// Clip provider keyed by anim state id. `phase` is the per-entity playhead in
// ticks owned by the caller (entity field), advanced by the source so looping /
// clip-length policy lives with the clip data. Returning false = clip missing
// (state unavailable; the selector falls back per the availability rules).
class IRootMotionSource {
public:
    virtual ~IRootMotionSource() = default;
    virtual bool has_clip(int state_id) const = 0;
    virtual bool advance(int state_id, int32_t &phase_ticks, RootMotionFrame &out) = 0;
};

// ---------------------------------------------------------------------------
// Per-entity infantry state (fields cite their entity offsets in the original).
// ---------------------------------------------------------------------------

struct InfantryState {
    bool active = false;        // routed through the infantry motor (org1 class)

    // The local player runs this SAME motor, but sources its move order from input
    // (world::apply_player_move_order) instead of the 16-tick AI think, and never
    // interpolates — [orig: Entity_UpdateInfantryAI @0x4b9910 @0x4b9a74 takes the simulate
    // branch loc_4B9C3E when is_authority || entity==g_local_player_entity; net-re §5.38].
    bool is_local_player = false;
    // 8-way move direction relative to the body facing, as a BAM32 offset added to
    // e.heading only for the player's root-motion rotation (0 = forward, ±90 = strafe,
    // 180 = back). The body still FACES target_heading (the look). Set from the witnessed
    // 8-way move_direction_index [orig: Player_PackInputStateToEntity @0x4df450]. The
    // forward-walk clip serves all 8 directions for now (dedicated strafe/back clips are a
    // tracked follow-up, net-re §5.38).
    int32_t move_offset = 0;
    // The local player's look pitch (entity Pitch@+0x14, BAM32), set from the mouse. The
    // motor applies it directly (look wins over the slope lean) and the first-person camera
    // reads it. [orig: Input_HandleActionBinding_0 @0x4e1330; net-re §5.38]
    int32_t look_pitch = 0;

    // Movement order, refreshed by the 16-tick think.
    // [orig moveMode local in 0x4b9910: 0 stop, 3 move-to-current-node,
    //  4 waypoint-walk (advanced past a node), 1/2/5/7/8/12 combat maneuvers
    //  (combat detail pass pending)]
    int move_mode = 0;
    int32_t target_dist = 0;
    int32_t arrival_radius = 0;
    int32_t move_target[3] = {};   // world target the order steers toward
    bool at_final_oneshot = false; // at the last node of a one-shot path (gait approach)

    // Anim machine.
    int anim_state = anim_state::kIdle;   // entity[175]
    int anim_pending = 0;                 // entity[174] (queued request)
    int anim_prev = anim_state::kIdle;    // entity[178]
    int32_t clip_phase = 0;               // channel playhead (ticks)
    uint32_t last_events = 0;             // last frame's .bad event bits

    // Heading pipeline (BAM32). body = entity[35] (+140), target = entity[106]
    // (+424), aim = entity[187]; torso/head stages feed bone overlays (deferred
    // with the skeletal overlay work, ADR skeletal).
    int32_t body_heading = 0;
    int32_t target_heading = 0;

    // Velocity accumulator (slide/knockback/gravity), entity+152/+156/+160.
    int32_t vel[3] = {};

    // Think bookkeeping.
    int32_t wait_cooldown = 0;   // entity[74], think-ticks ((wait+8)>>4 on arrival)
    int32_t alert_timer = 0;     // entity[190] (nonzero -> run gait)
    bool combat_reaction = false; // byte entity+875
    int32_t ground_cache = 0;    // entity+676, resampled every 8 ticks
    bool ground_cache_valid = false;
    int16_t max_health = 100;    // [orig: def+380; wounded gait at <= half]
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_INFANTRY_H
