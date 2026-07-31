// Aim-overlay unit tests [orig: Entity_BuildBoneTransformMatrices @ 0x4b1290;
// docs/world/world-wac-ai-re.md section 14]: the bone-class map, the exact BAM blend
// formulas of both on-foot branches, and the pose compose invariants (identity deltas =
// passthrough; child origins preserved under any delta; the bend gradient orders spine
// segments between body and aim).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "anim/aim_overlay.h"
#include <io/bam.h>

using namespace opennova::anim;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t kBamDeg = 11930464; // 2^32 / 360, engine-wide [docs/engine-primer.md]

Quat quat_axis_angle(float ax, float ay, float az, float rad) {
    const float s = std::sin(rad * 0.5f);
    return quat_normalize({std::cos(rad * 0.5f), ax * s, ay * s, az * s});
}

float quat_angle_between(Quat a, Quat b) {
    // atan2 on the vector part, not acos(w): acos is ill-conditioned at identity in
    // float32 (one ulp of w below 1.0 reads as ~7e-4 rad), which flips with the platform's
    // FMA contraction. The vector part carries small angles at full precision.
    Quat d = quat_mul(quat_inv(a), b);
    const float v = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * std::atan2(v, std::fabs(d.w));
}

std::vector<Quat> fk_rotations(const std::vector<int> &parents,
                               const std::vector<Quat> &local) {
    std::vector<Quat> world(local.size());
    for (size_t i = 0; i < local.size(); ++i) {
        const int p = parents[i];
        world[i] = (p >= 0 && static_cast<size_t>(p) < i)
                       ? quat_normalize(quat_mul(world[static_cast<size_t>(p)], local[i]))
                       : quat_normalize(local[i]);
    }
    return world;
}

void test_bone_class_map() {
    // The witnessed map, spot-checked at the semantic anchors (section 14.2).
    CHECK(kOverlayClassByBoneIndex[0] == kOverlayBody);        // BN01 Hips = default
    CHECK(kOverlayClassByBoneIndex[1] == kOverlaySpine);       // BN02 Lower Spine
    CHECK(kOverlayClassByBoneIndex[2] == kOverlayUpperSpine);  // BN03 Upper Spine
    CHECK(kOverlayClassByBoneIndex[7] == kOverlayLegR);        // BN08 R Thigh
    CHECK(kOverlayClassByBoneIndex[8] == kOverlayLegL);        // BN09 L Thigh
    CHECK(kOverlayClassByBoneIndex[11] == kOverlayLegR);       // BN12 R Calf
    CHECK(kOverlayClassByBoneIndex[12] == kOverlayLegL);       // BN13 L Calf
    CHECK(kOverlayClassByBoneIndex[13] == kOverlayNeck);       // BN14 Neck
    CHECK(kOverlayClassByBoneIndex[14] == kOverlayHead);       // BN15 Head
    CHECK(kOverlayClassByBoneIndex[16] == kOverlayArm);        // BN17 R Hand
    CHECK(kOverlayClassByBoneIndex[17] == kOverlayLegR);       // BN18 R Foot
    CHECK(kOverlayClassByBoneIndex[18] == kOverlayLegL);       // BN19 L Foot
    // The upper-body dual-channel mask {3,4,5,6,9,10,13,14,15,16} [orig: @ 0x4b14db]
    // must be exactly the non-body, non-leg, non-hips classes -- the map's complement
    // coherence that pinned the bone order.
    for (int i = 0; i < 19; ++i) {
        const bool upper = (i >= 3 && i <= 6) || i == 9 || i == 10 || (i >= 13 && i <= 16);
        const uint8_t c = kOverlayClassByBoneIndex[i];
        const bool leg = (c == kOverlayLegR || c == kOverlayLegL);
        if (upper) CHECK(!leg && c != kOverlayBody);
        if (i >= 7 && (i == 7 || i == 8 || i == 11 || i == 12 || i == 17 || i == 18))
            CHECK(leg);
    }
}

void test_aim_branch_blends() {
    // 40 deg look-up, body lagging 20 deg behind the aim yaw: the witnessed gradient.
    AimOverlayInputs in;
    in.aim_yaw = 60 * kBamDeg;
    in.aim_pitch = 40 * kBamDeg;
    in.body_yaw = 40 * kBamDeg;
    in.body_pitch = 0;
    in.leg_yaw_r = 35 * kBamDeg;
    in.leg_yaw_l = 36 * kBamDeg;
    in.aim_state = true;
    AimOverlayAngles out[kOverlayClassCount];
    compute_aim_overlay_angles(in, out);

    const int32_t A = in.aim_yaw, B = in.body_yaw, P = in.aim_pitch;
    // Body: pure body pair. [orig: @ 0x4b182e]
    CHECK(out[kOverlayBody].yaw == B && out[kOverlayBody].pitch == 0);
    // Spine: half yaw, quarter pitch. [orig: @ 0x4b1cb4]
    CHECK(out[kOverlaySpine].yaw == A + ((B - A) >> 1));
    CHECK(out[kOverlaySpine].pitch == (P >> 2));
    // Upper spine: 3/4 yaw, full pitch. [orig: @ 0x4b1c6d]
    CHECK(out[kOverlayUpperSpine].yaw == A + ((B - A) >> 2));
    CHECK(out[kOverlayUpperSpine].pitch == P);
    // Clavicles + neck are upper-spine copies on foot. [orig: @ 0x4b1c8f/0x4b1caa]
    CHECK(out[kOverlayClavicle].yaw == out[kOverlayUpperSpine].yaw);
    CHECK(out[kOverlayNeck].pitch == out[kOverlayUpperSpine].pitch);
    // Arms: full pitch (plus the zeroed extras). [orig: @ 0x4b1c37]
    CHECK(out[kOverlayArm].pitch == P);
    // Head: the aim pair exactly. [orig: @ 0x4b180a]
    CHECK(out[kOverlayHead].yaw == A && out[kOverlayHead].pitch == P);
    // Legs: the chase yaws with body pitch. [orig: @ 0x4b1845/0x4b185c]
    CHECK(out[kOverlayLegR].yaw == in.leg_yaw_r && out[kOverlayLegR].pitch == 0);
    CHECK(out[kOverlayLegL].yaw == in.leg_yaw_l);
    // The bend gradient is monotone: body <= spine <= upper-spine <= head pitch share.
    CHECK(std::abs(out[kOverlaySpine].pitch) <= std::abs(out[kOverlayUpperSpine].pitch));
    CHECK(std::abs(out[kOverlayUpperSpine].pitch) <= std::abs(out[kOverlayHead].pitch));

    // pitch_blend / pitch_kick_accum terms land where witnessed. [orig: @ 0x4b1bce]
    in.pitch_blend = 4 * kBamDeg;
    in.pitch_kick_accum = 8 * kBamDeg;
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayArm].pitch == P + in.pitch_kick_accum + 2 * in.pitch_blend);
    CHECK(out[kOverlayUpperSpine].pitch == P + in.pitch_blend);
    CHECK(out[kOverlaySpine].pitch == (P >> 2) + (in.pitch_blend >> 1));
}

void test_non_aim_branch() {
    AimOverlayInputs in;
    in.aim_yaw = 90 * kBamDeg;
    in.aim_pitch = -30 * kBamDeg;
    in.body_yaw = 60 * kBamDeg;
    in.pitch_kick_accum = 8 * kBamDeg;
    in.aim_state = false;
    AimOverlayAngles out[kOverlayClassCount];
    compute_aim_overlay_angles(in, out);
    // Spine family rides the body; head keeps full aim; arms track with HLD/4.
    // [orig: @ 0x4b1cf1..0x4b1dfa]
    CHECK(out[kOverlaySpine].yaw == in.body_yaw && out[kOverlaySpine].pitch == 0);
    CHECK(out[kOverlayNeck].yaw == in.body_yaw);
    CHECK(out[kOverlayHead].yaw == in.aim_yaw && out[kOverlayHead].pitch == in.aim_pitch);
    CHECK(out[kOverlayArm].pitch == in.aim_pitch + (in.pitch_kick_accum >> 2));
    in.arms_locked = true; // Flags & 0x100000 [orig: @ 0x4b1d48]
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayArm].yaw == in.body_yaw && out[kOverlayArm].pitch == 0);
}

bool same_angles(const AimOverlayAngles &a, const AimOverlayAngles &b) {
    return a.yaw == b.yaw && a.pitch == b.pitch && a.roll == b.roll;
}

void test_mounted_overlay_matrix_selection() {
    // Mounted matrix selection is one animation-owned table, not a collision
    // heuristic. Use deliberately distinct sources so an accidental copy is visible.
    // [orig: mounted block @ 0x4b1868..0x4b1ba9; target itemDef phrase_set +0x86c]
    using opennova::io::bam_add;
    using opennova::io::bam_dbl;
    using opennova::io::bam_sar;
    using opennova::io::bam_sub;

    AimOverlayInputs in;
    in.aim_yaw = 103 * kBamDeg;
    in.aim_pitch = 37 * kBamDeg;
    in.body_yaw = 41 * kBamDeg;
    in.body_pitch = 11 * kBamDeg;
    in.leg_yaw_r = 29 * kBamDeg;
    in.leg_yaw_l = 31 * kBamDeg;
    in.roll = 3 * kBamDeg;
    in.torso_roll = 5 * kBamDeg;
    in.lean = 7 * kBamDeg;
    in.pitch_blend = 13 * kBamDeg;
    in.aim_state = true;

    const AimOverlayAngles body{in.body_yaw, in.body_pitch, in.roll};
    const AimOverlayAngles aim{
        in.aim_yaw, in.aim_pitch, bam_add(in.torso_roll, bam_sar(in.lean, 1))};
    const AimOverlayAngles leg_r{in.leg_yaw_r, in.body_pitch, in.roll};
    const AimOverlayAngles leg_l{in.leg_yaw_l, in.body_pitch, in.roll};
    const AimOverlayAngles config_zero_arm{
        bam_sub(in.body_yaw, bam_sar(bam_sub(in.aim_yaw, in.body_yaw), 2)),
        bam_add(bam_sar(in.pitch_blend, 1),
                bam_sub(bam_dbl(in.body_pitch), in.aim_pitch)),
        bam_add(in.roll, in.lean)};
    const AimOverlayAngles other_arm{
        bam_sub(in.body_yaw, bam_sar(bam_sub(in.aim_yaw, in.body_yaw), 2)),
        bam_sub(bam_add(in.body_pitch, bam_sar(in.pitch_blend, 1)),
                bam_sar(bam_sub(in.aim_pitch, in.body_pitch), 1)),
        bam_add(in.roll, in.lean)};
    const AimOverlayAngles other_upper{
        bam_sub(in.body_yaw, bam_sar(bam_sub(in.aim_yaw, in.body_yaw), 4)),
        bam_add(in.body_pitch, bam_sar(in.pitch_blend, 1)),
        bam_add(in.roll, bam_sar(in.lean, 1))};

    enum class Recipe { Seated, ConfigZero, OtherNonzero, BodyAll, BodyExceptHead };
    struct Case {
        MountMode mode;
        bool config_valid;
        int32_t config;
        Recipe recipe;
    };
    // Controller slot 2 and Driver slot 5 share the Seated row. Gunner covers every
    // witnessed phrase_set value in the retail 0..8 family, including both counter-
    // lean recipes rather than folding valid zero into the nonzero fallback.
    const std::array<Case, 10> cases{{
        {MountMode::Seated, false, 0, Recipe::Seated},
        {MountMode::Gunner, true, 0, Recipe::ConfigZero},
        {MountMode::Gunner, true, 1, Recipe::OtherNonzero},
        {MountMode::Gunner, true, 2, Recipe::OtherNonzero},
        {MountMode::Gunner, true, 3, Recipe::BodyAll},
        {MountMode::Gunner, true, 4, Recipe::OtherNonzero},
        {MountMode::Gunner, true, 5, Recipe::BodyAll},
        {MountMode::Gunner, true, 6, Recipe::BodyExceptHead},
        {MountMode::Gunner, true, 7, Recipe::BodyAll},
        {MountMode::Gunner, true, 8, Recipe::OtherNonzero},
    }};

    for (const Case &tc : cases) {
        in.mount_mode = tc.mode;
        in.mount_config_valid = tc.config_valid;
        in.mount_config = tc.config;
        AimOverlayAngles out[kOverlayClassCount];
        compute_aim_overlay_angles(in, out);

        CHECK(same_angles(out[kOverlayBody], body));
        CHECK(same_angles(out[kOverlayLegR], leg_r));
        CHECK(same_angles(out[kOverlayLegL], leg_l));
        switch (tc.recipe) {
            case Recipe::Seated:
                CHECK(same_angles(out[kOverlaySpine], body));
                CHECK(same_angles(out[kOverlayUpperSpine], body));
                CHECK(same_angles(out[kOverlayClavicle], body));
                CHECK(same_angles(out[kOverlayArm], body));
                CHECK(same_angles(out[kOverlayNeck], aim));
                CHECK(same_angles(out[kOverlayHead], aim));
                break;
            case Recipe::ConfigZero:
                CHECK(same_angles(out[kOverlaySpine], body));
                CHECK(same_angles(out[kOverlayUpperSpine], body));
                CHECK(same_angles(out[kOverlayClavicle], config_zero_arm));
                CHECK(same_angles(out[kOverlayArm], config_zero_arm));
                CHECK(same_angles(out[kOverlayNeck], aim));
                CHECK(same_angles(out[kOverlayHead], aim));
                break;
            case Recipe::OtherNonzero:
                CHECK(same_angles(out[kOverlaySpine], body));
                CHECK(same_angles(out[kOverlayUpperSpine], other_upper));
                CHECK(same_angles(out[kOverlayClavicle], other_arm));
                CHECK(same_angles(out[kOverlayArm], other_arm));
                CHECK(same_angles(out[kOverlayNeck], aim));
                CHECK(same_angles(out[kOverlayHead], aim));
                break;
            case Recipe::BodyAll:
                CHECK(same_angles(out[kOverlaySpine], body));
                CHECK(same_angles(out[kOverlayUpperSpine], body));
                CHECK(same_angles(out[kOverlayClavicle], body));
                CHECK(same_angles(out[kOverlayArm], body));
                CHECK(same_angles(out[kOverlayNeck], body));
                CHECK(same_angles(out[kOverlayHead], body));
                break;
            case Recipe::BodyExceptHead:
                CHECK(same_angles(out[kOverlaySpine], body));
                CHECK(same_angles(out[kOverlayUpperSpine], body));
                CHECK(same_angles(out[kOverlayClavicle], body));
                CHECK(same_angles(out[kOverlayArm], body));
                CHECK(same_angles(out[kOverlayNeck], body));
                CHECK(same_angles(out[kOverlayHead], aim));
                break;
        }
    }
}

void test_unknown_gunner_config_is_not_valid_config_zero() {
    // Retail always has a target itemDef. The port's absent-metadata state must stay
    // on the existing on-foot branch; only an explicitly valid phrase_set 0 selects
    // the witnessed config-zero counter-lean formula.
    AimOverlayInputs in;
    in.aim_yaw = 90 * kBamDeg;
    in.aim_pitch = 30 * kBamDeg;
    in.body_yaw = 45 * kBamDeg;
    in.body_pitch = 5 * kBamDeg;
    in.pitch_blend = 8 * kBamDeg;
    in.aim_state = true;
    in.mount_mode = MountMode::Gunner;
    in.mount_config_valid = false;
    in.mount_config = 0;
    AimOverlayAngles unknown[kOverlayClassCount];
    compute_aim_overlay_angles(in, unknown);

    in.mount_config_valid = true;
    AimOverlayAngles valid_zero[kOverlayClassCount];
    compute_aim_overlay_angles(in, valid_zero);

    AimOverlayInputs on_foot = in;
    on_foot.mount_mode = MountMode::OnFoot;
    on_foot.mount_config_valid = false;
    AimOverlayAngles expected_on_foot[kOverlayClassCount];
    compute_aim_overlay_angles(on_foot, expected_on_foot);
    for (int i = 0; i < kOverlayClassCount; ++i)
        CHECK(same_angles(unknown[i], expected_on_foot[i]));
    CHECK(!same_angles(unknown[kOverlayArm], valid_zero[kOverlayArm]));
}

void test_rolling_zeroes_roll() {
    AimOverlayInputs in;
    in.roll = 5 * kBamDeg;
    in.torso_roll = 7 * kBamDeg;
    in.lean = 10 * kBamDeg;
    in.rolling = true; // states 41/42 [orig: @ 0x4b17d3]
    in.aim_state = true;
    AimOverlayAngles out[kOverlayClassCount];
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayHead].roll == (in.lean >> 1)); // torsoRoll zeroed, lean survives
    CHECK(out[kOverlayBody].roll == 0);
}

void test_apply_identity_is_passthrough() {
    // A 4-bone chain with arbitrary local rotations: identity deltas must not move it.
    std::vector<int> parents = {-1, 0, 1, 2};
    std::vector<Quat> rot = {
        quat_axis_angle(0, 1, 0, 0.3f),
        quat_axis_angle(1, 0, 0, -0.6f),
        quat_axis_angle(0, 0, 1, 1.1f),
        quat_axis_angle(0, 1, 0, -0.2f),
    };
    std::vector<Quat> orig = rot;
    Quat deltas[kOverlayClassCount];
    for (auto &d : deltas) d = Quat{};
    const uint8_t classes[4] = {kOverlayBody, kOverlaySpine, kOverlayUpperSpine, kOverlayHead};
    apply_aim_overlay(parents, deltas, classes, rot);
    for (size_t i = 0; i < rot.size(); ++i)
        CHECK(quat_angle_between(orig[i], rot[i]) < 1e-4f);
}

void test_apply_world_delta_reaches_target() {
    // With every class given the SAME delta D, each bone's world rotation must become
    // D * world_before -- and locals recompose consistently.
    std::vector<int> parents = {-1, 0, 1};
    std::vector<Quat> rot = {
        quat_axis_angle(0, 1, 0, 0.4f),
        quat_axis_angle(1, 0, 0, 0.7f),
        quat_axis_angle(0, 0, 1, -0.5f),
    };
    // world FK before
    std::vector<Quat> wb(3);
    wb[0] = rot[0];
    wb[1] = quat_mul(wb[0], rot[1]);
    wb[2] = quat_mul(wb[1], rot[2]);
    const Quat D = quat_axis_angle(0, 1, 0, 0.25f);
    Quat deltas[kOverlayClassCount];
    for (auto &d : deltas) d = D;
    const uint8_t classes[3] = {kOverlayBody, kOverlayArm, kOverlayHead};
    apply_aim_overlay(parents, deltas, classes, rot);
    std::vector<Quat> wa(3);
    wa[0] = rot[0];
    wa[1] = quat_mul(wa[0], rot[1]);
    wa[2] = quat_mul(wa[1], rot[2]);
    for (int i = 0; i < 3; ++i)
        CHECK(quat_angle_between(wa[static_cast<size_t>(i)],
                                 quat_mul(D, wb[static_cast<size_t>(i)])) < 1e-4f);
}

void test_apply_differential_bend() {
    // Body identity, head pitched: the head bone's world rotation gains exactly the
    // head delta while the root stays put -- the bend in miniature.
    std::vector<int> parents = {-1, 0};
    std::vector<Quat> rot = {Quat{}, Quat{}};
    Quat deltas[kOverlayClassCount];
    for (auto &d : deltas) d = Quat{};
    const Quat head = quat_axis_angle(1, 0, 0, 0.5f);
    deltas[kOverlayHead] = head;
    const uint8_t classes[2] = {kOverlayBody, kOverlayHead};
    apply_aim_overlay(parents, deltas, classes, rot);
    CHECK(quat_angle_between(rot[0], Quat{}) < 1e-4f);
    CHECK(quat_angle_between(quat_mul(rot[0], rot[1]), head) < 1e-4f);
}

void test_weapon_channel_splices_world_rotations() {
    // Miniature upper-body hierarchy:
    //   unmasked upper spine -> masked clavicle -> masked arm -> unmasked accessory.
    // The two channels deliberately disagree at every local. Selecting secondary
    // locals directly would contaminate the clavicle with the PRIMARY spine and make
    // the accessory inherit the SECONDARY arm. The matrix mask instead selects each
    // bone's absolute channel rotation, then re-localizes the entire mixed hierarchy.
    const std::vector<int> parents = {-1, 0, 1, 2};
    std::vector<Quat> primary = {
        quat_axis_angle(0, 1, 0, 0.55f),
        quat_axis_angle(1, 0, 0, -0.25f),
        quat_axis_angle(0, 0, 1, 0.35f),
        quat_axis_angle(1, 0, 0, 0.15f),
    };
    const std::vector<Quat> weapon = {
        quat_axis_angle(1, 0, 0, -0.70f),
        quat_axis_angle(0, 1, 0, 0.45f),
        quat_axis_angle(0, 0, 1, -0.60f),
        quat_axis_angle(0, 1, 0, -0.20f),
    };
    const uint8_t mask[] = {0, 1, 1, 0};
    const std::vector<Quat> primary_world = fk_rotations(parents, primary);
    const std::vector<Quat> weapon_world = fk_rotations(parents, weapon);

    splice_weapon_channel_rotations(parents, mask, weapon, primary);
    const std::vector<Quat> mixed_world = fk_rotations(parents, primary);
    CHECK(quat_angle_between(mixed_world[0], primary_world[0]) < 1e-4f);
    CHECK(quat_angle_between(mixed_world[1], weapon_world[1]) < 1e-4f);
    CHECK(quat_angle_between(mixed_world[2], weapon_world[2]) < 1e-4f);
    CHECK(quat_angle_between(mixed_world[3], primary_world[3]) < 1e-4f);
}


void test_blends_wrap_across_the_bam_seam() {
    // The +/-180 deg seam: aim just left of the seam, body just across it. The x86
    // int32 wrap makes (B - A) the SHORTEST arc (a small positive delta), so every
    // blend stays glued to the seam instead of sweeping the long way — and in C++
    // that must come from the io/bam.h wrap helpers, not signed overflow (UB).
    using opennova::io::bam_add;
    using opennova::io::bam_dbl;
    using opennova::io::bam_sar;
    using opennova::io::bam_sub;
    AimOverlayInputs in;
    in.aim_yaw = INT32_MAX - 0xFFFF;         // ~ +180 deg, just below the seam
    in.body_yaw = INT32_MIN + 0x20001;       // ~ -180 deg, just past it
    in.aim_pitch = 0;
    in.body_pitch = 0;
    in.aim_state = true;
    AimOverlayAngles out[kOverlayClassCount];
    compute_aim_overlay_angles(in, out);

    const int32_t A = in.aim_yaw, B = in.body_yaw;
    const int32_t d = bam_sub(B, A);
    CHECK(d == 0x30001);  // the short way across the seam, not a -360 deg sweep
    CHECK(out[kOverlayUpperSpine].yaw == bam_add(A, bam_sar(d, 2)));
    CHECK(out[kOverlaySpine].yaw == bam_add(A, bam_sar(d, 1)));
    // Both blends sit INSIDE the short arc from A across the seam.
    CHECK(bam_sub(out[kOverlayUpperSpine].yaw, A) >= 0);
    CHECK(bam_sub(B, out[kOverlaySpine].yaw) >= 0);

    // The doubled pitch-blend term wraps two's-complement like the x86 shl.
    in.pitch_blend = 0x40000001;  // 2*PB wraps into the negative half-turn
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayArm].pitch == bam_dbl(in.pitch_blend));
    CHECK(bam_dbl(in.pitch_blend) == INT32_MIN + 2);
}

} // namespace

// The HELD-WEAPON attach basis. It is deliberately NOT any of the nine bone classes:
// the head class carries full-aim pitch and the arm class carries the 3/4-blended yaw, so
// reusing either points the third-person rifle visibly off-axis. Pins the yaw purity, the
// pitch-kick term's presence in the aim branch and absence outside it, and the roll term.
// [orig: aim @ 0x4b1bdc..0x4b1bf8; non-aim @ 0x4b1dd9..0x4b1dfa]
void test_held_weapon_attach_basis() {
    AimOverlayInputs in;
    in.aim_yaw = 40 * kBamDeg;
    in.body_yaw = 0;              // a wide aim/body split makes a blended yaw obvious
    in.aim_pitch = 10 * kBamDeg;
    in.body_pitch = 0;
    in.roll = 3 * kBamDeg;
    in.lean = 6 * kBamDeg;
    in.pitch_blend = 2 * kBamDeg;
    in.pitch_kick_accum = 5 * kBamDeg;
    in.aim_state = true;

    AimOverlayAngles nine[kOverlayClassCount];
    compute_aim_overlay_angles(in, nine);
    const AimOverlayAngles w = compute_held_weapon_attach_angles(in);

    // Yaw is the PURE aim yaw — not the arms' 3/4 blend toward the body.
    CHECK(w.yaw == in.aim_yaw);
    CHECK(w.yaw != nine[kOverlayArm].yaw);
    // Pitch carries the pitch kick + twice the pitch blend, which matches the ARM class here
    // and differs from the HEAD class (full aim pitch, no extra terms).
    CHECK(w.pitch == opennova::io::bam_add(
                             opennova::io::bam_add(in.aim_pitch, in.pitch_kick_accum),
                             in.pitch_blend * 2));
    CHECK(w.pitch == nine[kOverlayArm].pitch);
    CHECK(w.pitch != nine[kOverlayHead].pitch);
    CHECK(w.roll == opennova::io::bam_add(in.roll, in.lean));

    // Outside an aim state the pitch-kick term is simply absent.
    in.aim_state = false;
    const AimOverlayAngles n = compute_held_weapon_attach_angles(in);
    CHECK(n.yaw == in.aim_yaw);
    CHECK(n.pitch == opennova::io::bam_add(in.aim_pitch, in.pitch_blend * 2));
    CHECK(n.roll == opennova::io::bam_add(in.roll, in.lean));

    // States 41/42 zero the roll source before any build, so the lean stands alone.
    in.rolling = true;
    CHECK(compute_held_weapon_attach_angles(in).roll == in.lean);
}

int main() {
    test_bone_class_map();
    test_aim_branch_blends();
    test_blends_wrap_across_the_bam_seam();
    test_non_aim_branch();
    test_mounted_overlay_matrix_selection();
    test_unknown_gunner_config_is_not_valid_config_zero();
    test_rolling_zeroes_roll();
    test_apply_identity_is_passthrough();
    test_apply_world_delta_reaches_target();
    test_apply_differential_bend();
    test_weapon_channel_splices_world_rotations();
    test_held_weapon_attach_basis();
    if (failures == 0) std::printf("aim_overlay_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
