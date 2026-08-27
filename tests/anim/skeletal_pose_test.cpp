// anim skeletal_pose — the time-domain pose primitives extracted from the
// shell adapter's skeletal evaluator (S3, ADR 0028): frame-window selection
// (loop wrap / hold-last / one-frame window), blend endpoints, the BN## tag
// parse, and the .bad bind-rest transpose+orthonormalize.
#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/skeletal_pose.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace opennova::anim;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

bool near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) < tol; }

bool quat_same_rotation(const Quat &a, const Quat &b, float tol = 1e-5f) {
    const float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    return std::fabs(std::fabs(dot) - 1.0f) < tol;
}

Quat qy(double angle) {
    return Quat{static_cast<float>(std::cos(angle * 0.5)), 0.0f,
            static_cast<float>(std::sin(angle * 0.5)), 0.0f};
}

// A one-bone clip whose keys rotate about +Y by 0 / 90 / 180 degrees across
// two interval windows (three baked keys), origin marching +x per key.
Clip test_clip(bool loops) {
    Clip clip;
    clip.fps = 30;
    clip.frame_count = 2;
    clip.flags = loops ? 0x1u : 0x0u;
    clip.bones.resize(1);
    clip.bones[0].name = "BN01 Hips";
    clip.frames.resize(3);
    for (int k = 0; k < 3; ++k) {
        BoneSample s;
        s.local_rotation = qy(k * 1.57079632679489661923);
        s.local_position = Vec3{static_cast<float>(k), 0.0f, 0.0f};
        clip.frames[k].push_back(s);
    }
    return clip;
}

} // namespace

int main() {
    const double kFrame = 1.0 / 30.0; // one frame at fps 30

    // ---- one-shot: plays both windows, HOLDS the true final key ----
    std::vector<PoseBone> pose;
    const Clip oneshot = test_clip(false);
    eval_clip_pose(oneshot, 0.0, pose);
    CHECK(pose.size() == 1 && quat_same_rotation(pose[0].rotation, qy(0.0)));
    eval_clip_pose(oneshot, 0.5 * kFrame, pose); // mid window 0
    CHECK(quat_same_rotation(pose[0].rotation, qy(0.25 * 3.14159265)));
    CHECK(near(pose[0].origin.x, 0.5f));
    eval_clip_pose(oneshot, 10.0, pose); // far past the end: hold key 2
    CHECK(quat_same_rotation(pose[0].rotation, qy(3.14159265)));
    CHECK(near(pose[0].origin.x, 2.0f));
    eval_clip_pose(oneshot, -1.0, pose); // negative clamps to key 0
    CHECK(near(pose[0].origin.x, 0.0f));

    // ---- loop: cycles the frame_count interval windows (the seam key is
    // unwalked: window 1 interpolates key1 -> key0 via the modulo) ----
    const Clip looped = test_clip(true);
    eval_clip_pose(looped, 2.0 * kFrame, pose); // exactly one cycle: key 0
    CHECK(near(pose[0].origin.x, 0.0f));
    eval_clip_pose(looped, 1.5 * kFrame, pose); // mid window 1: key1 -> key0
    CHECK(near(pose[0].origin.x, 0.5f));

    // ---- a one-frame clip is one full window of motion, not a static pose ----
    Clip single = test_clip(false);
    single.frame_count = 1;
    single.frames.resize(2);
    eval_clip_pose(single, 0.5 * kFrame, pose);
    CHECK(near(pose[0].origin.x, 0.5f));
    eval_clip_pose(single, 5.0, pose); // hold the final key
    CHECK(near(pose[0].origin.x, 1.0f));

    // ---- blend: endpoints short-circuit, mismatch yields the target ----
    std::vector<PoseBone> a(1), b(1), out;
    a[0].rotation = qy(0.0);
    a[0].origin = Vec3{0, 0, 0};
    b[0].rotation = qy(1.0);
    b[0].origin = Vec3{2, 0, 0};
    blend_poses(a, b, 0.0f, out);
    CHECK(near(out[0].origin.x, 0.0f));
    blend_poses(a, b, 1.0f, out);
    CHECK(near(out[0].origin.x, 2.0f));
    blend_poses(a, b, 0.5f, out);
    CHECK(near(out[0].origin.x, 1.0f));
    CHECK(quat_same_rotation(out[0].rotation, qy(0.5)));
    std::vector<PoseBone> mismatched(2);
    blend_poses(mismatched, b, 0.5f, out);
    CHECK(out.size() == 1 && near(out[0].origin.x, 2.0f));

    // ---- BN## tag parse ----
    CHECK(model_bone_index_from_name("BN01 Hips") == 0);
    CHECK(model_bone_index_from_name("bn17 R Hand") == 16);
    CHECK(model_bone_index_from_name("BN19 L Foot") == 18);
    CHECK(model_bone_index_from_name("Helmet") == -1);
    CHECK(model_bone_index_from_name("BN") == -1);
    CHECK(overlay_class_for_bone_name("BN01 Hips") == kOverlayBody);
    CHECK(overlay_class_for_bone_name("BN04 R Clavicle") == kOverlayClavicle);
    CHECK(overlay_class_for_bone_name("BN15 Head") == kOverlayHead);
    CHECK(overlay_class_for_bone_name("BN20 Gear") == kOverlayBody); // past table
    CHECK(overlay_class_for_bone_name("Helmet") == kOverlayBody);

    // ---- bind rest: the row-vector .bad product TRANSPOSES into the
    // column-vector basis (the adapter's column-axes Basis conversion) ----
    ClipBone root;
    // Row-vector Rz(90°): rows {0,1,0, -1,0,0, 0,0,1}; its transpose is the
    // column-vector Rz(90°) = {0,-1,0, 1,0,0, 0,0,1}.
    const float rz90_rowvec[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) root.rest_rotation[i] = rz90_rowvec[i];
    root.rest_position[0] = 1.0f;
    float rows[9];
    Vec3 origin;
    bind_rest_local(root, nullptr, rows, origin);
    CHECK(near(origin.x, 1.0f));
    CHECK(near(rows[0], 0.0f) && near(rows[1], -1.0f) && near(rows[3], 1.0f) &&
            near(rows[4], 0.0f) && near(rows[8], 1.0f));
    // A child sharing the parent's bind relativizes to identity.
    ClipBone child = root;
    child.rest_position[0] = 0.0f;
    child.rest_position[2] = 2.0f;
    bind_rest_local(child, &root, rows, origin);
    CHECK(near(origin.z, 2.0f));
    CHECK(near(rows[0], 1.0f) && near(rows[4], 1.0f) && near(rows[8], 1.0f) &&
            near(rows[1], 0.0f) && near(rows[3], 0.0f));
    // A degenerate (zero) parent matrix zeroes the product; the determinant
    // guard turns it into identity — the adapter's exact fallback.
    ClipBone degenerate;
    for (int i = 0; i < 9; ++i) degenerate.rest_rotation[i] = 0.0f;
    bind_rest_local(child, &degenerate, rows, origin);
    CHECK(near(rows[0], 1.0f) && near(rows[4], 1.0f) && near(rows[8], 1.0f));

    // ---- quat -> rows roundtrip against the sampler's mat3_to_quat ----
    const Quat q = quat_normalize(Quat{0.8f, 0.3f, -0.4f, 0.33f});
    float qrows[9];
    quat_to_mat3_rows(q, qrows);
    CHECK(quat_same_rotation(mat3_to_quat(qrows), q));

    if (failures == 0) std::printf("anim_skeletal_pose: OK\n");
    return failures == 0 ? 0 : 1;
}
