// Local player view state -- see player_view.h.
// [orig: CNetPlayerInterp_Setup @ 0x4df36e; ThirdPersonCamera_Update @ 0x437af0;
//  Player_ToggleWeaponScope @ 0x4df0c0..0x4df401]

#include <runtime/world/player_view.h>

#include <cmath>
#include <cassert>

#include <base/io/bam.h>
#include <base/io/fixed.h>

#include <runtime/renderer/aspect_ratio.h>
#include <runtime/renderer/frame_fx_effects.h>
#include <runtime/renderer/nvg_scope_lens.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/geom.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_look.h>
#include <runtime/world/tp_camera_mount.h>
#include <runtime/world/world.h>

namespace opennova::world {

// The six lanes share one active latch. In particular, an increasing-angle
// lane can snap on its first tick while a position lane keeps the pose active.
// [orig: CNetPlayerInterp_Setup @0x4DDFD0]
void player_view_bias_interp_setup(PlayerViewBiasInterp &interp, uint32_t steps,
                                  const PlayerViewPose &idle_source,
                                  const PlayerViewPose &target) {
    interp.target = target;
    const PlayerViewPose &source = interp.active ? interp.current : idle_source;
    double position_delta[3];
    uint32_t rotation_delta[3];
    bool unchanged = true;
    for (int i = 0; i < 3; ++i) {
        position_delta[i] = static_cast<double>(source.position_q16[i]) - target.position_q16[i];
        rotation_delta[i] = source.rotation_bam[i] - target.rotation_bam[i];
        unchanged = unchanged && position_delta[i] == 0.0 && rotation_delta[i] == 0;
    }
    if (unchanged) {
        // Only this idle/zero-delta branch arms the counter. A running
        // zero-delta setup preserves it and the velocities until spent.
        // [orig: @0x4DE0CC..0x4DE11F]
        if (!interp.active) {
            interp.velocity = {};
            interp.active = true;
            interp.remaining = steps;
        } else if (interp.remaining == 0) {
            interp.velocity = {};
            interp.active = false;
        }
        return;
    }
    // Every production caller supplies 1, 7, or 15 steps. Retail's integer
    // division also requires a positive count on this nonzero-delta branch.
    assert(steps != 0);
    const double reciprocal = 1.0 / static_cast<double>(steps);
    for (int i = 0; i < 3; ++i) {
        interp.velocity.position_q16[i] = static_cast<float>(position_delta[i] * reciprocal);
        // This is the witnessed threshold and one's-complement magnitude,
        // not a signed shortest-arc divide. [orig: @0x4DE13C..0x4DE1AA]
        const uint32_t delta = rotation_delta[i];
        interp.velocity.rotation_bam[i] = delta <= 0x7FFFFF80u
                ? delta / steps : 0u - ((0xFFFFFFFFu - delta) / steps);
    }
    if (!interp.active) {
        interp.current = idle_source;
        interp.active = true;
    }
}

void player_view_bias_interp_step(PlayerViewBiasInterp &interp, const PlayerViewPose &hip) {
    if (!interp.active) return;
    if (interp.remaining != 0) --interp.remaining;
    bool moving = false;
    for (int i = 0; i < 3; ++i)
        moving = moving || interp.velocity.position_q16[i] != 0.0f ||
                 interp.velocity.rotation_bam[i] != 0;
    if (!moving) {
        // The active latch clears on the call AFTER the last moving lane
        // snaps. Bias globals retain their last publication on this return.
        // [orig: Player_StepFpViewBiasInterp @0x4DDD47..0x4DDDC3]
        if (interp.remaining == 0) interp.active = false;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        // x87 stores the narrowed current value, then compares the still
        // unrounded subtraction against target. Equality does not snap.
        // [orig: @0x4DDDC4..0x4DDE35]
        const double current = static_cast<double>(interp.current.position_q16[i]) -
                               interp.velocity.position_q16[i];
        interp.current.position_q16[i] = static_cast<float>(current);
        if (std::fabs(interp.velocity.position_q16[i]) >
            std::fabs(current - interp.target.position_q16[i])) {
            interp.velocity.position_q16[i] = 0.0f;
            interp.current.position_q16[i] = interp.target.position_q16[i];
        }
        // The stored rotation words AND velocity are converted as unsigned
        // integers. A negative modular velocity therefore has a large
        // magnitude and can snap immediately. Preserve that retail quirk.
        // [orig: @0x4DDE37..0x4DDF3F]
        interp.current.rotation_bam[i] -= interp.velocity.rotation_bam[i];
        const double distance = std::fabs(static_cast<double>(interp.current.rotation_bam[i]) -
                                          interp.target.rotation_bam[i]);
        if (static_cast<double>(interp.velocity.rotation_bam[i]) > distance) {
            interp.velocity.rotation_bam[i] = 0;
            interp.current.rotation_bam[i] = interp.target.rotation_bam[i];
        }
    }
    // [orig: @0x4DDF42..0x4DDFC3] float difference -> truncating ftol for
    // position, modular subtraction for rotation; both are relative to hip.
    for (int i = 0; i < 3; ++i) {
        interp.position_bias_q16[i] = static_cast<int32_t>(
                static_cast<double>(interp.current.position_q16[i]) - hip.position_q16[i]);
        interp.rotation_bias_bam[i] = io::bam_sub(
                static_cast<int32_t>(interp.current.rotation_bam[i]),
                static_cast<int32_t>(hip.rotation_bam[i]));
    }
}

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

void player_view_apply_view_action(PlayerViewState &v, uint32_t *input_action_bits,
                                   int action) {
    uint32_t bit = 0;
    bool chase = false;
    switch (action) {
        case kViewActionFirstPerson: bit = 0x4000000u; break;
        case kViewActionWithGun: bit = 0x10000000u; break;
        case kViewActionChase: bit = 0x8000000u; chase = true; break;
        default: return;
    }
    if (input_action_bits != nullptr) *input_action_bits |= bit;
    player_view_set_third_person_selected(v, chase);
}

void player_view_chase_action(PlayerViewState &v, int action, uint32_t *input_action_bits) {
    // [orig: Input_HandleActionBinding cases 405..410 @0x49c10c..0x49c249;
    //  every case first tests the debug pager toggle dword_A895A0, clear]
    uint32_t bit = 0;
    switch (action) {
        case 405: bit = 0x10u; break; // [orig: @0x49c119]
        case 406: bit = 0x40u; break; // [orig: @0x49c132]
        case 407:
        case 408: {
            // The step, then the clamp read UNSIGNED: up to 0x80000000 caps at
            // 0x40000000, above it floors at 0xD0000000 [orig: @0x49c14b..0x49c190].
            uint32_t pitch = static_cast<uint32_t>(v.chase_orbit_pitch) +
                    (action != 407 ? 0x800000u : 0xFF800000u);
            if (pitch <= 0x80000000u) {
                if (pitch > 0x40000000u) pitch = 0x40000000u;
            } else if (pitch < 0xD0000000u) {
                pitch = 0xD0000000u;
            }
            v.chase_orbit_pitch = static_cast<int32_t>(pitch);
            bit = action != 407 ? 0x4u : 0x100u; // [orig: @0x49c1a3]
            break;
        }
        case 409: {
            // [orig: @0x49c1c5..0x49c203]
            int32_t step = v.chase_distance_q16 >> 6;
            if (step < 2048) step = 2048;
            v.chase_distance_q16 -= step;
            if (v.chase_distance_q16 < 0x8000) v.chase_distance_q16 = 0x8000;
            bit = 0x80u;
            break;
        }
        case 410: {
            // [orig: @0x49c20f..0x49c249]
            int32_t step = v.chase_distance_q16 >> 6;
            if (step < 2048) step = 2048;
            v.chase_distance_q16 += step;
            if (v.chase_distance_q16 > 0x2000000) v.chase_distance_q16 = 0x2000000;
            bit = 0x200u;
            break;
        }
        default: return;
    }
    if (input_action_bits != nullptr) *input_action_bits |= bit;
}

void player_view_chase_orbit_look(PlayerViewState &v, int32_t yaw_delta, int32_t pitch_delta) {
    // [orig: sub_52AD50 — the pitch @0x52ae4f..0x52ae86, the yaw @0x52ae93]
    int32_t pitch = io::bam_add(v.chase_orbit_pitch, pitch_delta);
    if (pitch > kLookPitchMax) pitch = kLookPitchMax;
    else if (pitch < kLookPitchMin) pitch = kLookPitchMin;
    v.chase_orbit_pitch = pitch;
    v.chase_orbit_yaw = io::bam_add(v.chase_orbit_yaw, yaw_delta);
}

void player_view_track_entity(PlayerViewState &v, uint32_t tracked, bool mode_changed,
                              bool tracked_dead) {
    if (tracked != v.camera_tracked) {
        // [orig: Camera_SetTrackedEntity @0x439201..0x43921d]
        v.camera_tracked = tracked;
        v.chase_orbit_yaw = 0;
        v.chase_orbit_pitch = kTpTrackedOrbitPitchBam;
        v.chase_distance_q16 = kTpTrackedDistanceQ16;
    }
    // [orig: @0x43925f..0x439275 — `test byte [tracked+24h], 2`]
    if (mode_changed && tracked_dead) v.chase_distance_q16 = kTpDeadTargetDistanceQ16;
}

void player_view_chase_tick(PlayerViewState &v, uint32_t input_action_bits, bool tracked_dead) {
    // [orig: ThirdPersonCamera_Update @0x437c1b..0x437c27]
    if ((input_action_bits & 0x10u) != 0) v.chase_orbit_yaw = io::bam_sub(v.chase_orbit_yaw, 0x1000000);
    if ((input_action_bits & 0x40u) != 0) v.chase_orbit_yaw = io::bam_add(v.chase_orbit_yaw, 0x1000000);
    if (!tracked_dead) return;
    // [orig: @0x437cc0..0x437d02]
    if (v.chase_distance_q16 > 458752) {
        v.chase_distance_q16 -= 0x10000;
    } else if (v.chase_distance_q16 > 196608) {
        const int32_t excess = v.chase_distance_q16 - 196608;
        if (excess <= 16) v.chase_distance_q16 = kTpTrackedDistanceQ16;
        else v.chase_distance_q16 -= excess >> 4;
    }
}

namespace {

// CNetPlayerInterp_Setup(&g_FpCameraInterp, steps, idle_source, target) on
// the bound def's two poses: the caller names retail's source and destination
// pointers (the hip copy +0x10C or the tpos +0x124); an active interp sources
// from its own pose regardless. The caller stores the hipfire latch beside it.
// [orig: CNetPlayerInterp_Setup @0x4DDFD0; caller latch stores @0x4DF373]
void scope_interp_setup(PlayerViewState &v, int32_t steps, const PlayerViewPose &idle_source,
                        const PlayerViewPose &target) {
    player_view_bias_interp_setup(v.weapon_pose_interp, static_cast<uint32_t>(steps),
                                  idle_source, target);
}

} // namespace

void player_view_scope_reset(PlayerViewState &v) {
    v.scope_engaged = false;
    v.scope_settled = false;
    v.scope_hipfire = true;
    v.weapon_pose_interp = {};
    v.weapon_pose_bound = false;
}

void player_view_weapon_switch_reset(PlayerViewState &v) {
    v.scope_engaged = false;
    v.scope_hipfire = true;
    PlayerViewBiasInterp &interp = v.weapon_pose_interp;
    for (int i = 0; i < 3; ++i) {
        interp.velocity.position_q16[i] = 0.0f;
        interp.current.position_q16[i] = 0.0f;
        interp.target.position_q16[i] = 0.0f;
        interp.current.rotation_bam[i] = interp.velocity.rotation_bam[i];
    }
    interp.active = false;
    v.weapon_pose_bound = false; // caller rebinds an optical equipped slot
}

void player_view_weapon_mount(PlayerViewState &v, int32_t flags, bool category_changed) {
    if ((flags & 0x20000000) != 0) v.scope_settled = true;
    else if (category_changed) v.scope_settled = false;
    // Mount clears only the six published biases, retaining the running pose.
    // [orig: Player_MountWeaponSlot @0x4DFBCF..0x4DFBE8]
    for (int i = 0; i < 3; ++i) {
        v.weapon_pose_interp.position_bias_q16[i] = 0;
        v.weapon_pose_interp.rotation_bias_bam[i] = 0;
    }
    v.weapon_pose_bound = (flags & (3 | 0x04000000)) != 0;
    if ((flags & 3) != 0 && v.scope_settled) {
        // [orig: g_ScopeEngaged = 1 @0x4dfc5b; Setup(1, +0x10C hip, +0x124 tpos)
        //  @0x4dfc7d; g_ScopeHipfire = 0 @0x4dfc83]
        v.scope_engaged = true;
        scope_interp_setup(v, 1, v.weapon_hip_pose, v.weapon_ads_pose);
        v.scope_hipfire = false;
    }
}

void player_view_tick(PlayerViewState &v, const float eye[3]) {
    // The mode first: the arbiter precedes the camera work every frame
    // [orig: Render_ProcessMainSceneFrame @ 0x5ca1d2, ahead of the view build].
    player_view_resolve_mode(v);
    // The scope-camera interp steps only while active, then THE SETTLE
    // PROMOTER fires on the call that drops the latch [orig: Player_UpdatePerFrame
    // -- `if (!activeFlag) goto done` @0x4de4c7; Player_StepFpViewBiasInterp
    // @0x4de4c9; `if (!activeFlag)` @0x4de4d9 -> g_WeaponScopeActive =
    // (g_ScopeEngaged != 0) @0x4de4f7]. The stepper itself deactivates on the
    // call after the last moving lane snapped, or at once for an unbound slot
    // [orig: Player_StepFpViewBiasInterp null slot/Def @0x4DDD2B..0x4DDDBC].
    if (v.weapon_pose_interp.active) {
        if (v.weapon_pose_bound)
            player_view_bias_interp_step(v.weapon_pose_interp, v.weapon_hip_pose);
        else
            v.weapon_pose_interp.active = false;
        if (!v.weapon_pose_interp.active) v.scope_settled = v.scope_engaged;
    }

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
            // The look-ahead is not eased here: it belongs to every compose,
            // not to the tick (compose_chase_camera).
            for (int i = 0; i < 3; ++i) {
                v.tp_anchor_q16[i] = mount_anchor_ease_q16(
                        v.tp_anchor_q16[i], target[i],
                        i == 2 ? kMountAnchorEaseShiftZ : kMountAnchorEaseShiftXY);
                v.tp_anchor[i] = static_cast<float>(from_fixed(v.tp_anchor_q16[i]));
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
    // Derived readout (player_view.h): no retail counterpart.
    const PlayerViewBiasInterp &interp = v.weapon_pose_interp;
    if (!interp.active) return v.scope_hipfire ? 0.0f : 1.0f;
    const auto clamp01 = [](double f) { return static_cast<float>(f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f); };
    for (int i = 0; i < 3; ++i) {
        const double span = static_cast<double>(v.weapon_ads_pose.position_q16[i]) -
                            v.weapon_hip_pose.position_q16[i];
        if (span == 0.0) continue;
        return clamp01((static_cast<double>(interp.current.position_q16[i]) -
                        v.weapon_hip_pose.position_q16[i]) / span);
    }
    for (int i = 0; i < 3; ++i) {
        const int32_t span = io::bam_sub(static_cast<int32_t>(v.weapon_ads_pose.rotation_bam[i]),
                                         static_cast<int32_t>(v.weapon_hip_pose.rotation_bam[i]));
        if (span == 0) continue;
        const int32_t progress = io::bam_sub(static_cast<int32_t>(interp.current.rotation_bam[i]),
                                             static_cast<int32_t>(v.weapon_hip_pose.rotation_bam[i]));
        return clamp01(static_cast<double>(progress) / static_cast<double>(span));
    }
    return v.scope_hipfire ? 1.0f : 0.0f; // a zero-span ease sits at its source
}

bool player_view_scope_ease_active(const PlayerViewState &v) {
    // [orig: g_FpCameraInterp.activeFlag, tested @ 0x4df177]
    return v.weapon_pose_interp.active;
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
        // [orig: g_WeaponScopeActive = 0 @0x4df31d; g_ScopeEngaged = 1 @0x4df323;
        //  Setup 7 @0x4df355 / 15 @0x4df36e from the hip copy (+0x10C) to tpos
        //  (+0x124); g_ScopeHipfire = 0 @0x4df373]
        v.scope_settled = false;
        v.scope_engaged = true;
        scope_interp_setup(v, full, v.weapon_hip_pose, v.weapon_ads_pose);
        v.scope_hipfire = false;
    } else {
        // [orig: Setup 1 @0x4df1c3 (hipfire return) / 7 @0x4df1e8 / 15 @0x4df201
        //  from tpos to the hip copy; g_ScopeEngaged = 0 @0x4df206;
        //  g_WeaponScopeActive = 0 @0x4df20c; g_ScopeHipfire = 1 @0x4df212]
        const int32_t steps = v.scope_hipfire ? kScopeEaseStepsHipfire : full;
        scope_interp_setup(v, steps, v.weapon_ads_pose, v.weapon_hip_pose);
        v.scope_engaged = false;
        v.scope_settled = false;
        v.scope_hipfire = true;
    }
    return true;
}

bool player_view_move_input(PlayerViewState &v, bool move_held, int32_t def_flags) {
    // [orig: Player_PackInputStateToEntity @ 0x4df450 — g_MovementKeyHeld = 1 while any
    //  of the four direction keys is down @ 0x4df4bb, = 0 otherwise @ 0x4df4f9]
    v.move_held = move_held;
    const bool scoped_def = (def_flags & 1) != 0;
    // Promoted at scope on a Scoped (flags 1) weapon: movement forces the full
    // unscope through the normal toggle [orig: g_WeaponScopeActive gate
    // @ 0x4df4c9 && Def->Flags & 1 @ 0x4df4ea -> Player_ToggleWeaponScope
    // @ 0x4df4ec]. The caller runs it; the legs below are no-ops after it.
    if (move_held && scoped_def && player_view_scope_settled(v)) return true;
    // The entitySlotPtr block: only a Scoped def has the three legs
    // [orig: Def @0x4df50e, Flags & 1 @0x4df52c].
    if (!scoped_def) return false;
    constexpr int32_t kPinnedFlags = 0x20000080; // ForceScoped | Emplaced
    if (move_held) {
        // (a) the running raise reverses toward the hip from its own pose
        // [orig: activeFlag && !g_ScopeHipfire @0x4df548 -> Setup(15, pos,
        //  hip copy) @0x4df567; g_ScopeHipfire = 1 @0x4df56c].
        if (player_view_scope_ease_active(v) && !v.scope_hipfire) {
            scope_interp_setup(v, kScopeEaseSteps, v.weapon_hip_pose, v.weapon_hip_pose);
            v.scope_hipfire = true;
        }
        // The pinned defs skip to LABEL_33, whose own term refuses them
        // [orig: @0x4df57c..0x4df58e].
        if ((def_flags & kPinnedFlags) != 0) return false;
        // (b) settled drop with the promoted byte kept
        // [orig: g_WeaponScopeActive && !activeFlag && !g_ScopeHipfire @0x4df5ae
        //  -> Setup(15, tpos, hip copy) @0x4df5d1; g_ScopeHipfire = 1 @0x4df5d6].
        if (player_view_scope_settled(v) && !player_view_scope_ease_active(v) &&
            !v.scope_hipfire) {
            scope_interp_setup(v, kScopeEaseSteps, v.weapon_ads_pose, v.weapon_hip_pose);
            v.scope_hipfire = true;
        }
        return false;
    }
    // (c) LABEL_33: the auto re-raise on key release
    // [orig: g_WeaponScopeActive && !activeFlag && g_ScopeHipfire &&
    //  !(flags & 0x20000080) @0x4df607 -> g_WeaponScopeActive = 0 @0x4df609;
    //  g_ScopeEngaged = 1 @0x4df60f; Setup(15, hip copy, tpos) @0x4df636;
    //  g_ScopeHipfire = 0 @0x4df63b].
    if (player_view_scope_settled(v) && !player_view_scope_ease_active(v) && v.scope_hipfire &&
        (def_flags & kPinnedFlags) == 0) {
        v.scope_settled = false;
        v.scope_engaged = true;
        scope_interp_setup(v, kScopeEaseSteps, v.weapon_hip_pose, v.weapon_ads_pose);
        v.scope_hipfire = false;
    }
    return false;
}

bool player_view_scope_up_blocked(const PlayerViewState &v, int32_t def_flags) {
    // [orig: the engage leg refuses while the movement latch is held on a
    //  Scoped weapon — g_MovementKeyHeld && (scope_flags & 1) -> return @ 0x4df29c]
    return v.move_held && (def_flags & 1) != 0;
}

// [orig: Player_OnDamageReceived @0x4DD880]
void player_on_damage_received(World &world, const RadarSource &source,
                               const int32_t pos_q16[3]) {
    LocalPlayer *local = world.local_player_state;
    if (local == nullptr) return;
    screen_flash_add_red(local->view.flash, kScreenFlashRedArm); // [orig: @0x4dd88f..0x4dd896]
    camera_shake_arm(local->view.shake, kShakeArmDamageReceived); // [orig: @0x4dd8a6..0x4dd8ad]
    // The radar damage blip [orig: Radar_AddBlip @0x59b280, called @0x4dd8ee].
    radar_add_blip(world, source.id, pos_q16, radar_damage_kind(source));
    // The per-player-slot words unk_26C77A0[100 * (shadowSlot1 & 0x7FFF)]
    // +11 = 6 / +12 = 10 [orig: @0x4dd907..0x4dd916] stay unported: they have
    // no witnessed consumer.
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
    // The PRNG word is shifted into a SIGNED BAM angle. The x87 stack
    // preserves that angle across the first ftol: yaw uses sin, pitch cos,
    // each truncated at Q22 BEFORE multiplication by eight.
    // [orig: Binoculars_RandomizeSwayOffsets @0x4dd830..0x4dd874]
    constexpr double q22 = 4194304.0;
    // Retail's dbl_7C3608 is about 30.5 ppm above exact 2pi / 2^32.
    constexpr double radians_per_bam = 1.4629627251502471e-9;
    const double signed_bam = (unit_random >= 0.5f ? unit_random - 1.0 : unit_random) * 4294967296.0;
    const double angle = signed_bam * radians_per_bam;
    const int32_t yaw_q22 = static_cast<int32_t>(std::sin(angle) * q22);
    const int32_t pitch_q22 = static_cast<int32_t>(std::cos(angle) * q22);
    // The rendered mission yaw is 90 - retail BAM heading.
    yaw_offset_deg = static_cast<float>(-yaw_q22 * (kBinocularAimOffsetDeg / q22));
    pitch_offset_deg = static_cast<float>(pitch_q22 * (kBinocularAimOffsetDeg / q22));
}

void player_view_update_effective_modes(PlayerViewState &v, bool alive, bool round_ended) {
    // Raw intent survives every temporary suppression. Third person only
    // suppresses the optical view: remote observers still see the raised pose.
    v.binoculars_raised =
        v.binoculars_requested && alive && !round_ended && !v.movement_input;
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
    // Every NVG world/post leg gates on the resolved mode word being first
    // person, so the death lerp camera (4) drops the treatment too.
    // [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c81c4..0x5c8205;
    //  Render_TerrainScene @ 0x610cfc..0x610d10;
    //  Render_ProcessMainSceneFrame @ 0x5ca6ab..0x5ca6bf]
    return v.nvg_active && v.camera_mode == 0;
}

float player_view_fov_h_deg(const PlayerViewState &v, int32_t current_fov_q16,
                           bool scoped, bool sighted, int32_t zoom) {
    if (v.binoculars_view_active) return kBinocularCameraFovHDeg;
    // ftol truncates the optical division back to Q16 before projection.
    // Guard malformed zero/negative zoom instead of a floating divide by zero.
    if (zoom < 1) zoom = 1;
    int32_t fov = current_fov_q16;
    // The equipped-slot sway rides each optical arm here in retail and is
    // unported (see the header): the SIGHTED arm applies it only behind the
    // WeaponDef +0x84 word [orig: @0x5ca452..0x5ca465], the SCOPED arm applies
    // it unconditionally with a slot equipped [orig: @0x5ca496..0x5ca4a0].
    if (sighted) fov = (80 << 16) / zoom;   // [orig: @0x5ca42b..0x5ca449]
    else if (scoped) fov /= zoom;           // [orig: @0x5ca472..0x5ca490]
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

ViewProjection nvg_view_projection(const ViewProjection &frame,
                                   const renderer::FrameFxNvgPlan &nvg,
                                   float selected_h_over_w, int32_t zoom) {
    ViewProjection out = frame;
    const int side = renderer::kNvgSceneSide;
    if (nvg.lens) {
        const float fov =
            static_cast<float>(renderer::nvg_scoped_scene_fov_q16(selected_h_over_w, zoom)) /
            io::kFp16One;
        out.fov_h_deg = fov;
        out.fov_v_deg = fov;
        out.aspect = 1.0f;
        out.target_w = side;
        out.target_h = side;
        return out;
    }
    if (nvg.sighted) {
        out.fov_h_deg =
            static_cast<float>(renderer::nvg_sighted_scene_fov_q16(zoom)) / io::kFp16One;
        out.fov_v_deg = fov_vertical_from_horizontal_deg(out.fov_h_deg, out.aspect);
    }
    // The frame-shaped arms rasterise the 512 square as well: 512 columns
    // across fov_h and 512 rows across fov_v, the frame's aspect riding the
    // projection's vertical scale (non-square texels)
    // [orig: NVG_RenderScene @0x5d2954..0x5d296d; NVG_RenderSightedScene @0x5d2aa9..0x5d2ada].
    out.target_w = side;
    out.target_h = side;
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

void player_view_bias_units(const PlayerViewState &v, const float pos[3], float out[3]) {
    // The published Q16 bias (interp_current - hip copy, truncated) is in the
    // def's *256 scale; back in file units that is bias / 256.
    for (int i = 0; i < 3; ++i)
        out[i] = pos[i] + static_cast<float>(v.weapon_pose_interp.position_bias_q16[i]) /
                                  kWeaponDefPosScale;
}

void player_view_bias_view_units(const PlayerViewState &v, bool suppress_bias,
                                 const float pos[3], float out[3]) {
    if (suppress_bias) {
        // The NoCardSwitch reload rule drops the published bias for the frame
        // (instant, not eased) — the presented viewmodel returns to the hip
        // offset, the ported reading of retail's skipped camera-bias add
        // [orig: @ 0x4dd439/@ 0x4dd4cc].
        for (int i = 0; i < 3; ++i) out[i] = pos[i] / kWeaponDefPosScale;
        return;
    }
    player_view_bias_units(v, pos, out);
    for (int i = 0; i < 3; ++i) out[i] /= kWeaponDefPosScale;
}

float player_view_fp_pitch_recoil_deg(int32_t recoil_pitch_bam) {
    // [orig: Camera_ComputeThirdPersonView @ 0x437fc7 — pitch += 2 * recoil]
    return static_cast<float>(
            static_cast<double>(io::bam_dbl(recoil_pitch_bam)) *
            kDegreesPerBam);
}

float player_view_fp_roll_deg(int32_t torso_roll_bam, int32_t lean_bam) {
    // [orig: @ 0x437fe6 — g_ViewRotRoll = entity+0x2DC + (entity+0xB0 >> 2)]
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

// THE CHASE (mode 1). One path serves the on-foot orbit and the control-seat
// arm (mount state +0x168 in {2, 5}): the arms choose the seed angles, the
// distance and the anchor; the pivot, the collision-march landing, the
// clearances and the look-at are shared.
// [orig: Camera_ComputeThirdPersonView @0x437D10 — mode 1 @0x4380E4..0x438650,
//  the look-at @0x4387DF..0x43892D]
void compose_chase_camera(PlayerViewState &v, const float position[3],
                          const float person_eye[3],
                          const terrain::TerrainHeightField *terrain, bool indoors,
                          float aim_yaw_deg, float aim_pitch_deg, bool march_candidates,
                          PlayerCameraPose &out) {
    const MountedCameraInput &m = v.mount;
    const bool mounted = m.control_seat;
    const float r = m.bound_radius;
    const bool have_terrain = terrain != nullptr && terrain->valid();

    // On foot: the entity's own yaw and pitch plus the orbit, backed off the
    // chase distance [orig: @0x4380FD..0x438119]. Mounted: the carrier's
    // heading plus a QUARTER of the rider's look offset as the arithmetic BAM
    // shift [orig: @0x438138..0x43814A]; the fixed downward -11.25 pitch,
    // ASSIGNED rather than added [orig: mov esi, 0F8000000h @0x438150];
    // 1.0 + 1.5 r back [orig: @0x438121..0x438136].
    // The orbit yaw adds to the BAM heading, so the mission yaw (90 -
    // heading) takes it negated [orig: `add eax, g_CameraOrbitYaw` @0x438109,
    // `add esi, g_CameraOrbitPitch` @0x43810F, g_CameraChaseDistance @0x438100].
    double yaw_deg = static_cast<double>(aim_yaw_deg) -
                     static_cast<double>(v.chase_orbit_yaw) * kDegreesPerBam;
    double pitch_deg = static_cast<double>(aim_pitch_deg) +
                       static_cast<double>(v.chase_orbit_pitch) * kDegreesPerBam;
    float distance = static_cast<float>(from_fixed(v.chase_distance_q16));
    // The anchor: the chased anchor, else (before the first tick seeds it)
    // the live eye, or the carrier lifted when mounted.
    float anchor[3] = {person_eye[0], person_eye[1], person_eye[2]};
    if (mounted) {
        const int32_t aim_bam = bam_heading_from_mission_yaw_deg(aim_yaw_deg);
        const int32_t yaw_bam = mount_look_yaw_bam(
                m.carrier_yaw_bam,
                m.carrier_yaw_bam + io::bam_sub(aim_bam, m.carrier_yaw_bam));
        yaw_deg = mission_yaw_deg_from_bam_heading(yaw_bam);
        pitch_deg = kMountPitchDeg;
        distance = mount_distance(r);
        anchor[0] = static_cast<float>(from_fixed(m.carrier_pos_q16[0]));
        anchor[1] = static_cast<float>(from_fixed(m.carrier_pos_q16[1]));
        anchor[2] = static_cast<float>(from_fixed(m.carrier_pos_q16[2])) +
                    mount_anchor_lift(r);
    }
    if (v.tp_anchor_valid) {
        anchor[0] = v.tp_anchor[0];
        anchor[1] = v.tp_anchor[1];
        anchor[2] = v.tp_anchor[2];
    }

    // The view matrix takes the ANCHOR as its translation [orig: the Euler
    // build @0x438179]. Its image of (0x2000, 0x2000, 0x2000) is the pivot:
    // the LOOK-AT TARGET, never the eye [orig: @0x43817E..0x4381D9].
    float fwd[3], left[3], up[3];
    view_axes_mission(yaw_deg, pitch_deg, fwd, left, up);
    float pivot[3];
    for (int i = 0; i < 3; ++i)
        pivot[i] = anchor[i] + (fwd[i] + left[i] + up[i]) * kTpPivotNudge;

    // The eye: the same matrix's image of (-distance, 0, 0) [orig:
    // @0x4383E0..0x4383FB]. The collision march runs only below 8.0 u with
    // proximity candidates at hand [orig: the +0x1C0 count @0x4381CB, the
    // gate @0x4381E3]; without a bone-collision force the eye stays on the
    // last 0.25 step it reached [orig: the no-force exit @0x438334..0x438341],
    // and with one step or none it stays on the pivot [orig: @0x43821F]. The
    // force pull-in itself is the tracked net-re §5.39 deferral.
    float eye[3];
    if (march_candidates && distance < kTpMarchGate) {
        const float landed = player_view_tp_effective_distance(distance);
        for (int i = 0; i < 3; ++i)
            eye[i] = landed > 0.0f ? anchor[i] - fwd[i] * landed : pivot[i];
    } else {
        for (int i = 0; i < 3; ++i) eye[i] = anchor[i] - fwd[i] * distance;
    }

    // The clearances, water then terrain, for every chase eye [orig:
    // @0x438409..0x438456]: the water floor only while the entity itself sits
    // above water + 0.25; the terrain floor skipped indoors (Flags & 0x800000
    // -> the height-0 plane).
    eye[2] = raise_above_water(eye[2], m.water_z, position[2]);
    if (have_terrain) {
        const float ground = terrain_height_at(terrain, eye[0], eye[1]);
        eye[2] = raise_above_terrain(eye[2], ground, indoors);
    } else if (indoors) {
        eye[2] = raise_above_terrain(eye[2], 0.0f, true);
    }

    float target[3] = {pivot[0], pivot[1], pivot[2]};
    if (mounted) {
        // The slope raise [orig: @0x43846E..0x438619]: the xy unit from the
        // anchor toward the eye (zero when degenerate), a march of half-unit
        // steps up to the distance keeping the steepest rise-over-run with the
        // running max SEEDED AT 0.0 (@0x438599 — a downhill run never lowers
        // the floor), then `eye.z = max(eye.z, anchor.z + maxSlope * dist +
        // 0.333 * dist)` @0x4385f8..0x438619.
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
        // look-at [orig: @0x43861D..0x43864C, itemDef+0x196 in {3,4} — the
        // eye z and var_AC (the look-at z) both lose boundRadius >> 1].
        const float drop = m.watercraft ? watercraft_eye_drop(r) : 0.0f;
        eye[2] -= drop;
        target[2] -= drop;

        // The look-ahead eases a thirty-second per axis on EVERY compose —
        // the logic quantum's and the rendered frame's — and rides the target
        // [orig: g_camera_lookahead += (target - lookahead + 16) >> 5
        //  @0x43885A..0x4388AF; added onto the look-at deltas @0x43887B /
        //  @0x43888F / @0x4388A0].
        for (int i = 0; i < 3; ++i) {
            v.lookahead_q16[i] += (m.lookahead_target_q16[i] - v.lookahead_q16[i] + 16) >> 5;
            target[i] += static_cast<float>(from_fixed(v.lookahead_q16[i]));
        }
    }

    // The final angles look from the eye at the target through fpatan
    // [orig: yaw @0x4388B5..0x4388D4, pitch @0x4388D9..0x43892D, roll 0
    //  @0x438933].
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

void player_view_compose_camera(PlayerViewState &v,
                                const float position[3],
                                const float anchor_eye[3], bool anchor_valid,
                                const terrain::TerrainHeightField *terrain,
                                bool indoors,
                                float aim_yaw_deg, float aim_pitch_deg,
                                int32_t recoil_pitch_bam,
                                int32_t torso_roll_bam, int32_t lean_bam,
                                bool march_candidates, float entity_roll_deg,
                                PlayerCameraPose &out) {
    // Mode 4: the death lerp camera — the composed FROM/TO poses against the
    // view tick, nothing of the FP/TP legs below [orig: the g_CameraMode == 4
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
    // CameraOffset was already posed and terrain-floored by the body motor.
    // The camera consumes it directly; only the non-person leg adds 1 unit.
    // [orig: Camera_ComputeThirdPersonView @0x437E8F / @0x437FA5..0x437FB7]
    float eye[3];
    if (anchor_valid) {
        eye[0] = anchor_eye[0];
        eye[1] = anchor_eye[1];
        eye[2] = anchor_eye[2];
    } else {
        eye[0] = position[0];
        eye[1] = position[1];
        eye[2] = position[2] + kNonPersonEyeBump;
    }
    out.third_person = v.third_person;
    if (v.third_person) {
        compose_chase_camera(v, position, eye, terrain, indoors, aim_yaw_deg, aim_pitch_deg,
                             march_candidates, out);
        return;
    }
    if (!anchor_valid) {
        // A non-person entity: the +1.0 bump over Position under its own
        // rotation triple, then straight to the tail — none of the person leg
        // runs [orig: the def type test @0x437E89, the bump @0x437E8F and the
        //  jump to the shake @0x437E99; the triple @0x437D86..0x437D9B].
        out.yaw_deg = aim_yaw_deg;
        out.pitch_deg = aim_pitch_deg;
        out.roll_deg = entity_roll_deg;
        for (int i = 0; i < 3; ++i) out.eye[i] = eye[i];
        return;
    }
    // [orig: mode 0, the person leg @ 0x437f9c..0x438031 — the ground-entity
    //  leg's latched lift clears first (dword_A89140 = 0 @0x437F9C), pitch adds
    //  the doubled recoil, roll composes torso+lean, then the eye pulls back
    //  along the full view rotation (forward is roll-invariant). A seated
    //  person whose seat neither carrier leg admits lands here too.]
    v.ground_leg_lift_q16 = 0;
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
