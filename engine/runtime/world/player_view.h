// The local player's view-side fixed-tick state: the ADS scope-camera ease and
// the third-person anchor chase, plus the optical projection of the weather FOV.
//
// The original runs these in its 62 Hz frame loop: the scope camera interp is a
// 15-step ease [orig: CNetPlayerInterp_Setup @ 0x4df36e; engaged mirror
// g_ScopeEngaged @ 0x82CE94], the chase camera's anchor eases a quarter-step
// per tick [orig: ThirdPersonCamera_Update @ 0x437c8d], and the scoped fov is
// 80 / zoom for sighted weapons, suppressed in third person
// [orig: Player_ToggleWeaponScope @ 0x4df401 / the g_CameraMode check
// @ 0x4df3fa; base fov g_CameraFovTargetQ16 @ 0x26C6848 default 0x500000 = 80 deg].
//
// Hosted, the simulation ticks camera lag and the ADS pose at world cadence.
// The optical render query can rewrite the shared FOV target, as in retail;
// the weather current advances only on simulation ticks. All policy remains
// in the engine; the host samples input and applies node transforms.

#pragma once

#include <cstdint>

#include <runtime/renderer/frame_fx_effects.h>
#include <runtime/world/death_camera.h>
#include <runtime/world/radar_contacts.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;

// The per-toggle ease lengths [orig: CNetPlayerInterp_Setup call sites in
// Player_ToggleWeaponScope — engage 15 @ 0x4df36e / 7 for Inset (flags2 0x200)
// weapons @ 0x4df355; disengage mirrors them @ 0x4df201 / @ 0x4df1e8, and the
// hipfire-return leg is a single step @ 0x4df1c3].
constexpr int32_t kScopeEaseSteps = 15;
constexpr int32_t kScopeEaseStepsInset = 7;
constexpr int32_t kScopeEaseStepsHipfire = 1;
// [orig: g_CameraFovTargetQ16 @ 0x26C6848 default 0x500000 = 80.0 horizontal degrees]
constexpr float kPlayerCameraFovHDeg = 80.0f;
// [orig: binocular camera fov constant in Player_UpdateFirstPersonCamera]
constexpr float kBinocularCameraFovHDeg = 20.0f;
// The fixed radius of the one random aim displacement a binocular raise seeds
// (2.8125 deg = 0x02000000 BAM32). The displacement survives movement/death/
// third-person suppression only until a rendered frame observes binoculars
// down; the next active render seeds again. Input action 26 only toggles the
// request. [orig: Render_ProcessMainSceneFrame @0x5ca3e1..0x5ca4b0]
constexpr float kBinocularAimOffsetDeg = 2.8125f;
// [orig: the five NVG gain positions selected by actions 56/57]
constexpr int32_t kNvgGainMin = 0;
constexpr int32_t kNvgGainMax = 4;
// [orig: the chase anchor ease @ 0x437c8d — one quarter per 62 Hz tick]
constexpr float kTpAnchorEase = 0.25f;
// The first-person eye pull-back along the full view rotation: -0x3000 on the
// view-frame FORWARD axis [orig: Math_FixedPointTransformPoint22 of
// (-0x3000, 0, 0) added onto g_ViewPosX/Y @ 0x438001..0x438031].
constexpr float kFpEyePullback = 0.1875f;
// The CameraOffset floor and the non-person eye bump.
// [orig: the 0x2000 floor @ 0x4b6b98; the +0x10000 bump @ 0x437e8f]
constexpr float kEyeMinAbovePosition = 0.125f;
constexpr float kNonPersonEyeBump = 1.0f;
// The head-bone eye's terrain floor (the D-INF-18 residual, ported): the probe
// offset (0x4000 = 0.25 u) and the per-sample clearance (0x1000 = 0.0625 u).
// [orig: Entity_UpdateInfantryPlayerBody @ 0x4b6c23 / @ 0x4b6c2d]
constexpr float kEyeTerrainProbeRadius = 0.25f;
constexpr float kEyeTerrainClearance = 0.0625f;
// The head bone the person eye reads (".bad row BN15", model bone table
// index 14) and the aim-projection ray length (65536000 q16 = 1000 units).
constexpr int kHeadBoneIndex = 14;
constexpr float kAimProjectRange = 1000.0f;
// The chase camera's in-play numbers: the ROUND-START reset (distance 1.0,
// orbit zeroed — the tight over-the-shoulder view) and the pivot nudge/march.
// [orig: Camera_ResetToLocalPlayer @ 0x4a3d30 (distance 0x10000 @ 0x4a3d4c,
//  orbit zeroed @ 0x4a3d56/5b); nudge R*(0x2000,0x2000,0x2000) @ 0x43818a;
//  0.25u march steps @ 0x438243, gate @ 0x4381e9]
constexpr float kTpDistance = 1.0f;
constexpr float kTpOrbitPitchDeg = 0.0f;
constexpr float kTpPivotNudge = 0.125f;
constexpr float kTpMarchStep = 0.25f;
constexpr float kTpMarchGate = 8.0f;
// weapon.def `pos`/`tpos` file unit -> world units [orig: the parser stores
// atof(str) * 256 (flt_7D1D70 @ 0x544770) and the camera ftol's it onto the
// 16.16 view position — the net world offset is file_value / 256].
constexpr float kWeaponDefPosScale = 256.0f;

// The FP viewmodel motion lead: a per-render-frame damped tracker of the
// tracked entity's per-tick movement delta (<<8), whose output >> 7 — clamped
// ±1024 on x/y and ±4096 on z (16.16) — is added component-wise onto the
// view-LOCAL camera offset BEFORE the view rotation (the witnessed
// pre-rotation add takes the world-delta components raw; no frame
// conversion). Steady velocity decays the lead toward zero — it responds to
// speed changes, not speed. [orig: the tracker
// vel += (prev_sample - new_sample - vel + 16) >> 5 @ 0x437bac..0x437c0e in
// ThirdPersonCamera_Update; the >> 7 + clamps @ 0x4dd4f2..0x4dd54f in
// Player_UpdateFirstPersonCamera]
struct PlayerViewMotionLead {
    int32_t vel[3] = {0, 0, 0};
    int32_t prev_sample[3] = {0, 0, 0};
};
constexpr int32_t kFpLeadClampXy = 1024;
constexpr int32_t kFpLeadClampZ = 4096;
// Advances the tracker one render frame from the entity's current per-tick
// movement delta (world units) and returns the clamped lead (16.16, the same
// value order the retail camera adds).
void player_view_motion_lead_update(PlayerViewMotionLead &lead,
                                    const float tick_delta_units[3],
                                    int32_t out_lead_q16[3]);

// The 4:3 framing compensation: with the viewport aspect at 4:3 or narrower
// (3*width <= 4*height), the FP camera offset drops 0x500 (0.0195 u) on the
// view-local z. Widescreen never takes it. [orig: the 3*dword_A78394 <=
// 4*dword_A78398 gate @ 0x4dd571 -> cam_offset_z -= 0x500 @ 0x4dd578; the
// same viewport block HUD_DrawEntityLabel projects with @ 0x5a3b6a]
constexpr int32_t kFpNarrowAspectDropQ16 = 0x500;
inline bool player_view_narrow_aspect(int viewport_w, int viewport_h) {
    return 3 * viewport_w <= 4 * viewport_h;
}

// The MOUNTED camera's inputs, resolved by the hosting simulation from the
// local player's carrier each tick: a control seat (mount state +0x168 == 2
// or 5 — Entity::is_vehicle_control_seat()) with the carrier's position,
// heading, look-ahead target and bound radius, its air class
// (itemDef+0x196 in {3,4}), and the water plane the clearances read.
// `control_seat` false = on foot or a passenger/gunner seat, which keeps the
// on-foot chase.
// [orig: the +0x168/+0x16C reads in ThirdPersonCamera_Update @0x437B1F and
//  Camera_ComputeThirdPersonView @0x438100/@0x43845A/@0x43861D]
struct MountedCameraInput {
    bool control_seat = false;
    int32_t carrier_pos_q16[3] = {0, 0, 0}; // mission space, 16.16
    int32_t carrier_yaw_bam = 0;            // BAM32 heading
    // The look-ahead target (16.16): the carrier's orientation matrix times
    // (6.0, 0, 0), rotation only, so it follows the hull's pitch as well as
    // its heading [orig: Math_TransformPointFixedPoint22 @0x412E90 of
    //  parentMatrix(+0xB4) x (0x60000, 0, 0), called @0x438855].
    int32_t lookahead_target_q16[3] = {0, 0, 0};
    float bound_radius = 0.0f;              // carrier +0, mission units
    bool aircraft = false;                  // unit_type 3/4 [orig: @0x43861D]
    // The water plane every chase eye clears, on foot as well as mounted
    // [orig: g_EnvWaterHeightFixed + 0x4000 @0x438409..0x43841E].
    float water_z = 0.0f;                   // g_EnvWaterHeightFixed, units
};

// THE FIRST-PERSON CAMERA SHAKE. A single counter drives an angular jitter on
// the FP view rotation; there is no positional component and no flinch.
//
// The counter is stored 0..255 and armed from three sites: a HEALTH DROP adds
// 10 [orig: @0x4305c1 — the `newHealth < Health` arm, capped @0x4305cb], an
// explosive near-miss adds 20 [orig: @0x4AF837 — gated on the ammo def's +46
// word and armed BEFORE the damage gate, so a near-miss that deals no damage
// still shakes], and a quake HARD-SETS 32 [orig: @0x57EB7D]. It decays by 2
// per tick with a floor rather than a clamp — at or below 1 it snaps to 0
// [orig: @0x4DE590], so an odd count cannot idle at 1 forever. A respawn
// zeroes the counter and NOTHING else [orig: @0x4B10D8]: the IIR
// accumulators deliberately survive, so the first tick after a respawn
// resumes from the previous filter state.
inline constexpr int kShakeArmHealthDrop = 10;
inline constexpr int kShakeArmNearMiss = 20;
inline constexpr int kShakeQuakeLevel = 32;
inline constexpr int kShakeDecayPerTick = 2;
inline constexpr int kShakeStoreMax = 255;
// Sampling clamps to 64 even though the STORE cap is 255, so the arms above
// buy DURATION past 64, never amplitude [orig: the > 64 test @0x43804b..0x438050].
inline constexpr int kShakeSampleMax = 64;

// The three one-pole IIR accumulators, in BAM32. They persist across ticks
// and across respawns.
struct CameraShakeState {
	int32_t counter = 0;  // the armed/decaying level
	int32_t roll = 0;     // dword_A89158
	int32_t pitch = 0;    // dword_A89154
	int32_t yaw = 0;      // dword_A89150
};

// Arm the counter, saturating at the 255 STORE cap.
inline void camera_shake_arm(CameraShakeState &st, int amount) {
	st.counter += amount;
	if (st.counter > kShakeStoreMax) st.counter = kShakeStoreMax;
}

// One tick of decay [orig: @0x4DE590]: at or below 1 the counter snaps to 0,
// otherwise it drops by 2.
inline void camera_shake_decay(CameraShakeState &st) {
	if (st.counter <= 1) st.counter = 0;
	else st.counter -= kShakeDecayPerTick;
}

// THE THREE FULLSCREEN DAMAGE-FEEDBACK WORDS. Retail keeps them beside the
// shake counter, decays all four in the SAME instruction run, and draws them
// as three viewport-filling quads at the very end of the scene frame.
//
//   red    dword_B764B4  the damage vignette (vignette.tga tinted 0xFF0000)
//   white  dword_B764B8  the hit flash (untextured white)
//   revive revive tint   dword_B764BC, the medic blue-white tint
//
// ARMS
//   red    += 120 cap 255 in Player_OnDamageReceived [orig: @0x4dd88f]; the
//          joiner's own 0x0A tail health DROP adds the same 0x78 = 120 inline
//          [orig: NapiNPClientMsg_0x00A @0x4305a3, cap @0x4305bb].
//   white  = 255 from BOTH legs of the local-player hit blackout
//          [orig: Entity_ApplyCollisionForce @0x4af729 / @0x4af769]; raised to
//          a FLOOR of 128 when the damaging ammo's kz_physics byte (+0xE1) is 3
//          [orig: Entity_OnDamageReceived @0x4af828..0x4af82a].
//   revive = 255 on the "a medic is reviving me" message
//          [orig: NapiNPClientMsg_0x03A @0x422685].
//
// DECAY, once per client frame, in retail's order (white, red, revive) right
// after the shake [orig: Player_UpdatePerFrame @0x4DE5A7..0x4DE5F7]. The revive
// leg is the odd one: it steps by ONE and FLOORS at 0xC4, so it slides 255 ->
// 196 over 59 ticks and then HOLDS at 196 until a clear. It also has no `else`
// branch, so a word already at 0 or 1 is left exactly where it is.
//
// CLEAR: all three are zeroed by the local respawn / mission start
// [orig: Game_InitNewRound @0x422778 / @0x422784 / @0x422790].
//
// DRAW [orig: Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48, only while
// !g_DeathScreenActive, after the HUD overlay pass and before the sun veil]:
//   1. white:  colour (white << 24) | 0xFFFFFF, quad mode 2
//   2. red:    only while g_CameraMode != 3, alpha = min(red, 0xC0),
//              colour (alpha << 24) | 0xFF0000, quad mode 3 = the vignette.tga
//              material (flags 593 = 0x251 AFUNC_BLEND | ASRC_TEXTURExITERATED
//              | COLOR_ITERATED: texture alpha x vertex alpha, vertex colour)
//   3. revive: colour 0xFFFFFFFF - ((revive >> 1) * 0x10100), quad mode 0
//              (A 255, R = G = 255 - (revive >> 1), B 255)
//
// While the white word is non-zero the ENTIRE HUD overlay pass early-returns,
// so a collision/explosion flash blanks the HUD for up to 64 ticks
// [orig: HUD_RenderAllOverlays @0x5a8098..0x5a809f].
inline constexpr int kScreenFlashMax = 255;
// Player_OnDamageReceived's red add, and the identical 0x78 the 0x0A tail
// health-drop detector adds [orig: @0x4dd88f / @0x4305a9].
inline constexpr int kScreenFlashRedArm = 120;
// Player_OnDamageReceived's shake add [orig: @0x4dd8a6].
inline constexpr int kShakeArmDamageReceived = 10;
// The red quad's alpha ceiling at draw time [orig: @0x5cabe7..0x5cabee].
inline constexpr int kScreenFlashRedDrawCap = 0xC0;
// The explosive-ammo floor [orig: @0x4af82a].
inline constexpr int kScreenFlashWhiteExplosiveFloor = 128;
// The revive tint's decay floor [orig: @0x4de5e1..0x4de5ed].
inline constexpr int kScreenFlashReviveFloor = 0xC4;
inline constexpr int kScreenFlashWhiteDecayPerTick = 4;
inline constexpr int kScreenFlashRedDecayPerTick = 2;
// The camera mode that suppresses the red vignette [orig: @0x5cabde].
inline constexpr int kScreenFlashRedSuppressedCameraMode = 3;

struct ScreenFlashState {
	int32_t red = 0;    // dword_B764B4
	int32_t white = 0;  // dword_B764B8
	int32_t revive = 0; // dword_B764BC
	// Our ClientState carries the medic-revive message as a LATCH rather than
	// as the raw 0x3A edge retail arms on, so the arm rides the latch's rising
	// edge; this bool is that edge detector and has no retail counterpart.
	bool revive_latched = false;
};

// red += amount, saturating at 255 [orig: @0x4dd88f..0x4dd896 / @0x4305a9..0x4305bb].
inline void screen_flash_add_red(ScreenFlashState &st, int amount) {
	st.red += amount;
	if (st.red > kScreenFlashMax) st.red = kScreenFlashMax;
}

// The local-player hit blackout's hard set [orig: @0x4af729 / @0x4af769].
inline void screen_flash_arm_white_hit(ScreenFlashState &st) {
	st.white = kScreenFlashMax;
}

// The explosive-ammo FLOOR, not a set [orig: @0x4af828..0x4af82a].
inline void screen_flash_arm_white_explosive(ScreenFlashState &st) {
	if (st.white < kScreenFlashWhiteExplosiveFloor)
		st.white = kScreenFlashWhiteExplosiveFloor;
}

// The medic-revive arm [orig: @0x422685].
inline void screen_flash_arm_revive(ScreenFlashState &st) {
	st.revive = kScreenFlashMax;
}

// The being-revived latch folded into the arm above: our replica state exposes
// the retained +0x1E0 word, so the 0x3A message edge is its 0 -> 1 transition.
inline void screen_flash_track_revive(ScreenFlashState &st, bool reviving) {
	if (!reviving) {
		st.revive_latched = false;
		return;
	}
	if (!st.revive_latched) {
		st.revive_latched = true;
		screen_flash_arm_revive(st);
	}
}

// One client frame of decay, in retail's instruction order
// [orig: Player_UpdatePerFrame @0x4DE5A7..0x4DE5F7].
inline void screen_flash_decay(ScreenFlashState &st) {
	if (st.white > 3) st.white -= kScreenFlashWhiteDecayPerTick; // [orig: @0x4de5a7]
	else st.white = 0;
	if (st.red > 1) st.red -= kScreenFlashRedDecayPerTick;       // [orig: @0x4de5bf]
	else st.red = 0;
	if (st.revive > 1) {                                          // [orig: @0x4de5d6]
		st.revive -= 1;
		if (st.revive < kScreenFlashReviveFloor) st.revive = kScreenFlashReviveFloor;
	}
	// No else: retail leaves a word already at 0 or 1 exactly where it is.
}

// [orig: Game_InitNewRound @0x422778 / @0x422784 / @0x422790]
inline void screen_flash_clear(ScreenFlashState &st) {
	st.red = 0;
	st.white = 0;
	st.revive = 0;
	st.revive_latched = false;
}

// The joiner's 0x0A tail-health DECREASE detector, the one arm that is not
// Player_OnDamageReceived: the handler compares the wire's signed 16-bit health
// against the recipient's stored Health and, only when it DROPPED, adds 120 to
// the red vignette and 10 to the camera shake, both capped at 255 -- and adds
// NO radar blip, which is what separates it from the body motor's arm.
// [orig: NapiNPClientMsg_0x00A @0x43059a `cmp dx,[eax+0x11E]` / `jge` @0x4305a1
//  -> @0x4305a3..0x4305d4; the Health store follows @0x4305df]
inline void screen_flash_arm_health_drop(ScreenFlashState &flash, CameraShakeState &shake,
                                         int new_health, int stored_health) {
	if (new_health >= stored_health) return;
	screen_flash_add_red(flash, kScreenFlashRedArm);
	camera_shake_arm(shake, kShakeArmHealthDrop);
}

// The whole HUD overlay pass early-returns while the white word burns
// [orig: HUD_RenderAllOverlays @0x5a8098..0x5a809f].
inline bool screen_flash_hud_overlays_suppressed(const ScreenFlashState &st) {
	return st.white != 0;
}

// The red quad's DRAW alpha: nothing in camera mode 3, otherwise the word
// capped at 0xC0 [orig: @0x5cabd5..0x5cabf3].
inline int32_t screen_flash_red_draw_alpha(const ScreenFlashState &st, int camera_mode) {
	if (st.red == 0 || camera_mode == kScreenFlashRedSuppressedCameraMode) return 0;
	return st.red > kScreenFlashRedDrawCap ? kScreenFlashRedDrawCap : st.red;
}

// The revive quad's red/green channel byte; blue stays 255 and alpha 255
// [orig: 0xFFFFFFFF - ((v >> 1) * 0x10100) @0x5cac18..0x5cac43].
inline int32_t screen_flash_revive_channel(const ScreenFlashState &st) {
	return kScreenFlashMax - (st.revive >> 1);
}

// THE LOCAL PLAYER'S DAMAGE FEEDBACK, one retail function
// [orig: Player_OnDamageReceived @0x4DD880]: the red vignette gains 120 and the
// camera shake 10, both capped at 255, then a radar damage blip is added at
// `pos_q16` (mission 16.16) for `source` — kind 255 for self damage, else 2
// when the source's +0x170 entity is class 6, else 0 (radar_damage_kind) —
// and two per-player-slot words are stamped. Every one of retail's five call
// sites is gated on the victim being the local player; this function is the
// arm they share. A world with no bound local player state is a no-op.
//
// UNPORTED here, deliberately: the unk_26C77A0[100 * (shadowSlot1 & 0x7FFF)]
// words +11 = 6 / +12 = 10 [orig: @0x4dd907..0x4dd916], whose consumers are
// unwitnessed.
void player_on_damage_received(World &world, const RadarSource &source,
                               const int32_t pos_q16[3]);

// Retail's six-lane first-person pose: three float Q16 position values and
// three wrapping BAM words, not six interchangeable scalar angles.
// [orig: WeaponDef +0x10C / +0x124; CNetPlayerInterp_Setup @0x4DDFD0]
struct PlayerViewPose {
    float position_q16[3] = {};
    uint32_t rotation_bam[3] = {};
};

struct PlayerViewBiasInterp {
    PlayerViewPose current;
    PlayerViewPose target;
    PlayerViewPose velocity;
    uint32_t remaining = 0;
    bool active = false;
    int32_t position_bias_q16[3] = {};
    int32_t rotation_bias_bam[3] = {};
};

void player_view_bias_interp_setup(PlayerViewBiasInterp &interp, uint32_t steps,
                                  const PlayerViewPose &idle_source,
                                  const PlayerViewPose &target);
void player_view_bias_interp_step(PlayerViewBiasInterp &interp, const PlayerViewPose &hip);

struct PlayerViewState {
    // THE ADS SCOPE TRI-STATE beside the FP camera interp below.
    // `scope_engaged` is the TARGET the promoter reads,
    // `scope_settled` the PROMOTED "scoped" byte every CanFire/crosshair/card
    // consumer keys on, `scope_hipfire` the latch every interp Setup stores
    // beside its target: the toggle and the PackInput legs target the hip
    // exactly when they store 1 and tpos when they store 0, so the latch IS
    // the interp's target pose [orig: g_ScopeEngaged @0x82CE94;
    // g_WeaponScopeActive @0xB76478; g_ScopeHipfire @0x82CE98, init/reset 1;
    // the paired Setup/latch stores @0x4df1c3/@0x4df212, @0x4df36e/@0x4df373,
    // @0x4df567/@0x4df56c, @0x4df5d1/@0x4df5d6, @0x4df636/@0x4df63b].
    bool scope_engaged = false;
    bool scope_settled = false;
    bool scope_hipfire = true;
    // THE FP CAMERA INTERP ITSELF [orig: g_FpCameraInterp @0x82CE40 -- the
    // per-step velocity +4..24, the current pose +28..48, the target +52..72,
    // the counter +0, entitySlotPtr +76, activeFlag +80]: the two authored
    // endpoints (WeaponDef +0x10C hip copy / +0x124 tpos) and the six-lane
    // interpolator every Setup targets. Its `active` latch IS the ease gate
    // the toggle tests, its completion IS what the settle promoter waits
    // for, and its published biases ARE the camera's position/rotation
    // offsets; there is no separate scalar clock.
    PlayerViewPose weapon_hip_pose;
    PlayerViewPose weapon_ads_pose;
    PlayerViewBiasInterp weapon_pose_interp;
    bool weapon_pose_bound = false; // fpCameraInterp.entitySlotPtr + its non-null Def
    // The movement-held latch: written by the input pack only, from the packed
    // word's direction bits, so a joiner's latches once per send boundary.
    // [orig: g_MovementKeyHeld @ 0xB7653B; Player_PackInputStateToEntity
    //  @0x4df4bb / @0x4df4f9]
    bool move_held = false;
    // The frame's input word holds a direction bit: what the binocular
    // suppression reads, every frame, ahead of the pack. [orig:
    //  Player_UpdatePerFrame `test byte ptr g_InputFlags, 1Eh` @0x4de3ae]
    bool movement_input = false;
    // THE CAMERA MODE, two words. `third_person_selected` is the user's
    // preference — the chase byte the view actions write, 1 from the session
    // reset on [orig: g_CameraThirdPersonSelected @ 0xA860DF — set to 1 by
    // Client_ResetGameSessionState @ 0x42ca3c; written by
    // Input_HandleActionBinding cases 400 @ 0x49c084 (0), 401 @ 0x49c0ea (0),
    // 402 @ 0x49c100 (1), 412 @ 0x49c0ad/@ 0x49c0c8 (the cycle)].
    // `third_person` is the RESOLVED mode the per-frame arbiter derives from
    // that preference and the seat — player_view_resolve_mode
    // [orig: g_CameraMode @ 0xA890C8].
    bool third_person_selected = true;
    bool third_person = false;
    // THE RESOLVED MODE WORD itself (g_CameraMode): 0 first person, 1 the
    // chase, 3 the spectator/overhead (unmodelled), 4 the death lerp camera.
    int camera_mode = 0;
    // The arbiter's remaining inputs [orig: Render_ProcessMainSceneFrame
    // @0x5ca1f4..0x5ca24b]: the client-local death screen and its sub-mode
    // (dword_A860F0: 0 / 1 / 2 = kill-cam; the sub-mode writers are the
    // spectate actions, unported), the local dead bit (`Flags & 2`), the
    // end-of-round gate (g_SpawnSuccessGate) with the on-foot test
    // (parentEntity == 0), and the two g_RulesFlags bits (bit 0 = no death
    // camera, bit 0x40 = server force-first-person while in session — both
    // admin `set` commands, no wire fold yet).
    bool death_screen_active = false;
    int death_screen_submode = 0;
    bool local_dead = false;
    bool round_ended = false;
    // The client's decided round winner (g_EndRoundWinnerTeam, the S2C
    // 0x1D header's winner byte); nonzero hides the FP viewmodel [orig:
    // Player_RenderViewModelIfAlive @0x4E014B].
    int32_t end_round_winner_team = 0;
    bool on_foot = true;
    bool in_session = false;
    bool rules_no_death_cam = false;
    bool rules_force_first_person = false;
    // The mode-4 lerp camera (world/death_camera.h): computed on the
    // transition into mode 4, viewed every frame against `view_tick`.
    DeathCameraState death_cam;
    uint32_t view_tick = 0;
    // The on-foot third person retail never resolves (stock 1.7.5.7 has no
    // on-foot chase, net-re §5.39): the debug affordance the onhook camera
    // patch provides, exposed on the debug menu and never on a gameplay key.
    bool debug_third_person_on_foot = false;
    // The first-person camera shake (CameraShakeState below): the pre-tick
    // input pass decays its counter, the quake tick HARD-SETS it [orig:
    // dword_B764B0 = 32 @ 0x57eb7d / @ 0x57ec29], and every
    // Camera_ComputeThirdPersonView call samples it — advancing the IIR
    // filters — once per quantum, once per rendered frame and once more for
    // the Inset scene [orig: the callers @ 0x526781, @ 0x5ca34d and
    //  @ 0x5c9841]. Only local_player_camera_compose advances it; an observed
    // frame reads the view the last compose left.
    CameraShakeState shake;
    // The three fullscreen damage-feedback words (ScreenFlashState above):
    // armed by the damage/collision/revive legs, decayed beside the shake in
    // the same pre-tick pass, cleared by the local respawn.
    ScreenFlashState flash;
    bool binoculars_requested = false;   // [orig: raw toggle g_BinocularsToggle @ 0xB76539]
    bool binoculars_raised = false;      // [orig: body-pose g_BinocularsRaised @ 0xB7653A]
    bool binoculars_view_active = false; // [orig: first-person view g_BinocularsViewActive @ 0xB76538]
    bool nvg_active = false;
    int32_t nvg_gain = kNvgGainMin;
    bool tp_anchor_valid = false;
    float tp_anchor[3] = {0.0f, 0.0f, 0.0f}; // mission space (Z-up)
    // The mounted anchor's exact 16.16 carrier: the mounted ease integrates
    // it (>> 4 on x/y, >> 5 on z, half-step rounded) and `tp_anchor` mirrors
    // it; the on-foot float ease keeps it in step for a seamless mount.
    int32_t tp_anchor_q16[3] = {0, 0, 0};
    // The mounted look-ahead offset (16.16), eased a thirty-second toward the
    // carrier's matrix x 6.0 by EVERY compose of the mounted chase — each
    // logic quantum's and each rendered frame's, so the ease runs faster at a
    // higher frame rate exactly as retail's does; the look-at point is the
    // pivot plus this [orig: g_camera_lookahead += (target - lookahead + 16)
    // >> 5 per axis @0x43885A..0x4388AF, inside Camera_ComputeThirdPersonView
    // @0x437D10 (called @0x526781 and @0x5CA34D)].
    int32_t lookahead_q16[3] = {0, 0, 0};
    // The ground-entity leg's lift: the longest CameraOffset seen while the
    // person stands or sits on a crashed or settled vehicle; every person-leg
    // compose clears it [orig: dword_A89140 — the max @0x437F03..0x437F0B,
    //  cleared @0x437F9C].
    int32_t ground_leg_lift_q16 = 0;
    MountedCameraInput mount;
};

// THE MODE ARBITER, run every tick ahead of the anchor chase (retail: every
// rendered frame) [orig: Render_ProcessMainSceneFrame @ 0x5ca1d2..0x5ca262]:
//   desired = 0; the chase preference AND a control seat (parentSlot 2 or 5
//   — `mount.control_seat`) -> 1 [@0x5ca1da..0x5ca1f2];
//   death screen up: sub-mode 0 -> 0, 1 -> 1, 2 -> 0 (the kill-cam retarget
//   lives in Camera_SetTrackedEntity), anything else keeps desired
//   [@0x5ca1f4..0x5ca215];
//   else the local dead bit (`Flags & 2`), or the end-of-round gate with the
//   player on foot (parentEntity == 0) -> 4 unless g_RulesFlags bit 0
//   [@0x5ca217..0x5ca24b];
//   else in session with g_RulesFlags bit 0x40 -> 0 [@0x5ca22d..0x5ca23e].
// The mode is then applied only when it changed, so the orbit/distance state
// carries across the flip [the changed test @ 0x5ca258 -> Camera_SetTrackedEntity
// @ 0x5ca262 — mode 4 computes the lerp camera there @0x439257]. Boarding and
// dismounting never touch the camera — Entity_ProcessVehicleAttach @ 0x435aa0
// only moves the seat plus the stance latches and the look yaw, and
// Entity_DetachFromVehicle @ 0x4355f0 zeroes parentSlot @ 0x435921 — the next
// frame's arbiter does the rest: a driver arrives in the chase, a gunner or
// passenger stays first person, a dismount returns to first person. The debug
// on-foot override ORs in. RESIDUAL: mode 3 (the spectator camera) and the
// death-screen sub-mode writers (the spectate actions) are not modelled; the
// two g_RulesFlags bits are carried as inputs with no wire fold.
void player_view_resolve_mode(PlayerViewState &v);

// The view actions' preference writes: `view1st` (400) and `viewwithgun` (401)
// select first person, `viewchase` (402) the chase; the mode re-resolves at
// once. The FP-gun bit 400/401 also write (g_FpWeaponViewFlags bit 0) belongs
// to the HUD flag owner. [orig: Input_HandleActionBinding @ 0x49c084 /
//  @ 0x49c0ea / @ 0x49c100]
void player_view_set_third_person_selected(PlayerViewState &v, bool selected);

// The three view-action ids the binding table's view rows fire.
inline constexpr int kViewActionFirstPerson = 400; // view1st, F2
inline constexpr int kViewActionWithGun = 401;     // viewwithgun, F3
inline constexpr int kViewActionChase = 402;       // viewchase, F4

// A view-action row fired: its chase preference (as above) and its own BMS
// input-action bit ORed into `input_action_bits`, the live word the cat-7
// player triggers read (view1st -> PlayerFirstPerson 0x4000000, viewwithgun
// -> PlayerCockpitView 0x10000000, viewchase -> PlayerThirdPerson
// 0x8000000). Pass nullptr off the authority, which never evaluates the
// .bms. Any other action changes nothing: the 412 cycle and the 405-410
// orbit/zoom cases have no binding-table row, so no key reaches them.
// [orig: Input_HandleActionBinding case 400 @0x49c073 (the bit @0x49c07a),
//  case 401 @0x49c0d9 (the bit @0x49c0e0), case 402 @0x49c0f6]
void player_view_apply_view_action(PlayerViewState &v, uint32_t *input_action_bits,
                                   int action);

// One 62.5 Hz tick: resolve the camera mode, step the six-lane scope-camera
// interp toward its target and promote the settled byte on the tick its
// active latch drops [orig: Player_UpdatePerFrame -- the step runs only
// behind an active interp @0x4de4c7 -> Player_StepFpViewBiasInterp @0x4de4c9,
// then `if (!activeFlag)` @0x4de4d9 -> g_WeaponScopeActive = (g_ScopeEngaged
// != 0) @0x4de4f7; the stepper reports done on the call AFTER the last
// moving lane snapped, so a 15-step authored ease promotes on tick 16, and
// an unbound slot (null entitySlotPtr/Def) deactivates on its first step
// @0x4DDD2B..0x4DDDBC and promotes at once], and chase the
// third-person anchor toward `eye` (mission space). Entering third person
// seeds the anchor at the eye [orig: Camera_SetTrackedEntity @ 0x4391d0
// resets the track on change]; leaving invalidates it. In a control seat the
// anchor chases the carrier position lifted max(1.0, 0.375 r) instead, a
// sixteenth per tick horizontally and a thirty-second vertically in 16.16
// [orig: ThirdPersonCamera_Update — the lift @0x437B1F..0x437B4B, the ease
// @0x437C56..0x437C79].
void player_view_tick(PlayerViewState &v, const float eye[3]);

// Whether the scope-camera interp is mid-ease: its active latch, which holds
// one call past the last lane snap. Every scope toggle is REFUSED while it
// runs [orig: the !g_FpCameraInterp.activeFlag gate @ 0x4df177].
bool player_view_scope_ease_active(const PlayerViewState &v);

// The PROMOTED scope byte [orig: g_WeaponScopeActive @0xB76478]: set only by
// the settle promoter (= the engaged target when the interp lands @0x4de4f7)
// and by the mount stamp, cleared by both toggle branches (@0x4df20c /
// @0x4df31d) and by the auto-re-raise (@0x4df609). It holds through the
// settled drop-with-memory ease (@0x4df5ae) and NOT through a raise, so it is
// no function of engaged + active. The camera reset clears only the target;
// a same-category mount retains this byte, and ForceScoped stamps it itself.
// [orig: reset @0x4DE275; mount @0x4DFB31..0x4DFB66]
inline bool player_view_scope_settled(const PlayerViewState &v) {
    return v.scope_settled;
}

// Discard the scope state when no held weapon/round remains. A weapon mount
// and the category-key camera reset have separate, narrower writes below.
void player_view_scope_reset(PlayerViewState &v);
// Category-handle/UseGun reset: retain the promoted byte, angle velocities,
// angle targets, counter and published biases. Clear the position lanes and
// copy angle velocity into current, then deactivate the interpolator.
// [orig: Player_ResetCameraAndMovementState @0x4DE1F0..0x4DE281]
void player_view_weapon_switch_reset(PlayerViewState &v);
// A direct mount clears only the published biases. Same-category mounts keep
// the promoted byte; ForceScoped sets it; an optical promoted mount sets up a
// one-step transition to the new def's ADS pose without stepping it here.
// [orig: Player_MountWeaponSlot @0x4DFB31..0x4DFC9A]
void player_view_weapon_mount(PlayerViewState &v, int32_t flags, bool category_changed);

// Whether a scope request for `engaged` has anything to do: false when that
// target is already reached or in flight. The one exception is the engaged
// target latched at an IDLE hip without the promoted byte (a raise reversed on
// its first frame -- the zero-delta re-target deactivates the interp outright
// @0x4de0fe..0x4de11b and the promoter never runs): retail's toggle branches on
// the promoted byte (@0x4df17f), so its engage branch re-raises that state.
bool player_view_scope_request_pending(const PlayerViewState &v, bool engaged);

// The witnessed toggle: the mid-ease refusal, then per branch the promoted
// byte clear, the interp Setup (engage: 15 steps, or 7 for Inset weapons,
// from the hip copy to tpos; disengage: the same, or 1 on the hipfire-return
// leg, from tpos to the hip copy -- an idle interp sources the def pose, not
// the camera, so a hip-parked raise snaps to tpos for its one return step),
// the target flip and the hipfire latch store. Returns false (state untouched)
// when refused mid-ease; true for a no-op request. [orig: Player_ToggleWeaponScope
// @0x4df177; disengage @0x4df1b3..0x4df212; engage @0x4df31d..0x4df373]
bool player_view_set_engaged(PlayerViewState &v, bool engaged, bool inset_weapon);

// A DERIVED hip (0) .. tpos (1) progress readout for probes and tests; retail
// has no such scalar (its consumers read the promoted byte and the published
// biases). Idle: the latch's target (hipfire 1 -> 0, else 1). Active: the
// progress of the first lane with an authored hip..ADS span (position lanes,
// then rotation), clamped to [0, 1]; a zero-span ease reports its SOURCE
// (hipfire 1 -> 1, else 0) until it lands. Never a gameplay input.
float player_view_scope_fraction(const PlayerViewState &v);

// The per-frame movement input pack and its scope legs, in retail order
// [orig: Player_PackInputStateToEntity @0x4df450]. Latches `move_held` (any of
// the four movement-direction keys [orig: g_MovementKeyHeld set @0x4df4bb,
// cleared @0x4df4f9]) and returns true when the SETTLED-at-scope unscope must
// fire: movement while PROMOTED on a Scoped (flags 1) weapon routes through
// the normal scope toggle [orig: g_WeaponScopeActive && Def->Flags & 1 ->
// Player_ToggleWeaponScope, the call @0x4df4ec from the gate @0x4df4c9] -- the
// caller runs its standard disengage, and the toggle's own ForceScoped pin
// applies there. Otherwise, on a Scoped def [orig: the entitySlotPtr block
// @0x4df500..0x4df63b], the three interp legs run here:
//   (a) moving while the RAISE runs (interp active, hipfire 0): the interp is
//       re-targeted at the hip FROM ITS OWN POSE over 15 steps and hipfire is
//       set -- the engaged target stays latched, so the promoter later promotes
//       "scoped" at the hip [orig: @0x4df548..0x4df56c];
//   (b) moving while settled with hipfire 0 on a def without ForceScoped or
//       Emplaced (0x20000080): a 15-step tpos -> hip ease with hipfire set, the
//       promoted byte KEPT [orig: @0x4df57c..0x4df58e, @0x4df5ae..0x4df5d6];
//   (c) not moving, settled, idle with hipfire 1, no 0x20000080: the promoted
//       byte drops, the target is set, a 15-step hip -> tpos ease starts and
//       hipfire clears -- the auto re-raise [orig: LABEL_33 @0x4df607..0x4df63b].
// After the step-1 toggle every leg is a no-op (the toggle leaves the interp
// active with hipfire 1, or pinned), so they are only evaluated when it does
// not fire; retail's ordering is thereby preserved without the caller's help.
bool player_view_move_input(PlayerViewState &v, bool move_held, int32_t def_flags);

// Whether a scope-UP toggle is refused by the movement-held latch: engaging a
// Scoped (flags 1) weapon is blocked while a movement key is down
// [orig: g_MovementKeyHeld && (flags & 1) -> return @ 0x4df29c].
bool player_view_scope_up_blocked(const PlayerViewState &v, int32_t def_flags);

// Toggle the persistent binocular request. Raising is resolved separately so
// movement/death/round-end/camera suppression never destroys the request.
// Turning the request off clears both derived states immediately. Returns the
// new requested state. [orig: input action 26; g_BinocularsToggle]
bool player_view_toggle_binoculars(PlayerViewState &v);

// The one random fixed-radius aim displacement a binocular raise seeds:
// `unit_random` in [0, 1) picks the angle around the kBinocularAimOffsetDeg
// circle; local_player_binocular_sway_latch owns their render-time lifetime.
// [orig: Binoculars_RandomizeSwayOffsets @0x4dd830]
void player_view_binocular_sway_offset(float unit_random,
                                       float &yaw_offset_deg,
                                       float &pitch_offset_deg);

// Recompute the binocular body pose and first-person view. The raised pose is
// suppressed by a direction bit in the frame's input word, death, and round
// end, but survives third person; the optical view additionally requires
// first person. [orig: Player_UpdatePerFrame @0x4de37b..0x4de3c8 -- Health
// @0x4de38d, g_SpawnSuccessGate @0x4de39e, `g_InputFlags & 0x1E` @0x4de3ae,
// g_CameraMode == 1 @0x4de3bf]
void player_view_update_effective_modes(PlayerViewState &v, bool alive, bool round_ended);

// Toggle NVG and return its new active state. Gain is independent of the
// toggle and is retained while inactive. [orig: input action 41]
bool player_view_toggle_nvg(PlayerViewState &v);

// Add `delta`, clamp to the retail five-position range, store, and return it.
// [orig: input actions 56/57]
int32_t player_view_adjust_nvg_gain(PlayerViewState &v, int32_t delta);

// The NVG state remains active in every camera, but its world/post treatment
// needs the resolved mode word g_CameraMode == 0: the chase (1) and the
// death lerp camera (4) both drop it. [orig: the g_CameraMode gates in the
// NVG render path, player_view.cpp]
bool player_view_nvg_visible(const PlayerViewState &v);

// Main-camera horizontal FOV. The weather current is independent of the ADS
// pose ease. Resolved optical flags select 80/zoom (Sighted) or current/zoom
// (Scoped); binoculars select 20 degrees. The caller owns the visibility gates.
// [orig: Render_ProcessMainSceneFrame @0x5CA3C5..0x5CA4A6]
//
// The same retail block also adds an EQUIPPED-SLOT sway to the view angles that
// this port does not model: `pitch -= slot->field_4; yaw += slot->field_8` off
// the local player's EquippedSlot (entity +0x118, a MountSlot). Its gate is
// NOT the same on the two optical arms -- the SIGHTED arm applies it only when
// the slot's WeaponDef word at +0x84 is non-zero [orig: @0x5ca452..0x5ca465],
// the SCOPED arm applies it whenever a slot is equipped at all
// [orig: @0x5ca496..0x5ca4a0]. The MountSlot pitch/yaw pair is the turret aim
// our vehicle mount does not publish to the view yet, so neither arm sways
// here; the binocular arm's own pair is ported
// (LocalPlayerViewTracker::binocular_*_offset_deg).
float player_view_fov_h_deg(const PlayerViewState &v, int32_t current_fov_q16,
                           bool scoped, bool sighted, int32_t zoom);

// Horizontal -> vertical projection fov through the aspect ratio, degrees.
// [orig: Render_SetViewAndProjectionMatrices @ 0x58d900:
//  fovY = 2*atan(tan(fovX/2) / aspect)]
float fov_vertical_from_horizontal_deg(float fov_h_deg, float aspect);

// THE FRAME'S PROJECTION for one pass over the real surface [orig:
// Render_SetViewAndProjectionMatrices @0x58d900 -- viewportWidth = w * scaleX
// @0x58d971, viewportHeight = h * scaleY @0x58d985, halfV = atan(tan(fov_h/2)
// * (vh / vw)) @0x58d9b2, aspect = vw / vh @0x58d9be ->
// D3DXMatrixPerspectiveFovLH(2 * halfV, aspect, near, far) @0x58d9de]. With
// scaleX = 1 and scaleY = flt_8409E8 = selected / (h/w) (renderer/aspect_ratio.h
// aspect_viewport_scale_y) that is proj[0][0] = cot(fov_h/2) across the REAL
// width and proj[1][1] = 1 / (tan(fov_h/2) * selected) across the REAL height:
// the horizontal fov never moves with the mode and the vertical half-extent
// follows the SELECTED ratio, not the surface's -- non-square pixels whenever
// the two differ (mode 0 on a 16:9 surface compresses the picture to 0.75 of
// its height, the classic wide stretch). Equivalently: the frustum a target of
// aspect 1/selected renders natively, stretched by scale_y onto the surface. A
// shell whose camera couples the two fovs through its viewport aspect
// reproduces the pass by rendering through such a target and blitting it
// full-surface; `target_w/h` is that target, the surface itself at scale 1 and
// otherwise never below the surface on either axis (the resampled axis is
// super-, never under-sampled).
struct ViewProjection {
    float fov_h_deg = 0.0f; // the horizontal fov, mode-invariant
    float fov_v_deg = 0.0f; // 2 * atan(tan(fov_h/2) * selected)
    float aspect = 1.0f;    // vw / vh = 1 / selected
    float scale_y = 1.0f;   // flt_8409E8: the vertical stretch onto the surface
    int target_w = 0;
    int target_h = 0;
};
ViewProjection view_projection(float fov_h_deg, int aspect_mode, int surface_w,
                               int surface_h);

// The NVG scene's pass over the 512-square target [orig: NVG_RenderScene
// @0x5d2954..0x5d296d (scaleY = flt_8409EC x 512 / 512: the frame's own
// frustum); NVG_RenderSightedScene @0x5d2aa9..0x5d2ada (the
// same shape at the Sighted arm's fov, nvg_sighted_scene_fov_q16); NVG_RenderScopedScene
// @0x5d29e4..0x5d2a2a (scaleY 1.0: the square Scoped frustum,
// renderer/nvg_scope_lens.h nvg_scoped_scene_fov_q16)]. Retail rasterises
// each into 512 x 512 and stretches it over the surface, so every arm's
// target is the 512 square: the frame-shaped arms keep the frame's `aspect`
// (and fov_v), their texels non-square, a projection a shell whose camera
// couples the two fovs through its target's ratio must supply explicitly;
// the Scoped arm's frustum is the square itself (aspect 1).
// `frame` is the frame's view_projection, `nvg` the frame's NVG arms,
// `selected_h_over_w` the selected ratio (flt_8409EC), `zoom` the slot's
// clamped magnification.
ViewProjection nvg_view_projection(const ViewProjection &frame,
                                   const renderer::FrameFxNvgPlan &nvg,
                                   float selected_h_over_w, int32_t zoom);

// The first-person viewmodel pass differs from the world pass only in its
// horizontal fov (the weapon renderfov): both push scaleX = 1 and the same
// flt_8409E8 as scaleY, so the focal ratio between the two frusta is the ratio
// of the horizontal half-tangents on BOTH axes, whatever the mode or surface
// [orig: the FP pass @0x4dee5a..0x4dee7f (fovDegrees = WeaponDef+0x148, the
//  caller's scaleY -- flt_8409E8 via Player_RenderViewModelIfAlive @0x4e0154);
//  the world pass Render_SetViewProjectionWithDefaults @0x58f6b0].
float viewmodel_focal_ratio(float world_fov_h_deg, float renderfov_h_deg);

// The first-person view position in RAW weapon.def units: the def `pos`
// (+0xF4) plus the interp's PUBLISHED position bias (g_ViewPosBiasX/Y/Z,
// the truncating ftol of interp_current - the hip copy at +0x10C, in the
// def's *256 Q16 scale) brought back to file units over kWeaponDefPosScale.
// Zero bias at the hip, tpos - pos once the lanes snap; no float blend and no
// tpos input, the ADS endpoint lives in the bound pose. [orig:
// Player_UpdateFirstPersonCamera @ 0x4dd380 -- ftol(Bone +0xF4) @0x4dd479..
// 0x4dd490 then + g_ViewPosBiasX/Y/Z @0x4dd4ce..0x4dd4da; Player_StepFpViewBiasInterp publication
// @ 0x4ddf53..0x4ddfc3]. (The camera's `Flags & 2` leg is the dead/round-end
// camera, not ADS; unported.)
void player_view_bias_units(const PlayerViewState &v, const float pos[3], float out[3]);

// The view bias in VIEW-FRAME world units (X=forward, Y=left, Z=up — the
// witnessed def/view frame; the aim ray's far point is {+1000, 0, 0} through
// the same transform @ 0x592a0f): the raw position over kWeaponDefPosScale;
// while the NoCardSwitch reload rule suppresses the bias the published half
// drops for the frame (the hip offset — the ported reading of retail's
// skipped camera-bias add). The presenting shell maps view axes onto its
// camera frame and parents the viewmodel — node work only.
// [orig: Player_UpdateFirstPersonCamera @ 0x4dd380 — the view-local rotate
//  @ 0x4dd5d8; the suppress skip @ 0x4dd439/@ 0x4dd4cc]
void player_view_bias_view_units(const PlayerViewState &v, bool suppress_bias,
                                 const float pos[3], float out[3]);

// The FP camera's recoil pitch: TWICE the live accumulator, camera-only —
// third-person orbit, projectile aim, and the HUD anchor keep the base pitch.
// [orig: Camera_ComputeThirdPersonView @ 0x437fc7]
float player_view_fp_pitch_recoil_deg(int32_t recoil_pitch_bam);

// The FP camera roll: torsoRoll + lean/4 (arithmetic-shift BAM quarter).
// [orig: the on-foot person leg @ 0x437fe6 —
//  g_ViewRotRoll = entity+0x2DC + (entity+0xB0 >> 2)]
float player_view_fp_roll_deg(int32_t torso_roll_bam, int32_t lean_bam);

// The chase camera's collision-march NO-COLLISION landing [orig:
// @ 0x438213..0x43832e]: under 8.0u the eye marches back in 0.25u steps for
// stepIndex 1..numSteps-1 (numSteps = floor(dist/0.25)) and stays on the LAST
// step — never the full distance (numSteps <= 1 leaves the eye at the pivot
// @ 0x43821f). The per-step bone-collision FORCES (the obstruction pull-in)
// stay a tracked deferral (net-re §5.39).
float player_view_tp_effective_distance(float distance);

// Advance the three filters from ONE weather-PRNG word and return the BAM32
// deltas to add to the FP view rotation. A zero counter produces nothing and
// does NOT advance the filters — retail gates the whole block on it.
//
// Each axis takes a different BIT SLICE of the SAME word, so the three are
// correlated exactly as retail's are; drawing three independent randoms would
// change the character of the shake even with identical per-axis statistics.
// The slices are arithmetic on int32 (sign-propagating), the filter is
// s = (7*s + n) >> 3, and the delta is (clamped * s) >> 6 with the multiply
// left to WRAP as retail's imul does.
// [orig: the block @0x43803c..0x4380df — roll (2p) sar 5 @0x43806c, pitch
//  (p << 17) sar 5 @0x43807c, yaw (p << 9) sar 5 @0x438093; the sar 3 filters
//  @0x4380a1..0x4380a7; the imul / sar 6 applies @0x4380b0..0x4380d9]
void camera_shake_sample(CameraShakeState &st, uint32_t weather_prng,
                         int32_t &d_yaw_bam, int32_t &d_pitch_bam,
                         int32_t &d_roll_bam);

// The CHASE (mode-1) shake leg: a stateless sin/cos chain over the RAW counter
// and the engine tick, scaled by an amplitude built from the weather PRNG's
// low byte — it never touches the mode-0 IIR filters, so a mode flip resumes
// them exactly where they stopped. Retail applies it to any non-zero camera
// mode after the look-at compose (the mode-4 lerp then overwrites the
// rotation wholesale, so only the chase renders it).
// amp = (min(4*counter, 255) * ((prng & 0xFF) + 64)) >> 8, each term
// truncated to int (ftol) before its add/subtract; the two tick terms are
// quartered by an ARITHMETIC >> 2 after truncation.
// [orig: Camera_ComputeThirdPersonView @0x437d10, the mode>=1 block
//  @0x438939..0x4389e5 — amp @0x43893f..0x438967; yaw += sin(C*0.4)*amp
//  @0x438974..0x43898b; pitch += sin(C*2/7)*amp - (sin(T*25/34)*amp >> 2)
//  @0x438985..0x4389cf; roll = sin(C*2/11)*amp - (cos(T*0.862069)*amp >> 2)
//  over a zero base @0x43899c..0x4389e5]
void camera_shake_sample_chase(const CameraShakeState &st, uint32_t weather_prng,
                               uint32_t tick, int32_t &d_yaw_bam,
                               int32_t &d_pitch_bam, int32_t &d_roll_bam);

// The head-bone eye's FIVE-SAMPLE terrain floor [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b6c08..0x4b6ca4]: the eye Z is floored
// at the MAX of the bilinear terrain height at the eye column and at
// ±kEyeTerrainProbeRadius along each ground axis, each raised
// kEyeTerrainClearance — skipped INDOORS (Flags & 0x800000
// kEntityFlagIndoors: the heightmap has no interiors, @ 0x4b6c08/@ 0x4b6c16).
// Mission-space floats; a null/invalid field leaves the eye untouched.
void player_view_floor_eye_to_terrain(const terrain::TerrainHeightField *terrain,
                                      bool indoors, float eye[3]);

// The camera legs of one Camera_ComputeThirdPersonView call that need no
// world lookup, mission space — the witnessed pose math; the presenting shell
// converts frames and stamps the Camera3D node. The mode-0 carrier legs and
// the ground-entity leg run before this (local_player_camera_compose,
// world/local_player_view.h). `anchor_eye` is live Position + the motor's
// CameraOffset; pass valid=false for a non-person entity. `aim_yaw/pitch_deg`
// are the body's aim angles and `entity_roll_deg` its own Roll word.
// `terrain` + `indoors` feed the chase clearances; the motor already floors
// the on-foot head. `march_candidates` is the entity's proximity-candidate
// count being non-zero (the chase march's gate). Advances the composition's
// own state: the person leg clears the ground-entity lift and the mounted
// chase eases the look-ahead.
// First person [orig: Camera_ComputeThirdPersonView @ 0x437d10 mode 0, the
// person leg @ 0x437f9c..0x438031]: eye = the floored anchor pulled back
// kFpEyePullback along the view forward; pitch adds the doubled recoil;
// roll = torsoRoll + lean/4. A non-person entity takes the +1.0 bump over
// Position under its own rotation triple instead [orig: @0x437E89..0x437E99].
// Third person [orig: mode 1 @ 0x4380E4..0x438650 and the look-at
// @0x4387DF..0x43892D]: the anchor-translated view matrix places the eye
// `distance` back (or on the collision march's landing) and the pivot nudge
// R*(nudge,nudge,nudge) is the look-at target; the clearances apply to every
// chase eye; roll 0. Mounted (`v.mount.control_seat`) [orig: the mounted arm
// of mode 1 — yaw @0x438138..0x43814A, pitch @0x438150, distance
// @0x438121..0x438136, the slope march @0x43846E..0x438619, the aircraft
// drop @0x43861D..0x43864C, the look-ahead @0x438811..0x4388AF]: the eye sits
// mount_distance(r) behind the eased mounted anchor along the quarter-damped
// look yaw at the fixed downward pitch, is floored by the clearances and the
// slope raise, dropped r/2 on an aircraft, and the final angles look at the
// pivot plus the eased look-ahead (world/tp_camera_mount.h carries the
// constants).
// Mode 4 [orig: the lerp @0x4389eb..0x438b49]: the death camera's FROM/TO
// poses lerped against `v.view_tick` (world/death_camera.h), converted
// through world/angle.h; roll rides the same lerp.
struct PlayerCameraPose {
    float eye[3] = {0.0f, 0.0f, 0.0f}; // mission units
    float yaw_deg = 0.0f;              // mission-euler view angles
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    bool third_person = false;
};
void player_view_compose_camera(PlayerViewState &v,
                                const float position[3],
                                const float anchor_eye[3], bool anchor_valid,
                                const terrain::TerrainHeightField *terrain,
                                bool indoors,
                                float aim_yaw_deg, float aim_pitch_deg,
                                int32_t recoil_pitch_bam,
                                int32_t torso_roll_bam, int32_t lean_bam,
                                bool march_candidates, float entity_roll_deg,
                                PlayerCameraPose &out);

} // namespace opennova::world
