// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include <runtime/world/player_view.h>

#include <cmath>

#include <base/io/bam.h>

#include <runtime/renderer/aspect_ratio.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/geom.h>
#include <runtime/world/tp_camera_mount.h>

namespace opennova::world {

void player_view_resolve_mode(PlayerViewState &v) {
    // [orig: Render_ProcessMainSceneFrame @ 0x5ca1d2..0x5ca24b]
    int desired = 0;
    // @0x5ca1da the preference byte; @0x5ca1e2..0x5ca1f2 parentSlot 2 / 5.
    if (v.third_person_selected && v.mount.control_seat) desired = 1;
    if (v.death_screen_active) {
        // @0x5ca1fd..0x5ca215: sub-mode 0 -> 0, 1 -> 1, 2 -> 0, else keep.
        switch (v.death_screen_submode) {
            case 0: desired = 0; break;
            case 1: desired = 1; break;
            case 2: desired = 0; break;
            default: break;
        }
    } else if (v.local_dead || (v.round_ended && v.on_foot)) {
        // @0x5ca217 Flags & 2; @0x5ca21d..0x5ca22b the spawn-success gate with
        // parentEntity == 0; @0x5ca242 rules bit 0 keeps the seat mode.
        if (!v.rules_no_death_cam) desired = 4;
    } else if (v.in_session && v.rules_force_first_person) {
        desired = 0; // @0x5ca22d..0x5ca23e
    }
    v.camera_mode = v.debug_third_person_on_foot && desired == 0 ? 1 : desired;
    v.third_person = desired == 1 || v.debug_third_person_on_foot;
}

void player_view_set_third_person_selected(PlayerViewState &v, bool selected) {
    v.third_person_selected = selected;
    player_view_resolve_mode(v);
}

namespace {

// The interp's target pose along the hip..tpos line, read back from the
// hipfire latch (player_view.h: every Setup stores the latch beside its
// target, hip <-> 1 and tpos <-> 0).
float scope_target_step(const PlayerViewState &v) {
    return v.scope_hipfire ? 0.0f : static_cast<float>(v.ease_steps);
}

// CNetPlayerInterp_Setup @0x4ddfd0, projected onto the line: an ACTIVE interp
// sources from its own current pose (@0x4de006..0x4de01a), an idle one from
// the caller's pose (@0x4de01f..0x4de033); the per-step velocity is
// delta / steps (@0x4de142..0x4de15a), carried here as the step count left
// over the current delta (the same poses per step, an exact landing). A zero
// delta against an active interp whose counter is spent deactivates it outright
// (@0x4de0fe..0x4de11b) -- here the pose already sits on the latch's target,
// so the ease simply reads idle -- and against an idle one arms `steps`
// zero-velocity frames (@0x4de0d8..0x4de0f8), unreachable on this line where
// every idle Setup spans the two endpoints. The caller stores the hipfire latch
// (the target) right after, as every retail site does.
void scope_interp_setup(PlayerViewState &v, int32_t steps, float idle_source_fraction) {
    const float from =
        player_view_scope_ease_active(v) ? player_view_scope_fraction(v) : idle_source_fraction;
    v.ease_steps = steps;
    v.scope_step = from * static_cast<float>(steps);
    v.scope_ease_remaining = steps;
}

// Player_StepFpViewBiasInterp @0x4ddd20, one step: the pose moves one velocity
// toward the target and snaps onto it once within a velocity of it
// (@0x4dddc4..0x4ddf3f). Returns true on the landing step.
bool scope_interp_step(PlayerViewState &v) {
    const float target = scope_target_step(v);
    if (v.scope_step == target) return false;
    if (v.scope_ease_remaining > 1) {
        v.scope_step = target + (v.scope_step - target) *
                                    static_cast<float>(v.scope_ease_remaining - 1) /
                                    static_cast<float>(v.scope_ease_remaining);
        --v.scope_ease_remaining;
        return false;
    }
    v.scope_step = target;
    v.scope_ease_remaining = 0;
    return true;
}

} // namespace

void player_view_scope_reset(PlayerViewState &v) {
    v.scope_engaged = false;
    v.scope_settled = false;
    v.scope_hipfire = true;
    v.ease_steps = kScopeEaseSteps;
    v.scope_step = 0.0f;
    v.scope_ease_remaining = 0;
}

void player_view_tick(PlayerViewState &v, const float eye[3]) {
    // The mode first: the arbiter precedes the camera work every frame
    // [orig: Render_ProcessMainSceneFrame @ 0x5ca1d2, ahead of the view build].
    player_view_resolve_mode(v);
    // The promoted byte never outlives its target in this port (player_view.h
    // player_view_scope_settled): a reset that wrote only the legacy fields
    // (the local_player / player_weapon reset sites) is completed here.
    if (!v.scope_engaged) v.scope_settled = false;
    // The scope-camera interp, one step per tick toward the latch's target,
    // then THE SETTLE PROMOTER on the landing step [orig: Player_UpdatePerFrame
    // @0x4de4c9 Player_StepFpViewBiasInterp -> @0x4de4f7 g_weaponScopeActive =
    // (g_scopeEngaged != 0) once the interp reports done -- the step after the
    // six velocities snapped; the landing step is that completion here].
    if (scope_interp_step(v)) v.scope_settled = v.scope_engaged;

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
    // Mid-ease = the pose has not reached the latch's target.
    // [orig: g_fpCameraInterp.activeFlag, tested @ 0x4df177]
    return v.scope_step != scope_target_step(v);
}

bool player_view_scope_request_pending(const PlayerViewState &v, bool engaged) {
    if (engaged != v.scope_engaged) return true;
    // The reversed-at-hip quirk: the target latched, the interp idle, nothing
    // promoted -- retail's promoted-byte branch @0x4df17f takes the engage leg.
    return engaged && !player_view_scope_settled(v) && !player_view_scope_ease_active(v);
}

bool player_view_set_engaged(PlayerViewState &v, bool engaged, bool inset_weapon) {
    if (!player_view_scope_request_pending(v, engaged)) return true;
    // Every toggle is refused while the previous ease still runs.
    // [orig: the !activeFlag gate @ 0x4df177 — both directions]
    if (player_view_scope_ease_active(v)) return false;
    const int32_t full = inset_weapon ? kScopeEaseStepsInset : kScopeEaseSteps;
    if (engaged) {
        // [orig: g_weaponScopeActive = 0 @0x4df31d; g_scopeEngaged = 1 @0x4df323;
        //  Setup 7 @0x4df355 / 15 @0x4df36e from the hip copy (+0x10C) to tpos
        //  (+0x124); g_scopeHipfire = 0 @0x4df373]
        v.scope_settled = false;
        v.scope_engaged = true;
        scope_interp_setup(v, full, 0.0f);
        v.scope_hipfire = false;
    } else {
        // [orig: Setup 1 @0x4df1c3 (hipfire return) / 7 @0x4df1e8 / 15 @0x4df201
        //  from tpos to the hip copy; g_scopeEngaged = 0 @0x4df206;
        //  g_weaponScopeActive = 0 @0x4df20c; g_scopeHipfire = 1 @0x4df212]
        const int32_t steps = v.scope_hipfire ? kScopeEaseStepsHipfire : full;
        scope_interp_setup(v, steps, 1.0f);
        v.scope_engaged = false;
        v.scope_settled = false;
        v.scope_hipfire = true;
    }
    return true;
}

bool player_view_move_input(PlayerViewState &v, bool move_held, int32_t def_flags) {
    // [orig: Player_PackInputStateToEntity @ 0x4df450 — g_movementKeyHeld = 1 while any
    //  of the four direction keys is down @ 0x4df4bb, = 0 otherwise @ 0x4df4f9]
    v.move_held = move_held;
    const bool scoped_def = (def_flags & 1) != 0;
    // Promoted at scope on a Scoped (flags 1) weapon: movement forces the full
    // unscope through the normal toggle [orig: g_weaponScopeActive gate
    // @ 0x4df4c9 && Def->Flags & 1 @ 0x4df4ea -> Player_ToggleWeaponScope
    // @ 0x4df4ec]. The caller runs it; the legs below are no-ops after it.
    if (move_held && scoped_def && player_view_scope_settled(v)) return true;
    // The entitySlotPtr block: only a Scoped def has the three legs
    // [orig: Def @0x4df50e, Flags & 1 @0x4df52c].
    if (!scoped_def) return false;
    constexpr int32_t kPinnedFlags = 0x20000080; // ForceScoped | Emplaced
    if (move_held) {
        // (a) the running raise reverses toward the hip from its own pose
        // [orig: activeFlag && !g_scopeHipfire @0x4df548 -> Setup(15, pos,
        //  hip copy) @0x4df567; g_scopeHipfire = 1 @0x4df56c].
        if (player_view_scope_ease_active(v) && !v.scope_hipfire) {
            scope_interp_setup(v, kScopeEaseSteps, 0.0f);
            v.scope_hipfire = true;
        }
        // The pinned defs skip to LABEL_33, whose own term refuses them
        // [orig: @0x4df57c..0x4df58e].
        if ((def_flags & kPinnedFlags) != 0) return false;
        // (b) settled drop with the promoted byte kept
        // [orig: g_weaponScopeActive && !activeFlag && !g_scopeHipfire @0x4df5ae
        //  -> Setup(15, tpos, hip copy) @0x4df5d1; g_scopeHipfire = 1 @0x4df5d6].
        if (player_view_scope_settled(v) && !player_view_scope_ease_active(v) &&
            !v.scope_hipfire) {
            scope_interp_setup(v, kScopeEaseSteps, 1.0f);
            v.scope_hipfire = true;
        }
        return false;
    }
    // (c) LABEL_33: the auto re-raise on key release
    // [orig: g_weaponScopeActive && !activeFlag && g_scopeHipfire &&
    //  !(flags & 0x20000080) @0x4df607 -> g_weaponScopeActive = 0 @0x4df609;
    //  g_scopeEngaged = 1 @0x4df60f; Setup(15, hip copy, tpos) @0x4df636;
    //  g_scopeHipfire = 0 @0x4df63b].
    if (player_view_scope_settled(v) && !player_view_scope_ease_active(v) && v.scope_hipfire &&
        (def_flags & kPinnedFlags) == 0) {
        v.scope_settled = false;
        v.scope_engaged = true;
        scope_interp_setup(v, kScopeEaseSteps, 0.0f);
        v.scope_hipfire = false;
    }
    return false;
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

float player_view_fov_h_deg(const PlayerViewState &v, int32_t current_fov_q16,
                           bool scoped, bool sighted, int32_t zoom) {
    if (v.binoculars_view_active) return kBinocularCameraFovHDeg;
    // ftol truncates the optical division back to Q16 before projection.
    // Guard malformed zero/negative zoom instead of a floating divide by zero.
    if (zoom < 1) zoom = 1;
    int32_t fov = current_fov_q16;
    if (sighted) fov = (80 << 16) / zoom;
    else if (scoped) fov /= zoom;
    return static_cast<float>(fov) / 65536.0f;
}

float fov_vertical_from_horizontal_deg(float fov_h_deg, float aspect) {
    // [orig: Render_SetViewAndProjectionMatrices @ 0x58d900]
    if (aspect <= 0.0f) return fov_h_deg;
    const float half_h = fov_h_deg * 0.5f * 3.14159265358979323846f / 180.0f;
    const float half_v = std::atan(std::tan(half_h) / aspect);
    return half_v * 2.0f * 180.0f / 3.14159265358979323846f;
}

ViewProjection view_projection(float fov_h_deg, int aspect_mode, int surface_w,
                               int surface_h) {
    // [orig: Render_SetViewAndProjectionMatrices @0x58d900 -- viewportWidth =
    //  w * 1.0 @0x58d971, viewportHeight = h * flt_8409E8 @0x58d985, halfV =
    //  atan(tan(fov_h/2) * (vh / vw)) @0x58d9b2, aspect = vw / vh @0x58d9be;
    //  flt_8409E8 = selected / (h/w) from Render_SetAspectRatioMode @0x58d8a7]
    ViewProjection out;
    out.fov_h_deg = fov_h_deg;
    out.fov_v_deg = fov_h_deg;
    out.target_w = surface_w;
    out.target_h = surface_h;
    if (surface_w <= 0 || surface_h <= 0) return out;
    const float w = static_cast<float>(surface_w);
    const float h = static_cast<float>(surface_h);
    const float selected = renderer::aspect_height_over_width(aspect_mode, w, h);
    out.scale_y = renderer::aspect_viewport_scale_y(aspect_mode, w, h);
    // vh / vw = (h * scale_y) / w = selected, so the pass is the perspective of
    // aspect 1 / selected, whatever the surface.
    out.aspect = 1.0f / selected;
    out.fov_v_deg = fov_vertical_from_horizontal_deg(fov_h_deg, out.aspect);
    if (std::fabs(out.scale_y - 1.0f) <= 1e-5f) {
        out.scale_y = 1.0f; // the surface's own ratio: no stretch, no resample
    } else if (out.scale_y > 1.0f) {
        // Taller than the surface: keep its width, grow the height.
        out.target_h = static_cast<int>(std::lround(w * selected));
    } else {
        // Wider than the surface: keep its height, grow the width.
        out.target_w = static_cast<int>(std::lround(h / selected));
    }
    return out;
}

float viewmodel_focal_ratio(float world_fov_h_deg, float renderfov_h_deg) {
    // [orig: the two Render_SetViewAndProjectionMatrices calls share scaleY --
    //  the FP pass @0x4dee7f (renderfov) and Render_SetViewProjectionWithDefaults
    //  @0x58f6b0 (the world fov) -- so proj[1][1]_fp / proj[1][1]_world =
    //  tan(fov_h/2) / tan(renderfov/2), and proj[0][0] the same]
    constexpr float kHalfDegToRad = 0.5f * 3.14159265358979323846f / 180.0f;
    const float fp = std::tan(renderfov_h_deg * kHalfDegToRad);
    if (fp <= 0.0f) return 1.0f;
    return std::tan(world_fov_h_deg * kHalfDegToRad) / fp;
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

constexpr double kRadPerDeg = io::kRadiansPerDegree;

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
                                bool carrier_view, float carrier_roll_deg,
                                PlayerCameraPose &out) {
    // Mode 4: the death lerp camera — the composed FROM/TO poses against the
    // view tick, nothing of the FP/TP legs below [orig: the g_camera_mode == 4
    // branch @0x4389eb..0x438b49 returns before the mode 0/1 composition].
    if (v.camera_mode == 4 && v.death_cam.valid) {
        DeathCameraPose pose;
        death_camera_view(v.death_cam, v.view_tick, pose);
        out.eye[0] = static_cast<float>(from_fixed(pose.pos[0]));
        out.eye[1] = static_cast<float>(from_fixed(pose.pos[1]));
        out.eye[2] = static_cast<float>(from_fixed(pose.pos[2]));
        // atan2(y, x) BAM -> the mission view yaw (0 = +Y): 90 - deg.
        out.yaw_deg = static_cast<float>(mission_yaw_deg_from_bam_heading(pose.yaw_bam));
        out.pitch_deg = static_cast<float>(static_cast<double>(pose.pitch_bam) * kDegreesPerBam);
        out.roll_deg = static_cast<float>(static_cast<double>(pose.roll_bam) * kDegreesPerBam);
        out.third_person = true;
        return;
    }
    // The eye anchor: the shell-fed head-bone eye floored kEyeMinAbovePosition
    // over Position, or the non-person +1.0 bump. The 0x2000-equivalent floor
    // is a DEFENSIVE stand-in on this leg: retail floors only the sample-less
    // capsule leg [orig: Entity_UpdateInfantryPlayerBody @0x4b6b98] — the head-bone legs store unfloored
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
    if (carrier_view) {
        // Mounted in a carrier: retail reads the entity rotation triple
        // (yaw/pitch/roll at +16/+20/+24), hands the position to the carrier's
        // own view transform, and jumps to the tail. That jump is the point —
        // it skips the whole person leg, so NONE of the doubled recoil, the
        // torso+lean roll, or the eye pull-back applies while seated, and the
        // eye takes no floor either.
        //
        // Composing the person leg here froze the standing terrain torso-roll
        // (16.9 degrees on the spawn hillside) into the cockpit view for the
        // entire flight, tilting the horizon and skewing the instrument panel.
        // [orig: Camera_ComputeThirdPersonView carrier branch @0x437c5d..
        //  0x437cc2 — gated on carrier def +0x1C0 && +0x174, then goto the tail
        //  past the person leg at 0x437f9c]
        out.yaw_deg = aim_yaw_deg;
        out.pitch_deg = aim_pitch_deg;
        out.roll_deg = carrier_roll_deg;
        for (int i = 0; i < 3; ++i) out.eye[i] = anchor_eye[i];
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

// The five chase-shake frequencies, bit-exact to the retail float pool
// (flt_7C56A0..flt_7C5690). The counter terms scan 0.4 / 2/7 / 2/11 rad per
// count; the tick terms are 25/34 and 0.862069 rad per tick (the latter is
// one ULP off 25/29 — retail's pool holds the typed decimal, not the ratio).
namespace {
inline constexpr float kChaseYawFreq = 0.4f;              // flt_7C56A0 (0x3ECCCCCD)
inline constexpr float kChasePitchFreq = 2.0f / 7.0f;     // flt_7C569C (0x3E924925)
inline constexpr float kChaseRollFreq = 2.0f / 11.0f;     // flt_7C5698 (0x3E3A2E8C)
inline constexpr float kChasePitchTickFreq = 25.0f / 34.0f; // flt_7C5694 (0x3F3C3C3C)
inline constexpr float kChaseRollTickFreq = 0.862069f;    // flt_7C5690 (0x3F5CB08E)
} // namespace

// [orig: Camera_ComputeThirdPersonView @0x437d10, the mode>=1 shake block
//  @0x438939..0x4389e5 — see the header note for the per-term map]
void camera_shake_sample_chase(const CameraShakeState &st, uint32_t weather_prng,
                               uint32_t tick, int32_t &d_yaw_bam,
                               int32_t &d_pitch_bam, int32_t &d_roll_bam) {
	d_yaw_bam = 0;
	d_pitch_bam = 0;
	d_roll_bam = 0;
	// The same whole-block counter gate as the mode-0 leg [orig: @0x43892b].
	if (st.counter == 0) return;

	// amp = (min(4*counter, 255) * ((prng & 0xFF) + 64)) >> 8
	// [orig: @0x43893f..0x438967 — the two add eax,eax, the 0xFF clamp, the
	//  low-byte mask, +64, imul, sar 8]. The SAMPLE clamp here is the STORE cap
	//  255, not the mode-0 leg's 64.
	int32_t quad = st.counter * 4;
	if (quad > kShakeStoreMax) quad = kShakeStoreMax;
	const int32_t amp =
			(quad * ((static_cast<int32_t>(weather_prng) & 0xFF) + 64)) >> 8;

	// x87 evaluates each product in double from the float constants; every
	// term truncates toward zero (ftol) before its integer add/subtract, and
	// the tick terms are quartered by an arithmetic shift AFTER truncation.
	const double c = static_cast<double>(st.counter);
	const double a = static_cast<double>(amp);
	// fild loads the tick dword SIGNED [orig: fild current_tick @0x4389ad].
	const double t = static_cast<double>(static_cast<int32_t>(tick));
	d_yaw_bam = static_cast<int32_t>(
			std::sin(c * static_cast<double>(kChaseYawFreq)) * a);
	d_pitch_bam =
			static_cast<int32_t>(
					std::sin(c * static_cast<double>(kChasePitchFreq)) * a) -
			(static_cast<int32_t>(
					std::sin(t * static_cast<double>(kChasePitchTickFreq)) * a) >> 2);
	// Roll REPLACES a zero base on this path (the look-at composes roll 0), so
	// the delta IS the retail roll.
	d_roll_bam =
			static_cast<int32_t>(
					std::sin(c * static_cast<double>(kChaseRollFreq)) * a) -
			(static_cast<int32_t>(
					std::cos(t * static_cast<double>(kChaseRollTickFreq)) * a) >> 2);
}

} // namespace opennova::world
