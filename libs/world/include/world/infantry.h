// Infantry motor: per-frame update for AI soldiers (org1) and the local player path
// that shares this port. IDA is the source of truth for the movement and animation
// state fields cited here.
#ifndef OPENNOVA_WORLD_INFANTRY_H
#define OPENNOVA_WORLD_INFANTRY_H

#include <cstdint>

#include "world/body_anim.h"
#include "world/entity.h" // EntityHandle (the combat-pass target/focus fields)

namespace opennova::world {

// 252 entries: 0..239 body states (180..239 = the 15-group bullet death matrix),
// 240..251 the wpn_* FP viewmodel states. [orig: AnimMap_FindSlotByName @0x40cfa0
// scans exactly 252 entries of g_animStateNameTable @0x8135F0]
inline constexpr int kInfantryAnimStateCount = 252;

// State id -> .adm key without the "anim_" prefix. [orig: g_animStateNameTable @0x8135F0]
extern const char *const kInfantryAnimNames[kInfantryAnimStateCount];

// Per-state behavior flags. [orig: g_animStateFlagsTable @0x8139E8]
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
    kRollLeft = 41,  // prone lean, MoveOrder bit 6 [orig: @0x4b7335]
    kRollRight = 42, // prone lean, MoveOrder bit 7 [orig: @0x4b734c]
    kIdle = 43,
    kIdle2 = 44,
    kIdleCrouch = 45,
    kIdleMortar = 46, // crouch idle for ForceCrouch (0x40000) weapons [orig: @0x4b726f]
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
    kPreAttack = 152,
    kOutOfGround = 153,
    kSwimAttack = 154,
    kAttack = 155,  // 155..158: the combat-reaction attack anims (world-wac-ai-re §17.3)
    kAttack2 = 156,
    kAttack3 = 157,
    kAttack4 = 158,
    kCoverIdle = 163,
    kCoverRun = 164,
    kCoverAttack = 165,
    kCoverAttack2 = 166,
    kRunAttack = 167,
    kRunAway = 168,
    kRun2Crouch = 169,
    kDeathFire = 173,
    kDeathPungi = 174,
    kDeathDrown = 175,
    kDeathGrenadeBase = 176, // 176..179: death_grenade F/R/B/L
    kDeathBulletBase = 180,  // 180..239: death_bullet, 15 bone groups x F/R/B/L
};
} // namespace anim_state

// Death causes routed by the death-anim selector. The original passes these as the
// selector's 4th argument; any other value falls through to death_pungi 174.
// [orig: Entity_ComputeAnimSlotIndex @0x43a690 switch]
namespace death_cause {
enum : int {
    kBullet = 1,    // -> 180 + quadrant + 4*bone_group
    kExplosive = 2, // -> 176 + quadrant (death_grenade_*)
    kFire = 3,      // -> 173 death_fire
    kGeneric = 4,   // -> 174 death_pungi (the no-cause fallback the death edge uses)
    kDrown = 5,     // -> 175 death_drown
};
} // namespace death_cause

// The death-anim selector: bone index 0..31 (>=32 -> 0), attack quadrant 0..3
// (>=4 -> 0), cause -> anim state id. Bullet deaths map the hit bone through the
// witnessed 32-entry bone->group table (15 groups: hip, torso, head, R/L shoulder,
// R/L arm, R/L hand, R/L thigh, R/L calf, R/L foot). [orig: Entity_ComputeAnimSlotIndex
// @0x43a690; the unused outPos arg dropped]
int compute_death_anim_state(int bone_index, int quadrant, int cause);

// The bullet-death attack quadrant: 0 forward / 1 right / 2 back / 3 left, from the
// victim's engine heading and the killing round's horizontal velocity.
// [orig: Entity_HandleDamageTrigger @0x407478 — (yaw - atan2BAM(vel.y, vel.x)
// - 0x60000000) >> 30; atan2 scale 683565275.5764316 = 2^32/2pi]
int death_quadrant_from_round(int32_t victim_heading_bam, float round_vel_x, float round_vel_y);

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
    // Advance a stable primary plus the current target and return their blended
    // output. The default composes already-quantized RootMotionFrames for test and
    // headless providers. Asset-backed providers may override this to blend raw
    // track floats before fixed conversion, as retail does.
    virtual bool advance_blended(int adm_id,
                                 int primary_state, int32_t &primary_phase_ticks,
                                 int target_state, int32_t &target_phase_ticks,
                                 float target_weight, RootMotionFrame &out);
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
    // The primary AnimMap keeps both playheads alive while it cross-fades state
    // changes. The target channel is anim_state/clip_phase; anim_prev owns this
    // independent old-channel phase. The float32 weight is accumulated by 0.1
    // normally, or 1/15 when the target state has flag 0x400.
    // [orig: AnimMap_UpdateEntity @0x40b5f0; AnimMap_InitFromParams @0x410640]
    int32_t anim_prev_clip_phase = 0;
    float anim_blend_weight = 1.0f;
    float anim_blend_step = 0.0f;

    bool body_blend_active() const { return anim_blend_weight < 1.0f; }

    void begin_body_transition(int target_state) {
        if (target_state == anim_state) {
            anim_pending = 0;
            return;
        }
        // Retargeting an in-flight A->B blend keeps primary A alive and replaces
        // only secondary B with C. Once a blend has completed, the playing target
        // becomes the next transition's primary.
        if (!body_blend_active()) {
            anim_prev = anim_state;
            anim_prev_clip_phase = clip_phase;
        }
        anim_state = target_state;
        anim_pending = 0;
        clip_phase = 0;
        anim_blend_weight = 0.0f;
        anim_blend_step =
                (infantry_anim_flags(target_state) & 0x400u) != 0
                        ? (1.0f / 15.0f)
                        : 0.1f;
    }

    void reset_body_animation(int state = opennova::world::anim_state::kIdle) {
        anim_state = state;
        anim_pending = 0;
        anim_prev = state;
        clip_phase = 0;
        anim_prev_clip_phase = 0;
        anim_blend_weight = 1.0f;
        anim_blend_step = 0.0f;
        last_events = 0;
        prev_capsule_bottom = 0;
    }
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
    // The arms-dip feed (entity+0x371 byte / +0x36C pitch-kick term): while the
    // dip window runs, pitch_kick_accum drops 0x2800000 per tick before the eighth-step
    // ease, and the window decrements TWICE per tick (both witnessed sub-1 sites), so
    // the 20-tick weapon-switch stamp dips for 10 ticks. Seeds: 20 on a held-weapon
    // adm change [orig: @ 0x4b46f5], 80 by the remote-reload 0x49 path (net-re §5.58).
    // [orig: @ 0x4b5cab..0x4b5ce7; consumed by the section-14.2 aim overlay]
    int32_t arms_dip_ticks = 0;
    int32_t pitch_kick_accum = 0;
    // Recoil and movement/weapon-weight dispersion accumulators. Both decay in
    // every person body tick. A successful ballistic/shotgun spawn adds the
    // stance-indexed ammo impulse to recoil_pitch after that shot's spread has
    // already been calculated; only the local player produces weight sway.
    // [orig: entity+0x380/+0x384; body updater + RoundData_SpawnRound @0x4EC0D0]
    int32_t recoil_pitch = 0;
    int32_t weapon_weight_spread = 0;
    // Host-fed Player_CanFireWeapon analogue used by the weight multiplier and
    // by HUD ERROR's second stance triplet. It is true only for a settled aimed
    // shot in a camera mode that permits it.
    bool aimed_shot_available = false;
    // Standing-idle tick counter (entity+0x148): the player-body idle starts at 43 and
    // promotes to 44 once >= 62 idle ticks; any movement resets it. [orig:
    // Entity_UpdateInfantryPlayerBody @0x4b727b-0x4b7293 (state = 0x2B + (cnt >= 0x3E)),
    // reset @0x4b719b]
    int32_t idle_counter = 0;
    // Lean inputs (MoveOrder bits 6/7 [orig: entity+0x12C 0x40/0x80]) and the smoothed
    // lean angle (entity+0xB0, BAM32). Every body tick decays lean -= (lean+8)>>4
    // [orig: @0x4b5c97]; the on-foot ramp adds -/+0x3000000 per held lean key, gated
    // alive + not prone [orig: @0x4b7dbf/@0x4b7dd6; Flags&0x100020 legs unmodeled].
    // Consumers: prone roll anims 41/42, the aim-overlay lean term, and the FP camera
    // roll = torsoRoll + lean/4 [orig: @0x437fcd].
    bool lean_left = false;
    bool lean_right = false;
    int32_t lean_angle = 0;
    // The torso roll (entity+0x2DC, BAM32): chases the entity's slope roll (+0x18)
    // a sixteenth-step per body tick with the LAG clamped to roll +-20 deg; prone
    // idle 48 decays it toward level; the combat rolls 41/42 RAMP it -/+5.625 deg
    // per tick (the FP barrel-roll view). Consumers: the FP camera roll
    // (torsoRoll + lean/4 [orig: @0x437fe6]) and the section-14 head/spine roll
    // terms. [orig: @0x4b5cff-0x4b5d6d + @0x4b700c-0x4b7025]
    int32_t torso_roll = 0;
    // The held weapon's run-gait class (weapon.def run_anim -> AdmDefs +0xAC) and its
    // ForceCrouch flag (weapon.def flags 0x40000), mirrored per tick like wpn_hold_kind.
    // run gait: forward-walk promotes to run_2/run_3 by 2 + run_anim [orig: @0x4b729d];
    // ForceCrouch: crouch idle promotes 45 -> 46 idle_mortar [orig: @0x4b723f] and
    // stance-change requests are refused [orig: @0x4e0d8a].
    int wpn_run_anim = 0;
    bool wpn_force_crouch = false;
    uint32_t last_events = 0;
    int32_t prev_capsule_bottom = 0;      // anim_slot[19]
    int32_t adm_id = 0;

    int32_t body_heading = 0;
    int32_t target_heading = 0;
    // Leg-chain chase yaws + their re-plant targets (BAM32): the feet keep pointing
    // where they were planted and shuffle after the heading under the witnessed
    // hysteresis; the render bone overlay consumes them as the R/L leg yaw. The two
    // motors differ in kind (D-INF-12 closure): the NPC's legs chase re-plant targets
    // seeded from the body/target midpoint and the body quarter-chases its target;
    // the PLAYER's legs chase the mouse yaw directly and body_heading is written as
    // the leg midpoint. [orig: entity +0x2d4/+0x2d8 (IDB legChaseYawR/L, renamed ex
    // the torsoYaw/torsoPitch misnomers) chasing +0x2e4/+0x2e8 (legReplantYawR/L);
    // org1 @0x4be8fd-0x4beb18, org2 @0x4b4945-0x4b4ac1; witness
    // docs/world/world-wac-ai-re.md s3.3 + s14]
    int32_t leg_yaw[2] = {};              // 0 = right chain, 1 = left chain
    int32_t leg_target[2] = {};
    int32_t vel[3] = {};                  // entity+152/+156/+160

    // Local-player stance input. NPC org1 selection does not consume this field.
    // [orig: entity+0x12C prone bit 0x100, crouch bit 0x200; player body @0x4b40e0]
    enum class Stance : uint8_t { kStand = 0, kCrouch = 1, kProne = 2 };
    Stance stance = Stance::kStand;
    // Standing on another entity (the +0x28 groundEntity link, written by the
    // ground probes [orig: Entity_RaycastGroundHeightAndObject @0x525fd0]).
    // Unwired until the platform slice lands — the footstep pass reads it for
    // the SS*FootOBJ slots and falls through to the terrain surface meanwhile
    // (the world sound D-entry).
    bool standing_on_entity = false;
    bool airborne = false;
    bool jump_requested = false;
    // The player body's jump cooldown/edge latch. The original REUSES entity+0x1A8
    // (org1's targetHeading slot) for this on the org2 body: clamp [0,32], >1 counts
    // down, held-at-1 until the jump key releases, jump only from 0; a jump reloads
    // 32. [orig: Entity_UpdateInfantryPlayerBody @0x4b7de0-0x4b7e15 + @0x4b7f06]
    int32_t jump_cooldown = 0;

    int32_t wait_cooldown = 0;            // entity[74]
    int32_t alert_timer = 0;              // entity[190]
    bool combat_reaction = false;         // entity+875
    int32_t ground_cache = 0;             // entity+676
    bool ground_cache_valid = false;
    int16_t max_health = 100;

    // ---- The infantry combat pass (org1 riflemen; world-wac-ai-re §17) ----
    // The 32-tick perception commit + the per-tick behavior/aim/fire state. Handles
    // stand in for the original entity pointers (container rebase).
    EntityHandle combat_target;       // AiSlot[3] mirror for the infantry pass [orig: slot+12]
    EntityHandle ai_focus;            // entity aiFocus (look/attention entity)
    EntityHandle last_attacker;       // entity lastAttacker (stamped by the damage pass,
                                      // consumed + cleared by each perception scan)
    EntityHandle aim_ref0;            // entity+0x2F0 — last fired-at target (accuracy settle)
    int32_t aim_point[3] = {};        // entity aimPoint (16.16, the led target point)
    int32_t damage_timer = 0;         // entity damageTimer (alert countdown, +12 on sight)
    bool was_hit = false;             // entity wasHit (consumed by the hit reactions)
    int32_t same_target_ticks = 0;    // entity+0x33C — scans-on-the-same-target counter
    int32_t combat_move_timer = 0;    // entity moveTimer (reaction hold / walking-fire cadence)
    bool fire_secondary_latch = false;// shouldFireSecondary [orig: the 0x8 event latch +
                                      // the walking-fire aim gate]
    int32_t aim_heading = 0;          // the aim solution (BAM; bearing + sawtooth error)
    int32_t aim_pitch = 0;            // (elevation + error)
    bool aim_valid = false;           // entity aimFlag
    int16_t magazine = 0;             // entity+0x35C word (reload at <=0, refill = clipsize)
};

// Pure retail body-tick kernels, exposed so deterministic tests can pin the
// wrap/arithmetic-shift behavior independently of locomotion.
// [orig: Entity_UpdateInfantryPlayerBody / Entity_UpdateInfantryAI]
void infantry_recoil_tick(InfantryState &inf, int32_t &heading,
                          int32_t &pitch, int32_t random16);

struct InfantryWeightSpreadInputs {
    bool produce = false;
    bool aimed_shot_available = false;
    bool prone = false;
    bool crouched = false;
    bool drowning = false;
    bool airborne_rising = false;
    int32_t weaponweight_fp16 = 0;
    int32_t clipweight_fp16 = 0;
};

void infantry_weapon_weight_spread_tick(
        InfantryState &inf, const InfantryWeightSpreadInputs &inputs);

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

// The upper-body hold-pose ladder: which secondary anim state a player body should be
// showing, given the held weapon's `special_hold` kind, that body's current PRIMARY
// state, and its scoped / binocular / reloading conditions.
//
// Every observer runs this for every player body it draws — the original applies no
// ownership test to it, so a remote player's pose is re-derived locally from the two
// bytes the wire already carries (the equipped ADM index at entity+0x2B0 and Flags bit
// 0x10) rather than replicated. Kept pure and free-standing because the two roles reach
// it differently: the authority runs it from the infantry motor's InfantryState, while a
// joiner has no motor entity for its peers at all and derives straight off the decoded
// row.
// [orig: Entity_UpdateInfantryPlayerBody @ 0x4b5dad..0x4b5e6f]
int infantry_weapon_hold_state(int hold_kind, int primary_anim_state, bool scope_raised,
                               bool binoculars_raised, bool reloading);

struct AiEntity;

// Snap a motor-driven infantry entity back to a deployed pose, standing and idle.
//
// The original has ONE Entity store, so its respawn writes Position/Yaw/Health once and
// the mover reads them back. Our AiEntity motor store is a tracked split of that store
// (see finish_infantry_tick's two-store reconciliation), and the motor is the WRITER of
// the pair: it mirrors AiEntity.pos/heading into the registry Entity every tick. A
// respawn that touches only the registry Entity is therefore silently reverted on the
// next tick, leaving the body at the corpse pose. Anything that redeploys a
// motor-simulated entity must reset BOTH stores, which is what this does for the motor
// half — velocities, the interpolation staging, the collide cache, and the InfantryState
// pose/stance/anim latches that would otherwise keep the death clip.
//
// [orig: the deploy flow places the entity, then Entity_ResetToSpawnState @ 0x4B9610
//  re-records Position and reseeds the body-anim channel; the death-family anim latch it
//  clears is @0x4b9714]
void infantry_respawn_snap(AiEntity &e, const int32_t pos[3], int32_t heading,
                           int16_t health);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_INFANTRY_H
