// Aim-overlay unit tests [orig: Entity_BuildBoneTransformMatrices @ 0x4b1290;
// docs/world/world-wac-ai-re.md section 14]: the bone-class map, the exact BAM blend
// formulas of both on-foot branches, and the pose compose invariants (identity deltas =
// passthrough; child origins preserved under any delta; the bend gradient orders spine
// segments between body and aim).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "anim/aim_overlay.h"

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
    // float32 (one ulp of w below 1.0 reads as ~7e-4 rad), which flips with the host's
    // FMA contraction. The vector part carries small angles at full precision.
    Quat d = quat_mul(quat_inv(a), b);
    const float v = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * std::atan2(v, std::fabs(d.w));
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

    // pitch_blend / head_look_decay terms land where witnessed. [orig: @ 0x4b1bce]
    in.pitch_blend = 4 * kBamDeg;
    in.head_look_decay = 8 * kBamDeg;
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayArm].pitch == P + in.head_look_decay + 2 * in.pitch_blend);
    CHECK(out[kOverlayUpperSpine].pitch == P + in.pitch_blend);
    CHECK(out[kOverlaySpine].pitch == (P >> 2) + (in.pitch_blend >> 1));
}

void test_non_aim_branch() {
    AimOverlayInputs in;
    in.aim_yaw = 90 * kBamDeg;
    in.aim_pitch = -30 * kBamDeg;
    in.body_yaw = 60 * kBamDeg;
    in.head_look_decay = 8 * kBamDeg;
    in.aim_state = false;
    AimOverlayAngles out[kOverlayClassCount];
    compute_aim_overlay_angles(in, out);
    // Spine family rides the body; head keeps full aim; arms track with HLD/4.
    // [orig: @ 0x4b1cf1..0x4b1dfa]
    CHECK(out[kOverlaySpine].yaw == in.body_yaw && out[kOverlaySpine].pitch == 0);
    CHECK(out[kOverlayNeck].yaw == in.body_yaw);
    CHECK(out[kOverlayHead].yaw == in.aim_yaw && out[kOverlayHead].pitch == in.aim_pitch);
    CHECK(out[kOverlayArm].pitch == in.aim_pitch + (in.head_look_decay >> 2));
    in.arms_locked = true; // Flags & 0x100000 [orig: @ 0x4b1d48]
    compute_aim_overlay_angles(in, out);
    CHECK(out[kOverlayArm].yaw == in.body_yaw && out[kOverlayArm].pitch == 0);
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

} // namespace

int main() {
    test_bone_class_map();
    test_aim_branch_blends();
    test_non_aim_branch();
    test_rolling_zeroes_roll();
    test_apply_identity_is_passthrough();
    test_apply_world_delta_reaches_target();
    test_apply_differential_bend();
    if (failures == 0) std::printf("aim_overlay_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
