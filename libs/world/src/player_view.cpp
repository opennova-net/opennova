// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include "world/player_view.h"

#include <cmath>

namespace opennova::world {

void player_view_tick(PlayerViewState &v, const float eye[3]) {
    // The 15-step scope-camera ease, one step per tick toward the engaged
    // target. [orig: the interp steps @ 0x4df36e]
    v.scope_step += v.scope_engaged ? 1 : -1;
    if (v.scope_step < 0) v.scope_step = 0;
    if (v.scope_step > kScopeEaseSteps) v.scope_step = kScopeEaseSteps;

    if (v.third_person) {
        if (!v.tp_anchor_valid) {
            // Entering third person seeds the track at the eye.
            // [orig: Camera_SetTrackedEntity @ 0x4391d0 resets on change]
            v.tp_anchor[0] = eye[0];
            v.tp_anchor[1] = eye[1];
            v.tp_anchor[2] = eye[2];
            v.tp_anchor_valid = true;
        } else {
            // Quarter-step ease per 62 Hz tick. [orig: @ 0x437c8d]
            v.tp_anchor[0] += (eye[0] - v.tp_anchor[0]) * kTpAnchorEase;
            v.tp_anchor[1] += (eye[1] - v.tp_anchor[1]) * kTpAnchorEase;
            v.tp_anchor[2] += (eye[2] - v.tp_anchor[2]) * kTpAnchorEase;
        }
    } else {
        v.tp_anchor_valid = false;
    }
}

float player_view_scope_fraction(const PlayerViewState &v) {
    return static_cast<float>(v.scope_step) / static_cast<float>(kScopeEaseSteps);
}

float player_view_fov_h_deg(const PlayerViewState &v, int32_t def_flags, float scope_max_mag) {
    // Third person renders at the base fov regardless of the scope state.
    // [orig: @ 0x4df3fa g_camera_mode -> 80.0]
    if (v.third_person) return kPlayerCameraFovHDeg;
    // Sighted defs (file flag 2) with a magnification zoom to 80 / mag.
    // [orig: Player_ToggleWeaponScope @ 0x4df401 -> 80.0 / zoom]
    float mag = 1.0f;
    if ((def_flags & 2) != 0 && scope_max_mag > 1.0f) mag = scope_max_mag;
    const float f = player_view_scope_fraction(v);
    const float scoped = kPlayerCameraFovHDeg / mag;
    return kPlayerCameraFovHDeg + (scoped - kPlayerCameraFovHDeg) * f;
}

float fov_vertical_from_horizontal_deg(float fov_h_deg, float aspect) {
    // [orig: Render_SetViewAndProjectionMatrices @ 0x58d900]
    if (aspect <= 0.0f) return fov_h_deg;
    const float half_h = fov_h_deg * 0.5f * 3.14159265358979323846f / 180.0f;
    const float half_v = std::atan(std::tan(half_h) / aspect);
    return half_v * 2.0f * 180.0f / 3.14159265358979323846f;
}

void player_view_bias_units(const PlayerViewState &v, const float pos[3],
                            const float tpos[3], float out[3]) {
    const float f = player_view_scope_fraction(v);
    for (int i = 0; i < 3; ++i) out[i] = pos[i] + (tpos[i] - pos[i]) * f;
}

} // namespace opennova::world
