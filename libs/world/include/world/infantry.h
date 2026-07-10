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
    // The weapon-channel hold-pose ladder [orig: g_animStateNameTable @ 0x8135F0
    // indices 50-61; selected by the special_hold kind @ 0x4b5dc0..0x4b5e35].
    kHoldKnife = 50,
    kHoldPistol = 51,
    kHoldGrenade = 52,
    kHoldStinger = 53,
    kHoldDesignator = 54,
    kHoldDesignatorScoped = 55,
    kHoldP90 = 56,
    kHoldP90Scoped = 57,
    kHoldMP7 = 58,
    kHoldMP7Scoped = 59,
    kHoldJavelin = 60,
    kHoldJavelinScoped = 61,
    kKnifeAttack = 62,
    kGrenadeAttack = 63,
    kBinoculars = 64,
    kSit = 76,
    kReload = 65,
    kReload2 = 66,
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
    // Clip length for a state's track, in the phase-tick convention advance() uses
    // (half-frame ticks), or -1 when the state has no track. The weapon channel's
    // deferred-state promotion fires when the playhead reaches this — the original's
    // clip-end channel flag [orig: the 0x20000 end-flag promotion in
    // AnimMap_UpdateEntity @ 0x40b77b; witness world-wac-ai-re.md §14.8.1].
    virtual int32_t clip_length_ticks(int adm_id, int state_id) const = 0;
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
    // The SECONDARY (upper-body weapon) AnimMap channel's state pair + playhead:
    // target state, clip-end-deferred state, and its own playhead — the entity
    // +0x2C8/+0x2C4 pair the dual-channel update swaps through the shared machinery.
    // [orig: AnimMap_UpdateDualChannels @ 0x40b8c0; witness world-wac-ai-re.md §14.8]
    int wpn_state = anim_state::kIdle;    // entity+0x2C8
    int wpn_deferred = 0;                 // entity+0x2C4
    int32_t wpn_clip_phase = 0;
    // The 3P reload-anim window: 80 ticks, stamped by the reload refill and counted
    // down once per tick; while nonzero the weapon channel wants state 65 reload
    // (66 reload2 when the hold kind is 2, pistol). [orig: entity+0x372 byte;
    // stamp WeaponSlot_ReloadAmmo @ 0x54173c, decrement @ 0x4b5cf9; §14.8.5]
    int32_t reload_anim_ticks = 0;
    // The held weapon's 3P body-channel hold kind — the AdmDefs record dword +0xA4 the
    // original reads through byte entity+0x2B0 each tick; mirrored from the equipped
    // def's special_hold key. 1..8 selects the 50-61 pose ladder (2 also selects
    // reload2); 0 = rifle default, mirror the primary. [orig: read @ 0x4b5dba;
    // parser key 'special_hold' @ 0x543cb7]
    int wpn_hold_kind = 0;
    // Host-issued identity serial for the resolved held AnimMap. This lives on the
    // entity (the original's previous-held record is per entity), so a fresh local
    // player receives the initial switch stamp even when the simulation keeps the
    // same equipped weapon across a world/player replacement.
    uint64_t wpn_anim_map_serial = 0;
    // Local-player Flags-bit mirrors, refreshed per tick by the host [orig: the
    // @ 0x4b5d7f..0x4b5da9 refresh — Flags|0x10 from g_weaponScopeActive,
    // Flags|8 from g_binocularsRaised (the case-26 input toggle @ 0x4e064c, forced
    // off when dead / spawn-gated / inputFlags&0x1E; no host binoculars input yet)].
    bool scope_raised = false;
    bool binoculars_raised = false;
    // The arms-dip feed (entity+0x371 byte / +0x36C head-look decay term): while the
    // dip window runs, head_look_decay drops 0x2800000 per tick before the eighth-step
    // ease, and the window decrements TWICE per tick (both witnessed sub-1 sites), so
    // the 20-tick weapon-switch stamp dips for 10 ticks. Seeds: 20 on a held-weapon
    // adm change [orig: @ 0x4b46f5], 80 by the remote-reload 0x49 path (D-NET-117).
    // [orig: @ 0x4b5cab..0x4b5ce7; consumed by the section-14.2 aim overlay]
    int32_t arms_dip_ticks = 0;
    int32_t head_look_decay = 0;
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

// The fire-path 3P attack stamp, keyed on the held weapon's attack kind (attack_anim):
// 1 -> 62 knife_attack, 2 -> 63 grenade_attack, anything else -> no body stamp (rifle
// fire plays only the FP clip on the weapon adm + the .3di control registers). Written
// IMMEDIATELY — it bypasses the selection commit's locked/emote defer.
// [orig: WeaponAction_Fire @ 0x542bbc..0x542bea — +0x2C8 = state, +0x2C4 = 0]
void infantry_weapon_attack_stamp(InfantryState &inf, int attack_kind);

// Stamp the witnessed 20-tick arms dip when this entity observes a different resolved
// held AnimMap identity. Serial 0 means no mounted weapon map. Keeping the observed
// serial on InfantryState makes the edge per entity rather than simulation-global.
void infantry_weapon_switch_stamp(InfantryState &inf, uint64_t anim_map_serial);

// Consumer gate for the secondary channel. Equal primary/secondary state ids do NOT
// disable composition: their playheads are independent. mount_blocks_channel is true
// for controller/gunner/driver seats; passenger seats retain the on-foot composition.
// [orig: Flags&0x100 + mount-class tests + PRIMARY state flag 0x40 @0x4b14a7]
bool infantry_weapon_channel_visible(const InfantryState &inf, bool weapon_in_hands,
                                     bool mount_blocks_channel);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_INFANTRY_H
