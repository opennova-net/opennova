// The local player's view-side fixed-tick state: the ADS scope-camera ease and
// the third-person anchor chase, plus the camera fov policy they drive.
//
// The original runs these in its 62 Hz frame loop: the scope camera interp is a
// 15-step ease [orig: CNetPlayerInterp_Setup @ 0x4df36e; engaged mirror
// g_scopeEngaged @ 0x82CE94], the chase camera's anchor eases a quarter-step
// per tick [orig: ThirdPersonCamera_Update @ 0x437c8d], and the scoped fov is
// 80 / zoom for sighted weapons, suppressed in third person
// [orig: Player_ToggleWeaponScope @ 0x4df401 / the g_camera_mode check
// @ 0x4df3fa; base fov g_cameraFovDeg @ 0x26C6848 default 0x500000 = 80 deg].
//
// Hosted, the simulation ticks this state at the world cadence and render
// frames only READ it — camera lag and the ADS swing are therefore identical
// at 30, 60, or 144 fps (ADR 0016: policy, state, and cadence live in the
// engine; the host samples input and applies node transforms).

#ifndef OPENNOVA_WORLD_PLAYER_VIEW_H
#define OPENNOVA_WORLD_PLAYER_VIEW_H

#include <cstdint>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

// The per-toggle ease lengths [orig: CNetPlayerInterp_Setup call sites in
// Player_ToggleWeaponScope — engage 15 @ 0x4df36e / 7 for Inset (flags2 0x200)
// weapons @ 0x4df355; disengage mirrors them @ 0x4df201 / @ 0x4df1e8, and the
// hipfire-return leg is a single step @ 0x4df1c3].
constexpr int32_t kScopeEaseSteps = 15;
constexpr int32_t kScopeEaseStepsInset = 7;
constexpr int32_t kScopeEaseStepsHipfire = 1;
// [orig: g_cameraFovDeg @ 0x26C6848 default 0x500000 = 80.0 horizontal degrees]
constexpr float kPlayerCameraFovHDeg = 80.0f;
// [orig: binocular camera fov constant in Player_UpdateFirstPersonCamera]
constexpr float kBinocularCameraFovHDeg = 20.0f;
// The fixed radius of the one random aim displacement a binocular raise seeds
// (2.8125 deg = 0x02000000 BAM32). The displacement survives movement/death/
// third-person suppression until the raw toggle (g_binocularsToggle) drops.
// [orig: the binocular-raise aim offset seeded with the input action 26 toggle]
constexpr float kBinocularAimOffsetDeg = 2.8125f;
// [orig: the five NVG gain positions selected by actions 56/57]
constexpr int32_t kNvgGainMin = 0;
constexpr int32_t kNvgGainMax = 4;
// [orig: the chase anchor ease @ 0x437c8d — one quarter per 62 Hz tick]
constexpr float kTpAnchorEase = 0.25f;
// The first-person eye pull-back along the full view rotation: -0x3000 on the
// view-frame FORWARD axis [orig: Math_FixedPointTransformPoint22 of
// (-0x3000, 0, 0) added onto g_view_pos @ 0x438001..0x438031].
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

struct PlayerViewState {
    bool scope_engaged = false;   // [orig: g_scopeEngaged @ 0x82CE94]
    int32_t scope_step = 0;       // 0 (hip) .. ease_steps (sighted), of the CURRENT ease
    int32_t ease_steps = kScopeEaseSteps; // latched per toggle [orig: the Setup steps arg]
    bool scope_hipfire = true;    // [orig: g_scopeHipfire @ 0x82CE98, init/reset 1]
    bool move_held = false;       // [orig: the movement-held latch g_movementKeyHeld @ 0xB7653B]
    bool third_person = false;    // [orig: g_camera_mode @ 0xA890C8]
    bool binoculars_requested = false;   // [orig: raw toggle g_binocularsToggle @ 0xB76539]
    bool binoculars_raised = false;      // [orig: body-pose g_binocularsRaised @ 0xB7653A]
    bool binoculars_view_active = false; // [orig: first-person view g_binocularsViewActive @ 0xB76538]
    bool nvg_active = false;
    int32_t nvg_gain = kNvgGainMin;
    bool tp_anchor_valid = false;
    float tp_anchor[3] = {0.0f, 0.0f, 0.0f}; // mission space (Z-up)
};

// One 62.5 Hz tick: step the scope ease toward the engaged target and chase
// the third-person anchor toward `eye` (mission space). Entering third person
// seeds the anchor at the eye [orig: Camera_SetTrackedEntity @ 0x4391d0 resets
// the track on change]; leaving invalidates it.
void player_view_tick(PlayerViewState &v, const float eye[3]);

// Whether the scope-camera interp is mid-ease. Every scope toggle is REFUSED
// while it runs [orig: the !g_fpCameraInterp.activeFlag gate @ 0x4df177].
bool player_view_scope_ease_active(const PlayerViewState &v);

// The witnessed toggle: latch this ease's step count (engage: 15, or 7 for
// Inset weapons; disengage: the same, or 1 on the hipfire-return leg), seed the
// step at the departing endpoint, and flip the target. Returns false (state
// untouched) when refused mid-ease. [orig: Player_ToggleWeaponScope
// @ 0x4df1b3..0x4df373 — the Setup calls + the g_scopeHipfire writes]
bool player_view_set_engaged(PlayerViewState &v, bool engaged, bool inset_weapon);

// The eased hip->sighted blend, 0..1 in 1/15ths.
float player_view_scope_fraction(const PlayerViewState &v);

// The per-tick movement input and its settled-scope leg [orig:
// Player_PackInputStateToEntity @ 0x4df450]. Latches `move_held` (any of the
// four movement-direction keys [orig: g_movementKeyHeld set @ 0x4df4bb, cleared
// @ 0x4df4f9]) and returns true when the SETTLED-at-scope auto-unscope must
// fire: movement while fully sighted on a Scoped (flags 1) weapon routes
// through the normal scope toggle [orig: g_weaponScopeActive && Def->Flags & 1
// -> Player_ToggleWeaponScope @ 0x4df4c9..0x4df4ec] — the caller runs its
// standard disengage, and the toggle's own ForceScoped pin applies there.
// The mid-ease reversal and the auto-re-raise legs (@ 0x4df548 / @ 0x4df5ae /
// @ 0x4df607) are witnessed-deferred: they keep g_scopeEngaged latched while
// easing to the hip, which needs the explicit engaged/active/hipfire tri-state
// (net-re section 5.62 follow-up).
bool player_view_move_input(PlayerViewState &v, bool move_held, int32_t def_flags);

// Whether a scope-UP toggle is refused by the movement-held latch: engaging a
// Scoped (flags 1) weapon is blocked while a movement key is down
// [orig: g_movementKeyHeld && (flags & 1) -> return @ 0x4df29c].
bool player_view_scope_up_blocked(const PlayerViewState &v, int32_t def_flags);

// Toggle the persistent binocular request. Raising is resolved separately so
// movement/death/round-end/camera suppression never destroys the request.
// Turning the request off clears both derived states immediately. Returns the
// new requested state. [orig: input action 26; g_binocularsToggle]
bool player_view_toggle_binoculars(PlayerViewState &v);

// The one random fixed-radius aim displacement a binocular raise seeds:
// `unit_random` in [0, 1) picks the angle around the kBinocularAimOffsetDeg
// circle; the yaw/pitch offsets persist until the request drops (the caller
// zeroes them then). [orig: the binocular-raise offset beside g_binocularsToggle]
void player_view_binocular_sway_offset(float unit_random,
                                       float &yaw_offset_deg,
                                       float &pitch_offset_deg);

// Recompute the binocular body pose and first-person view. The raised pose is
// suppressed by movement, death, and round end, but survives third person;
// the optical view additionally requires first person. [orig: per-frame
// binocular state update around g_binocularsViewActive..g_movementKeyHeld]
void player_view_update_effective_modes(PlayerViewState &v, bool alive, bool round_ended);

// Toggle NVG and return its new active state. Gain is independent of the
// toggle and is retained while inactive. [orig: input action 41]
bool player_view_toggle_nvg(PlayerViewState &v);

// Add `delta`, clamp to the retail five-position range, store, and return it.
// [orig: input actions 56/57]
int32_t player_view_adjust_nvg_gain(PlayerViewState &v, int32_t delta);

// The NVG state remains active in third person, but its world/post treatment
// is first-person only. [orig: g_camera_mode gates in the NVG render path]
bool player_view_nvg_visible(const PlayerViewState &v);

// The main camera's HORIZONTAL fov in degrees: 80 at the hip, eased to
// 80 / scope_max_mag for sighted defs (file flag 2) with a magnification,
// overridden by the fixed 20-degree binocular view, and pinned to 80 in third
// person. `def_flags` is the raw weapon.def flag mask, `scope_max_mag` the
// def's zoom (0 = key absent).
// [orig: Player_ToggleWeaponScope @ 0x4df401; 3P suppress @ 0x4df3fa]
float player_view_fov_h_deg(const PlayerViewState &v, int32_t def_flags, float scope_max_mag);

// Horizontal -> vertical projection fov through the aspect ratio, degrees.
// [orig: Render_SetViewAndProjectionMatrices @ 0x58d900:
//  fovY = 2*atan(tan(fovX/2) / aspect)]
float fov_vertical_from_horizontal_deg(float fov_h_deg, float aspect);

// The eased first-person view bias in RAW weapon.def units: `pos` (hip) blended
// toward `tpos` (sighted) by the scope fraction. The original's camera swaps
// pos -> tpos instantly once sighted (entity Flags & 2 -> WeaponDef.AltCamOffset
// @ +0x10C) with the visible ease carried by the interp [orig:
// Player_UpdateFirstPersonCamera @ 0x4dd380; the interp @ 0x4df36e].
void player_view_bias_units(const PlayerViewState &v, const float pos[3],
                            const float tpos[3], float out[3]);

// The eased view bias in VIEW-FRAME world units (X=forward, Y=left, Z=up —
// the witnessed def/view frame; the aim ray's far point is {+1000, 0, 0}
// through the same transform @ 0x592a0f): the raw blend over
// kWeaponDefPosScale; while the NoCardSwitch reload rule suppresses the bias
// the ADS half drops for the frame (the hip offset — the ported reading of
// retail's skipped camera-bias add). The presenting shell maps view axes onto
// its camera frame and parents the viewmodel — node work only.
// [orig: Player_UpdateFirstPersonCamera @ 0x4dd380 — the view-local rotate
//  @ 0x4dd5d8; the suppress skip @ 0x4dd439/@ 0x4dd4cc]
void player_view_bias_view_units(const PlayerViewState &v, bool suppress_bias,
                                 const float pos[3], const float tpos[3],
                                 float out[3]);

// The FP camera's recoil pitch: TWICE the live accumulator, camera-only —
// third-person orbit, projectile aim, and the HUD anchor keep the base pitch.
// [orig: Camera_ComputeThirdPersonView @ 0x437fc7]
float player_view_fp_pitch_recoil_deg(int32_t recoil_pitch_bam);

// The FP camera roll: torsoRoll + lean/4 (arithmetic-shift BAM quarter).
// [orig: the on-foot person leg @ 0x437fe6 —
//  g_view_rot_roll = entity+0x2DC + (entity+0xB0 >> 2)]
float player_view_fp_roll_deg(int32_t torso_roll_bam, int32_t lean_bam);

// The chase camera's collision-march NO-COLLISION landing [orig:
// @ 0x438213..0x43832e]: under 8.0u the eye marches back in 0.25u steps for
// stepIndex 1..numSteps-1 (numSteps = floor(dist/0.25)) and stays on the LAST
// step — never the full distance (numSteps <= 1 leaves the eye at the pivot
// @ 0x43821f). The per-step bone-collision FORCES (the obstruction pull-in)
// stay a tracked deferral (net-re §5.39).
float player_view_tp_effective_distance(float distance);

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

// The head-bone eye's FIVE-SAMPLE terrain floor [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b6c08..0x4b6ca4]: the eye Z is floored
// at the MAX of the bilinear terrain height at the eye column and at
// ±kEyeTerrainProbeRadius along each ground axis, each raised
// kEyeTerrainClearance — skipped INDOORS (Flags & 0x800000
// kEntityFlagIndoors: the heightmap has no interiors, @ 0x4b6c08/@ 0x4b6c16).
// Mission-space floats; a null/invalid field leaves the eye untouched.
void player_view_floor_eye_to_terrain(const terrain::TerrainHeightField *terrain,
                                      bool indoors, float eye[3]);

// The composed local camera for one presented frame, mission space — the
// witnessed pose math; the presenting shell converts frames and stamps the
// Camera3D node. `anchor_eye` is the shell-fed head-bone eye (the permanent
// D-INF-18 write-back; pass valid=false for the non-person +1.0 bump over
// `position`), `aim_yaw/pitch_deg` the post-binocular aim angles. `terrain` +
// `indoors` feed the head-bone eye's terrain floor (above; null skips it).
// First person [orig: Camera_ComputeThirdPersonView @ 0x437d10 mode 0, the
// on-foot person leg @ 0x437f9c..0x438031]: eye = the floored anchor pulled
// back kFpEyePullback along the view forward; pitch adds the doubled recoil;
// roll = torsoRoll + lean/4.
// Third person [orig: mode 1 @ 0x438100..0x4383e2]: the chased anchor plus
// the pivot nudge R*(nudge,nudge,nudge) backed off by the march-landed
// distance along the orbit forward; roll 0. With no march collision ported,
// emitting the seed angles equals the original's final look-at recompute.
struct PlayerCameraPose {
    float eye[3] = {0.0f, 0.0f, 0.0f}; // mission units
    float yaw_deg = 0.0f;              // mission-euler view angles
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    bool third_person = false;
};
void player_view_compose_camera(const PlayerViewState &v,
                                const float position[3],
                                const float anchor_eye[3], bool anchor_valid,
                                const terrain::TerrainHeightField *terrain,
                                bool indoors,
                                float aim_yaw_deg, float aim_pitch_deg,
                                int32_t recoil_pitch_bam,
                                int32_t torso_roll_bam, int32_t lean_bam,
                                PlayerCameraPose &out);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_VIEW_H
