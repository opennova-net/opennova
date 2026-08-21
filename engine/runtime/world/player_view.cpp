// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include "world/player_view.h"

#include <cmath>

#include <io/bam.h>

#include "terrain_query/height_field.h"
#include "world/angle.h"
#include "world/geom.h"
#include "world/tp_camera_mount.h"

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
            for (int i = 0; i < 3; ++i) v.tp_anchor_q16[i] = to_fixed(eye[i]);
            v.tp_anchor_valid = true;
        } else if (v.mount.control_seat) {
            // MOUNTED: the anchor chases the CARRIER position lifted
            // max(1.0, 0.375 r) — not Position + CameraOffset — a sixteenth
            // per tick on x/y and a thirty-second on z, in 16.16 with the
            // half-step rounding [orig: ThirdPersonCamera_Update — the
            // parentSlot 2/5 target @0x437B1F..0x437B4B, `(target - anchor +
            // 8) >> 4` @0x437C56/@0x437C6A and `(+ 16) >> 5` @0x437C79].
            const int32_t target[3] = {
                v.mount.carrier_pos_q16[0],
                v.mount.carrier_pos_q16[1],
                v.mount.carrier_pos_q16[2] +
                        mount_anchor_lift_q16(to_fixed(v.mount.bound_radius)),
            };
            for (int i = 0; i < 3; ++i) {
                v.tp_anchor_q16[i] = mount_anchor_ease_q16(
                        v.tp_anchor_q16[i], target[i],
                        i == 2 ? kMountAnchorEaseShiftZ : kMountAnchorEaseShiftXY);
                v.tp_anchor[i] = static_cast<float>(from_fixed(v.tp_anchor_q16[i]));
            }
            // The look-ahead offset eases a thirty-second per axis toward the
            // carrier's forward x 6.0 [orig: `g_camera_lookahead += (target -
            // lookahead + 16) >> 5` @0x438811..0x4388b5, target = parentMatrix
            // x (6, 0, 0), rotation only].
            for (int i = 0; i < 3; ++i) {
                const int32_t ahead = to_fixed(
                        static_cast<double>(v.mount.carrier_forward[i]) *
                        kMountLookaheadDistance);
                v.lookahead_q16[i] += (ahead - v.lookahead_q16[i] + 16) >> 5;
            }
        } else {
            // Quarter-step ease per 62 Hz tick. [orig: @ 0x437c8d]
            v.tp_anchor[0] += (eye[0] - v.tp_anchor[0]) * kTpAnchorEase;
            v.tp_anchor[1] += (eye[1] - v.tp_anchor[1]) * kTpAnchorEase;
            v.tp_anchor[2] += (eye[2] - v.tp_anchor[2]) * kTpAnchorEase;
            for (int i = 0; i < 3; ++i) v.tp_anchor_q16[i] = to_fixed(v.tp_anchor[i]);
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
    // [orig: Player_PackInputStateToEntity @ 0x4df450 — g_movementKeyHeld = 1 while any
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
    //  Scoped weapon — g_movementKeyHeld && (scope_flags & 1) -> return @ 0x4df29c]
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

// The heightmap sample the mounted clearances and the slope march read, in
// the engine ground frame (x, y) -> the renderer atlas (x, -y) mapping the
// head-bone floor uses.
float terrain_height_at(const terrain::TerrainHeightField *terrain, float x, float y) {
    return terrain::height_field_height_world_bilinear(*terrain, x, -y);
}

// THE MOUNTED THIRD-PERSON LEG [orig: the mount-state 2/5 arm of mode 1 in
// Camera_ComputeThirdPersonView @0x437D10].
void compose_mounted_camera(const PlayerViewState &v, const float position[3],
                            const terrain::TerrainHeightField *terrain,
                            bool indoors, float aim_yaw_deg,
                            PlayerCameraPose &out) {
    const MountedCameraInput &m = v.mount;
    const float r = m.bound_radius;
    const bool have_terrain = terrain != nullptr && terrain->valid();

    // Yaw: the carrier's heading plus a QUARTER of the rider's look offset,
    // as the arithmetic BAM shift [orig: @0x438138..0x43814A].
    const int32_t aim_bam = bam_heading_from_mission_yaw_deg(aim_yaw_deg);
    const int32_t yaw_bam = mount_look_yaw_bam(
            m.carrier_yaw_bam,
            m.carrier_yaw_bam + io::bam_sub(aim_bam, m.carrier_yaw_bam));
    const double yaw_deg = mission_yaw_deg_from_bam_heading(yaw_bam);
    // Pitch: the fixed downward -11.25, ASSIGNED — the on-foot arm adds the
    // orbit pitch to the entity's, the mounted arm replaces it
    // [orig: mov esi, 0F8000000h @0x438150 vs the on-foot sum].
    const double pitch_deg = kMountPitchDeg;

    // The anchor: the eased mounted anchor (carrier + lift), or the carrier
    // lifted directly before the first tick seeds it.
    float anchor[3];
    if (v.tp_anchor_valid) {
        anchor[0] = v.tp_anchor[0];
        anchor[1] = v.tp_anchor[1];
        anchor[2] = v.tp_anchor[2];
    } else {
        anchor[0] = static_cast<float>(from_fixed(m.carrier_pos_q16[0]));
        anchor[1] = static_cast<float>(from_fixed(m.carrier_pos_q16[1]));
        anchor[2] = static_cast<float>(from_fixed(m.carrier_pos_q16[2])) +
                    mount_anchor_lift(r);
    }

    // The eye: the anchor backed off 1.0 + 1.5 r along the view forward
    // [orig: @0x438121..0x438136].
    float fwd[3];
    view_axes_mission(yaw_deg, pitch_deg, fwd, nullptr, nullptr);
    const float distance = mount_distance(r);
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = anchor[i] - fwd[i] * distance;

    // The clearances, water then terrain [orig: @0x438409..0x438456]: the
    // water floor only while the entity itself sits above water + 0.25; the
    // terrain floor skipped indoors (Flags & 0x800000 -> the height-0 plane).
    eye[2] = raise_above_water(eye[2], m.water_z, position[2]);
    if (have_terrain) {
        const float ground = terrain_height_at(terrain, eye[0], eye[1]);
        eye[2] = raise_above_terrain(eye[2], ground, indoors);
    } else if (indoors) {
        eye[2] = raise_above_terrain(eye[2], 0.0f, true);
    }

    // The slope raise [orig: @0x43846E..0x438619]: the xy unit from the
    // anchor toward the eye (zero when degenerate), a march of half-unit
    // steps up to the distance keeping the steepest rise-over-run with the
    // running max SEEDED AT 0.0 (@0x438599 — a downhill run never lowers the
    // floor), then `eye.z = max(eye.z, anchor.z + maxSlope * dist + 0.333 *
    // dist)` @0x4385f8..0x438619.
    if (have_terrain) {
        const float dx = eye[0] - anchor[0];
        const float dy = eye[1] - anchor[1];
        const float horizontal = std::sqrt(dx * dx + dy * dy);
        if (horizontal > 0.0f) {
            const float ux = dx / horizontal;
            const float uy = dy / horizontal;
            float max_slope = 0.0f;
            for (float run = kSlopeStep; run <= horizontal; run += kSlopeStep) {
                const float h = terrain_height_at(terrain, anchor[0] + ux * run,
                                                  anchor[1] + uy * run);
                const float slope = (h - anchor[2]) / run;
                if (slope > max_slope) max_slope = slope;
            }
            const float floor_z = slope_raise_floor(anchor[2], horizontal, max_slope);
            if (eye[2] < floor_z) eye[2] = floor_z;
        }
    }

    // The watercraft drop: half the carrier radius off the eye AND the
    // look-at [orig: @0x43861D..0x43864C, itemDef+0x196 in {3,4} — the eye z
    // and var_AC (the look-at z) both lose boundRadius >> 1].
    const float drop = m.watercraft ? watercraft_eye_drop(r) : 0.0f;
    eye[2] -= drop;

    // The look-at point: the anchor plus the eased look-ahead offset (the
    // carrier's forward x 6.0, integrated by the tick), with the drop applied;
    // the final angles are the look-at from the eye to it via atan
    // [orig: @0x438811..0x4388b5 — the eased g_camera_lookahead added to the
    //  eye accumulators, yaw via fpatan].
    float target[3] = {
        anchor[0] + static_cast<float>(from_fixed(v.lookahead_q16[0])),
        anchor[1] + static_cast<float>(from_fixed(v.lookahead_q16[1])),
        anchor[2] + static_cast<float>(from_fixed(v.lookahead_q16[2])),
    };
    target[2] -= drop;

    const double tx = static_cast<double>(target[0] - eye[0]);
    const double ty = static_cast<double>(target[1] - eye[1]);
    const double tz = static_cast<double>(target[2] - eye[2]);
    const double flat = std::sqrt(tx * tx + ty * ty);
    out.yaw_deg = flat > 0.0
            ? static_cast<float>(normalize_mission_yaw_deg(std::atan2(tx, ty) / kRadPerDeg))
            : static_cast<float>(yaw_deg);
    out.pitch_deg = static_cast<float>(std::atan2(tz, flat) / kRadPerDeg);
    out.roll_deg = 0.0f;
    out.eye[0] = eye[0];
    out.eye[1] = eye[1];
    out.eye[2] = eye[2];
    out.third_person = true;
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

void player_view_motion_lead_update(PlayerViewMotionLead &lead,
                                    const float tick_delta_units[3],
                                    int32_t out_lead_q16[3]) {
    // [orig: @0x437bac..0x437c0e — all three lanes compute the SAME recurrence
    // vel += (prev - new - vel + 16) >> 5 with prev_sample = (tick position
    // delta) << 8; the x lane merely SCHEDULES it differently (its fresh
    // sample is computed inline, so the compiler folds `- new` into the final
    // add) — an instruction-order artifact, not a per-lane asymmetry.]
    for (int i = 0; i < 3; ++i) {
        // << via uint32: the original's 32-bit shl wraps; signed << would be UB.
        const int32_t sample = static_cast<int32_t>(
                static_cast<uint32_t>(static_cast<int32_t>(
                        static_cast<double>(tick_delta_units[i]) * 65536.0))
                << 8);
        lead.vel[i] +=
                (lead.prev_sample[i] - sample - lead.vel[i] + 16) >> 5;
        lead.prev_sample[i] = sample;
    }
    // [orig: the >> 7 + clamps @ 0x4dd4f2..0x4dd54f]
    for (int i = 0; i < 3; ++i) {
        int32_t v = lead.vel[i] >> 7;
        const int32_t clamp = i == 2 ? kFpLeadClampZ : kFpLeadClampXy;
        if (v > clamp) v = clamp;
        if (v < -clamp) v = -clamp;
        out_lead_q16[i] = v;
    }
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
    // over Position, or the non-person +1.0 bump. The 0x2000-equivalent floor
    // is a DEFENSIVE stand-in on this leg: retail floors only the sample-less
    // capsule leg (retail: @ 0x4b6b98) — the head-bone legs store unfloored
    // (on-foot @ 0x4b6bb3..0x4b6cc8, mounted @ 0x4b6908..0x4b696c; D-INF-18).
    // [orig: the bump @ 0x437e8f]
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
    if (v.third_person && v.mount.control_seat) {
        // The control-seat arm of mode 1 (world/tp_camera_mount.h).
        compose_mounted_camera(v, position, terrain, indoors, aim_yaw_deg, out);
        return;
    }
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

// [orig: the mode-0 shake block inside Camera_ComputeThirdPersonView
//  @0x437D10, the block @0x43803c..0x4380df — the counter gate, the > 64
//  clamp, the three IIR updates and the three >> 6 applications]
void camera_shake_sample(CameraShakeState &st, uint32_t weather_prng,
                         int32_t &d_yaw_bam, int32_t &d_pitch_bam,
                         int32_t &d_roll_bam) {
	d_yaw_bam = 0;
	d_pitch_bam = 0;
	d_roll_bam = 0;
	// Retail gates the ENTIRE block on a non-zero counter, so a settled camera
	// leaves the filters exactly where they stopped rather than decaying them
	// toward zero in the background.
	if (st.counter == 0) return;

	int32_t clamped = st.counter;
	if (clamped > kShakeSampleMax) clamped = kShakeSampleMax;

	// Three slices of ONE word. The shifts are arithmetic on int32 so the sign
	// propagates, which is what makes each slice signed noise rather than a
	// positive-only magnitude.
	const int32_t p = static_cast<int32_t>(weather_prng);
	const int32_t n_roll = static_cast<int32_t>(static_cast<uint32_t>(p) << 1) >> 5;
	const int32_t n_pitch = static_cast<int32_t>(static_cast<uint32_t>(p) << 17) >> 5;
	const int32_t n_yaw = static_cast<int32_t>(static_cast<uint32_t>(p) << 9) >> 5;

	// s = (7s + n) >> 3 — a 1/8 one-pole low-pass, computed through uint32 so
	// the intermediate wraps like the original imul/add rather than tripping
	// signed-overflow UB.
	auto step = [](int32_t s, int32_t n) -> int32_t {
		const uint32_t acc = uint32_t(7) * static_cast<uint32_t>(s) +
				static_cast<uint32_t>(n);
		return static_cast<int32_t>(acc) >> 3;
	};
	st.roll = step(st.roll, n_roll);
	st.pitch = step(st.pitch, n_pitch);
	st.yaw = step(st.yaw, n_yaw);

	// (clamped * s) >> 6, the multiply left to WRAP as the original imul does.
	auto delta = [](int32_t c, int32_t s) -> int32_t {
		const uint32_t m = static_cast<uint32_t>(c) * static_cast<uint32_t>(s);
		return static_cast<int32_t>(m) >> 6;
	};
	d_roll_bam = delta(clamped, st.roll);
	d_pitch_bam = delta(clamped, st.pitch);
	d_yaw_bam = delta(clamped, st.yaw);
}

} // namespace opennova::world
