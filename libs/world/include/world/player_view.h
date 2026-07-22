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
// [orig: the five NVG gain positions selected by actions 56/57]
constexpr int32_t kNvgGainMin = 0;
constexpr int32_t kNvgGainMax = 4;
// [orig: the chase anchor ease @ 0x437c8d — one quarter per 62 Hz tick]
constexpr float kTpAnchorEase = 0.25f;

struct PlayerViewState {
    bool scope_engaged = false;   // [orig: g_scopeEngaged @ 0x82CE94]
    int32_t scope_step = 0;       // 0 (hip) .. ease_steps (sighted), of the CURRENT ease
    int32_t ease_steps = kScopeEaseSteps; // latched per toggle [orig: the Setup steps arg]
    bool scope_hipfire = true;    // [orig: g_scopeHipfire @ 0x82CE98, init/reset 1]
    bool move_held = false;       // [orig: the movement-held latch byte_B7653B @ 0xB7653B]
    bool third_person = false;    // [orig: g_camera_mode @ 0xA890C8]
    bool binoculars_requested = false;   // [orig: raw toggle byte_B76539 @ 0xB76539]
    bool binoculars_raised = false;      // [orig: body-pose byte_B7653A @ 0xB7653A]
    bool binoculars_view_active = false; // [orig: first-person view byte_B76538 @ 0xB76538]
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
// four movement-direction keys [orig: byte_B7653B set @ 0x4df4bb, cleared
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
// [orig: byte_B7653B && (flags & 1) -> return @ 0x4df29c].
bool player_view_scope_up_blocked(const PlayerViewState &v, int32_t def_flags);

// Toggle the persistent binocular request. Raising is resolved separately so
// movement/death/round-end/camera suppression never destroys the request.
// Turning the request off clears both derived states immediately. Returns the
// new requested state. [orig: input action 26; byte_B76539]
bool player_view_toggle_binoculars(PlayerViewState &v);

// Recompute the binocular body pose and first-person view. The raised pose is
// suppressed by movement, death, and round end, but survives third person;
// the optical view additionally requires first person. [orig: per-frame
// binocular state update around byte_B76538..byte_B7653B]
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

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_VIEW_H
