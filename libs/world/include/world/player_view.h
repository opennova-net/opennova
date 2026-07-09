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

// [orig: the interp step count @ 0x4df36e]
constexpr int32_t kScopeEaseSteps = 15;
// [orig: g_cameraFovDeg @ 0x26C6848 default 0x500000 = 80.0 horizontal degrees]
constexpr float kPlayerCameraFovHDeg = 80.0f;
// [orig: the chase anchor ease @ 0x437c8d — one quarter per 62 Hz tick]
constexpr float kTpAnchorEase = 0.25f;

struct PlayerViewState {
    bool scope_engaged = false;   // [orig: g_scopeEngaged @ 0x82CE94]
    int32_t scope_step = 0;       // 0 (hip) .. kScopeEaseSteps (sighted)
    bool third_person = false;    // [orig: g_camera_mode @ 0xA890C8]
    bool tp_anchor_valid = false;
    float tp_anchor[3] = {0.0f, 0.0f, 0.0f}; // mission space (Z-up)
};

// One 62.5 Hz tick: step the scope ease toward the engaged target and chase
// the third-person anchor toward `eye` (mission space). Entering third person
// seeds the anchor at the eye [orig: Camera_SetTrackedEntity @ 0x4391d0 resets
// the track on change]; leaving invalidates it.
void player_view_tick(PlayerViewState &v, const float eye[3]);

// The eased hip->sighted blend, 0..1 in 1/15ths.
float player_view_scope_fraction(const PlayerViewState &v);

// The main camera's HORIZONTAL fov in degrees: 80 at the hip, eased to
// 80 / scope_max_mag for sighted defs (file flag 2) with a magnification,
// and pinned to 80 in third person. `def_flags` is the raw weapon.def flag
// mask, `scope_max_mag` the def's zoom (0 = key absent).
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
