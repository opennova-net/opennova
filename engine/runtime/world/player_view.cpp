// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include "world/player_view.h"

#include <cmath>

#include <io/bam.h>

#include "terrain_query/height_field.h"
#include "world/angle.h"

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

void player_view_binocular_sway_offset(float unit_random,
                                       float &yaw_offset_deg,
                                       float &pitch_offset_deg) {
    constexpr double kTau = 6.28318530717958647692;
    const double angle = static_cast<double>(unit_random) * kTau;
    yaw_offset_deg =
        static_cast<float>(std::cos(angle) * kBinocularAimOffsetDeg);
    pitch_offset_deg =
        static_cast<float>(std::sin(angle) * kBinocularAimOffsetDeg);
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

void player_view_bias_view_units(const PlayerViewState &v, bool suppress_bias,
                                 const float pos[3], const float tpos[3],
                                 float out[3]) {
    if (suppress_bias) {
        // The NoCardSwitch reload rule drops the ADS half for the frame
        // (instant, not eased) — the presented viewmodel returns to the hip
        // offset, the ported reading of retail's skipped camera-bias add
        // [orig: @ 0x4dd439/@ 0x4dd4cc].
        for (int i = 0; i < 3; ++i) out[i] = pos[i] / kWeaponDefPosScale;
        return;
    }
    player_view_bias_units(v, pos, tpos, out);
    for (int i = 0; i < 3; ++i) out[i] /= kWeaponDefPosScale;
}

float player_view_fp_pitch_recoil_deg(int32_t recoil_pitch_bam) {
    // [orig: Camera_ComputeThirdPersonView @ 0x437fc7 — pitch += 2 * recoil]
    return static_cast<float>(
            static_cast<double>(io::bam_dbl(recoil_pitch_bam)) *
            kDegreesPerBam);
}

float player_view_fp_roll_deg(int32_t torso_roll_bam, int32_t lean_bam) {
    // [orig: @ 0x437fe6 — g_view_rot_roll = entity+0x2DC + (entity+0xB0 >> 2)]
    return static_cast<float>(
            static_cast<double>(
                    io::bam_add(torso_roll_bam, io::bam_sar(lean_bam, 2))) *
            kDegreesPerBam);
}

float player_view_tp_effective_distance(float distance) {
    if (distance >= kTpMarchGate) return distance; // [orig: the gate @ 0x4381e9]
    const int steps = static_cast<int>(distance / kTpMarchStep);
    if (steps <= 1) return 0.0f; // [orig: numSteps <= 1 stays at the pivot @ 0x43821f]
    return static_cast<float>(steps - 1) * kTpMarchStep;
}

namespace {

constexpr double kRadPerDeg = 3.14159265358979323846 / 180.0;

// The mission-frame view axes for a yaw/pitch pair (roll spins about forward
// and moves none of these) — the (x, z, -y) godot conversion of the presented
// look basis, kept in one place so both camera legs and the pivot nudge agree.
void view_axes_mission(double yaw_deg, double pitch_deg, float fwd[3],
                       float left[3], float up[3]) {
    const double sy = std::sin(yaw_deg * kRadPerDeg);
    const double cy = std::cos(yaw_deg * kRadPerDeg);
    const double sp = std::sin(pitch_deg * kRadPerDeg);
    const double cp = std::cos(pitch_deg * kRadPerDeg);
    fwd[0] = static_cast<float>(sy * cp);
    fwd[1] = static_cast<float>(cy * cp);
    fwd[2] = static_cast<float>(sp);
    if (left != nullptr) {
        left[0] = static_cast<float>(-cy);
        left[1] = static_cast<float>(sy);
        left[2] = 0.0f;
    }
    if (up != nullptr) {
        up[0] = static_cast<float>(-sp * sy);
        up[1] = static_cast<float>(-sp * cy);
        up[2] = static_cast<float>(cp);
    }
}

} // namespace

void player_view_floor_eye_to_terrain(const terrain::TerrainHeightField *terrain,
                                      bool indoors, float eye[3]) {
    // [orig: Entity_UpdateInfantryPlayerBody — the Flags & 0x800000 INDOORS
    //  skip @ 0x4b6c08/@ 0x4b6c16 (the heightmap has no interiors)]
    if (terrain == nullptr || !terrain->valid() || indoors) return;
    // Engine ground plane is (x, y); the renderer-loaded atlas samples in
    // Godot coords (x, -y) — the calc_average_ground_height mapping.
    const auto sample = [&](float x, float y) {
        return terrain::height_field_height_world_bilinear(*terrain, x, -y) +
               kEyeTerrainClearance; // [orig: each sample + 0x1000 @ 0x4b6c2d]
    };
    // The eye column, then ±0x4000 along each ground axis, max-folded
    // [orig: @ 0x4b6c1e / @ 0x4b6c33 / @ 0x4b6c4e / @ 0x4b6c69 / @ 0x4b6c84].
    float floor_z = sample(eye[0], eye[1]);
    const float r = kEyeTerrainProbeRadius;
    const float probes[4][2] = {{r, 0.0f}, {-r, 0.0f}, {0.0f, r}, {0.0f, -r}};
    for (const float *p : probes) {
        const float h = sample(eye[0] + p[0], eye[1] + p[1]);
        if (h > floor_z) floor_z = h;
    }
    // [orig: the eye-Z max @ 0x4b6c9e..0x4b6ca2]
    if (eye[2] < floor_z) eye[2] = floor_z;
}

void player_view_compose_camera(const PlayerViewState &v,
                                const float position[3],
                                const float anchor_eye[3], bool anchor_valid,
                                const terrain::TerrainHeightField *terrain,
                                bool indoors,
                                float aim_yaw_deg, float aim_pitch_deg,
                                int32_t recoil_pitch_bam,
                                int32_t torso_roll_bam, int32_t lean_bam,
                                PlayerCameraPose &out) {
    // The eye anchor: the shell-fed head-bone eye floored kEyeMinAbovePosition
    // over Position, or the non-person +1.0 bump.
    // [orig: the 0x2000 floor @ 0x4b6b98; the bump @ 0x437e8f]
    float eye[3];
    if (anchor_valid) {
        eye[0] = anchor_eye[0];
        eye[1] = anchor_eye[1];
        eye[2] = anchor_eye[2] < position[2] + kEyeMinAbovePosition
                ? position[2] + kEyeMinAbovePosition
                : anchor_eye[2];
        // The D-INF-18 terrain floor rides only the head-bone eye path
        // [orig: @ 0x4b6c08..0x4b6ca4 — the fallback branch @ 0x4b6b92 has no
        //  terrain leg].
        player_view_floor_eye_to_terrain(terrain, indoors, eye);
    } else {
        eye[0] = position[0];
        eye[1] = position[1];
        eye[2] = position[2] + kNonPersonEyeBump;
    }
    out.third_person = v.third_person;
    if (v.third_person) {
        // [orig: mode 1 @ 0x438100..0x4383e2 — the nudged pivot backs off the
        //  march-landed distance along the orbit forward; the final rotation is
        //  the look-at back to the pivot, equal to the seed angles with no
        //  march collision ported (net-re §5.39)]
        out.yaw_deg = aim_yaw_deg;
        out.pitch_deg = aim_pitch_deg + kTpOrbitPitchDeg;
        out.roll_deg = 0.0f;
        const float *anchor = v.tp_anchor_valid ? v.tp_anchor : eye;
        float fwd[3], left[3], up[3];
        view_axes_mission(out.yaw_deg, out.pitch_deg, fwd, left, up);
        const float back = player_view_tp_effective_distance(kTpDistance);
        for (int i = 0; i < 3; ++i) {
            const float pivot =
                    anchor[i] + (fwd[i] + left[i] + up[i]) * kTpPivotNudge;
            out.eye[i] = pivot - fwd[i] * back;
        }
        return;
    }
    // [orig: mode 0, the on-foot person leg @ 0x437f9c..0x438031 — pitch adds
    //  the doubled recoil, roll composes torso+lean, then the eye pulls back
    //  along the full view rotation (forward is roll-invariant)]
    out.yaw_deg = aim_yaw_deg;
    out.pitch_deg =
            aim_pitch_deg + player_view_fp_pitch_recoil_deg(recoil_pitch_bam);
    out.roll_deg = player_view_fp_roll_deg(torso_roll_bam, lean_bam);
    float fwd[3];
    view_axes_mission(out.yaw_deg, out.pitch_deg, fwd, nullptr, nullptr);
    for (int i = 0; i < 3; ++i) out.eye[i] = eye[i] - fwd[i] * kFpEyePullback;
}

} // namespace opennova::world
