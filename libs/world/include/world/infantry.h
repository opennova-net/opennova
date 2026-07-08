// Infantry motor: per-frame update for AI soldiers (org1) and the local player path
// that shares this port. IDA is the source of truth for the movement and animation
// state fields cited here.
#ifndef OPENNOVA_WORLD_INFANTRY_H
#define OPENNOVA_WORLD_INFANTRY_H

#include <cstdint>

#include "world/body_anim.h"

namespace opennova::world {

inline constexpr int kInfantryAnimStateCount = 200;

// State id -> .adm key without the "anim_" prefix. [orig: off_8135F0]
extern const char *const kInfantryAnimNames[kInfantryAnimStateCount];

// Per-state behavior flags. [orig: g_animStateFlagsTable]
extern const uint32_t kInfantryAnimFlags[kInfantryAnimStateCount];

namespace anim_state {
enum : int {
    kReset = 0,
    kWalkForward = 1,
    kRun2 = 9,
    kRun3 = 10,
    kWalkCrouchForward = 11, // 11..18: crouch walk directional block
    kWalkProneForward = 19,  // 19..26: prone walk directional block
    kJumpStart = 30,
    kJumpLoop = 31,
    kClimbIdle = 32,
    kSwimIdle = 36,
    kSwimForward = 37,
    kIdle = 43,
    kIdle2 = 44,
    kIdleCrouch = 45,
    kIdleProne = 48,
    kIdle3 = 49,
    kSit = 76,
    kReload = 65,
    kEmplaced = 67,
    kIdleLook = 125,
    kIdle2Look = 126,
    kDraggerIdle = 137,
    kDraggerWalk = 138,
    kGuard = 140,
    kGuardLook = 141,
    kWoundedWalk = 145,
    kWoundedRun = 146,
    kStop = 147,
    kJogForward = 148,
    kRunForward = 149,
    kPostAttack = 151,
    kOutOfGround = 153,
    kSwimAttack = 154,
    kCoverIdle = 163,
    kCoverRun = 164,
    kRunAttack = 167,
    kRunAway = 168,
    kRun2Crouch = 169,
    kDeathFire = 173,
    kDeathPungi = 174,
    kDeathDrown = 175,
    kDeathGrenadeBase = 176, // 176..179: death_grenade F/R/B/L
    kDeathBulletBase = 180,  // 180..199: death_bullet families
};
} // namespace anim_state

// Map the selected infantry state to the present-pass BodyAnim slot. Directional
// walk blocks all render through the same canonical walk slot.
inline int32_t body_anim_slot_from_state(int state) {
    if ((state >= anim_state::kWalkForward && state < anim_state::kWalkForward + 8) ||
        (state >= anim_state::kWalkCrouchForward && state < anim_state::kWalkCrouchForward + 8) ||
        (state >= anim_state::kWalkProneForward && state < anim_state::kWalkProneForward + 8))
        return kBodyAnimWalkForward;
    switch (state) {
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

uint32_t infantry_anim_flags(int state);

// One tick of AnimMap root output, in entity-local axes. [orig: AnimMap_UpdateEntity
// @0x40b5f0 tail]
struct RootMotionFrame {
    int32_t dx = 0; // out[0] = vel[2] * 32768, forward
    int32_t dy = 0; // out[1] = vel[0] * 32768, lateral
    int32_t dz = 0; // out[2] = vel[1] * 32768 or delta(bottom * 65536)
    uint32_t events = 0;
    int32_t capsule_bottom = 0; // out[3] = bottom * 65536
    int32_t capsule_top = 0;    // out[4] = top * 65536 + 0x2000
};

// Runtime source for real .bad/.adm data, with tests providing IDA-shaped doubles.
class IRootMotionSource {
public:
    virtual ~IRootMotionSource() = default;
    virtual bool has_clip(int adm_id, int state_id) const = 0;
    virtual bool advance(int adm_id, int state_id, int32_t &phase_ticks, RootMotionFrame &out) = 0;
};

struct InfantryState {
    bool active = false;
    bool is_local_player = false;

    // Packed local-player movement direction. The player body converts this index to
    // the real directional state inside the 1-8 / 11-18 / 19-26 walk blocks.
    // [orig: Player_PackInputStateToEntity @0x4df450; player body @0x4b40e0]
    bool player_moving = false;
    int player_move_dir_index = 0;
    // NPC route-order fields. Org1 think writes these; the local player is driven by
    // player_moving/player_move_dir_index instead.
    int move_dir_index = 0;
    int32_t look_pitch = 0;

    int move_mode = 0;
    int32_t target_dist = 0;
    int32_t arrival_radius = 0;
    int32_t move_target[3] = {};
    bool at_final_oneshot = false;

    int anim_state = anim_state::kIdle;   // entity[175]
    int anim_pending = 0;                 // entity[174]
    int anim_prev = anim_state::kIdle;    // entity[178]
    int32_t clip_phase = 0;
    // Standing-idle tick counter (entity+0x148): the player-body idle starts at 43 and
    // promotes to 44 once >= 62 idle ticks; any movement resets it. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b727b-0x4b7293 (state = 0x2B + (cnt >= 0x3E)),
    // reset @0x4b719b]
    int32_t idle_counter = 0;
    uint32_t last_events = 0;
    int32_t prev_capsule_bottom = 0;      // anim_slot[19]
    int32_t adm_id = 0;

    int32_t body_heading = 0;
    int32_t target_heading = 0;
    // Leg-chain chase yaws + their re-plant targets (BAM32): the feet keep pointing
    // where they were planted and shuffle after the body under the section-3.3
    // hysteresis; the render bone overlay consumes them as the R/L leg yaw.
    // [orig: entity +0x2d4/+0x2d8 (IDB "torsoYaw/torsoPitch" -- misnomers) chasing
    //  +0x2e4/+0x2e8; witness docs/world/world-wac-ai-re.md s3.3 + s14]
    int32_t leg_yaw[2] = {};              // 0 = right chain, 1 = left chain
    int32_t leg_target[2] = {};
    int32_t vel[3] = {};                  // entity+152/+156/+160

    // Local-player stance input. NPC org1 selection does not consume this field.
    // [orig: entity+0x12C prone bit 0x100, crouch bit 0x200; player body @0x4b40e0]
    enum class Stance : uint8_t { kStand = 0, kCrouch = 1, kProne = 2 };
    Stance stance = Stance::kStand;
    bool airborne = false;
    bool jump_requested = false;

    int32_t wait_cooldown = 0;            // entity[74]
    int32_t alert_timer = 0;              // entity[190]
    bool combat_reaction = false;         // entity+875
    int32_t ground_cache = 0;             // entity+676
    bool ground_cache_valid = false;
    int16_t max_health = 100;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_INFANTRY_H
