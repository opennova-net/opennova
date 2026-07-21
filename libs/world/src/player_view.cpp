// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include "world/player_view.h"

#include <cmath>

namespace opennova::world {

void player_view_tick(PlayerViewState &v, const float eye[3]) {
    // The scope-camera ease, one step per tick toward the engaged target within
    // the ease length this toggle latched. [orig: CNetPlayerInterp steps —
    // 15 @ 0x4df36e / 7 Inset @ 0x4df355 / 1 hipfire-return @ 0x4df1c3]
    v.scope_step += v.scope_engaged ? 1 : -1;
    if (v.scope_step < 0) v.scope_step = 0;
    if (v.scope_step > v.ease_steps) v.scope_step = v.ease_steps;

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
    if (v.ease_steps <= 0) return v.scope_engaged ? 1.0f : 0.0f;
    return static_cast<float>(v.scope_step) / static_cast<float>(v.ease_steps);
}

bool player_view_scope_ease_active(const PlayerViewState &v) {
    // Mid-ease = the step has not reached the engaged target's endpoint.
    // [orig: g_fpCameraInterp.activeFlag, tested @ 0x4df177]
    return v.scope_engaged ? (v.scope_step < v.ease_steps) : (v.scope_step > 0);
}

bool player_view_set_engaged(PlayerViewState &v, bool engaged, bool inset_weapon) {
    if (engaged == v.scope_engaged) return true;
    // Every toggle is refused while the previous ease still runs.
    // [orig: the !activeFlag gate @ 0x4df177 — both directions]
    if (player_view_scope_ease_active(v)) return false;
    const int32_t full = inset_weapon ? kScopeEaseStepsInset : kScopeEaseSteps;
    if (engaged) {
        // [orig: Setup 15 @ 0x4df36e / 7 @ 0x4df355; g_scopeHipfire = 0 @ 0x4df373]
        v.ease_steps = full;
        v.scope_step = 0;
        v.scope_hipfire = false;
    } else {
        // [orig: Setup 1 @ 0x4df1c3 (hipfire return) / 7 @ 0x4df1e8 / 15 @ 0x4df201;
        //  g_scopeHipfire = 1 @ 0x4df212]
        v.ease_steps = v.scope_hipfire ? kScopeEaseStepsHipfire : full;
        v.scope_step = v.ease_steps;
        v.scope_hipfire = true;
    }
    v.scope_engaged = engaged;
    return true;
}

bool player_view_move_input(PlayerViewState &v, bool move_held, int32_t def_flags) {
    // [orig: Player_PackInputStateToEntity @ 0x4df450 — byte_B7653B = 1 while any
    //  of the four direction keys is down @ 0x4df4bb, = 0 otherwise @ 0x4df4f9]
    v.move_held = move_held;
    if (!move_held) return false;
    // Settled at scope on a Scoped (flags 1) weapon: movement forces the full
    // unscope through the normal toggle [orig: g_weaponScopeActive gate
    // @ 0x4df4c9 (only ever 1 once the ease completed — the @ 0x4de4f7 promoter)
    // && Def->Flags & 1 @ 0x4df4ea -> Player_ToggleWeaponScope @ 0x4df4ec].
    return (def_flags & 1) != 0 && v.scope_engaged && !player_view_scope_ease_active(v);
}

bool player_view_scope_up_blocked(const PlayerViewState &v, int32_t def_flags) {
    // [orig: the engage leg refuses while the movement latch is held on a
    //  Scoped weapon — byte_B7653B && (scope_flags & 1) -> return @ 0x4df29c]
    return v.move_held && (def_flags & 1) != 0;
}

bool player_view_toggle_binoculars(PlayerViewState &v) {
    v.binoculars_requested = !v.binoculars_requested;
    if (!v.binoculars_requested) {
        // Effective states normally update once per simulation tick, but a
        // toggle-off must not leave one frame of stale optics/body pose.
        v.binoculars_raised = false;
        v.binoculars_view_active = false;
    }
    return v.binoculars_requested;
}

void player_view_update_effective_modes(PlayerViewState &v, bool alive, bool round_ended) {
    // Raw intent survives every temporary suppression. Third person only
    // suppresses the optical view: remote observers still see the raised pose.
    v.binoculars_raised =
        v.binoculars_requested && alive && !round_ended && !v.move_held;
    v.binoculars_view_active = v.binoculars_raised && !v.third_person;
}

bool player_view_toggle_nvg(PlayerViewState &v) {
    v.nvg_active = !v.nvg_active;
    return v.nvg_active;
}

int32_t player_view_adjust_nvg_gain(PlayerViewState &v, int32_t delta) {
    // Widen before addition so an arbitrary caller-provided delta cannot
    // overflow before the clamp.
    const int64_t adjusted = static_cast<int64_t>(v.nvg_gain) + delta;
    if (adjusted < kNvgGainMin) v.nvg_gain = kNvgGainMin;
    else if (adjusted > kNvgGainMax) v.nvg_gain = kNvgGainMax;
    else v.nvg_gain = static_cast<int32_t>(adjusted);
    return v.nvg_gain;
}

bool player_view_nvg_visible(const PlayerViewState &v) {
    return v.nvg_active && !v.third_person;
}

float player_view_fov_h_deg(const PlayerViewState &v, int32_t def_flags, float scope_max_mag) {
    // Effective-mode refresh guarantees this optical view is first-person.
    if (v.binoculars_view_active) return kBinocularCameraFovHDeg;
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
