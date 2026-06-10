// libs/anim skeletal sampler tests.
//
// Validates the portable .bad sampler (opennova::anim::sample_clip) in engine-native
// (Y-up) space against:
//   1. the native conversion conventions (bad_channel_quat = normalize(w, x, y, z) reorder
//      only; bone bind position used as-is),
//   2. quaternion helper correctness, and
//   3. world<->local self-consistency on the real BINOC.bad fixture (catches sign /
//      multiply-order bugs; space-independent).

#include "anim/anim_sample.h"

#include "bad/bad.h"

#include <cmath>
#include <cstdio>

#include "common/test_expect.h"
#include "common/test_paths.h"

using opennova::anim::Quat;
using opennova::anim::Vec3;

static bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

static bool quat_approx(const Quat &a, const Quat &b, float eps = 1e-4f) {
    return approx(a.w, b.w, eps) && approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

int main() {
    using namespace opennova::anim;

    // --- native conversion conventions (engine Y-up; reorder only, no axis swap) ---
    {
        // bad_channel_quat(x,y,z,w) = normalize(w, x, y, z)
        Quat q = bad_channel_quat(0.1f, 0.2f, 0.3f, 0.9f);
        // before normalize: (0.9, 0.1, 0.2, 0.3)
        float n = std::sqrt(0.9f * 0.9f + 0.1f * 0.1f + 0.2f * 0.2f + 0.3f * 0.3f);
        TEST_EXPECT(quat_approx(q, {0.9f / n, 0.1f / n, 0.2f / n, 0.3f / n}));
    }

    // --- quaternion helpers ---
    {
        Quat id = {1, 0, 0, 0};
        // 90 deg about Z (w=cos45, z=sin45)
        const float s = std::sqrt(0.5f);
        Quat rz = {s, 0, 0, s};
        // identity * q == q
        TEST_EXPECT(quat_approx(quat_mul(id, rz), rz));
        // q * inv(q) == identity
        TEST_EXPECT(quat_approx(quat_mul(rz, quat_inv(rz)), id));
        // rotate x-axis by +90 about Z -> y-axis
        Vec3 v = quat_rotate(rz, {1, 0, 0});
        TEST_EXPECT(approx(v.x, 0.0f) && approx(v.y, 1.0f) && approx(v.z, 0.0f));
    }

    // --- sample the real fixture ---
    char path[4096];
    std::snprintf(path, sizeof(path), "%s%cfixtures%cbad%cBINOC.bad",
                  test_paths_repo_root(__FILE__), TEST_PATHS_SEP,
                  TEST_PATHS_SEP, TEST_PATHS_SEP);
    BadFile bad = {};
    TEST_EXPECT(bad_parse(path, &bad) == 0);

    Clip clip = sample_clip(bad);
    TEST_EXPECT(clip.bones.size() == 19);
    TEST_EXPECT(clip.frame_count == 3);
    TEST_EXPECT(clip.fps == 30);
    TEST_EXPECT(clip.loops());  // flags == 1
    TEST_EXPECT(clip.frames.size() == 3);

    // Bone 0 is the root (no parent): world == local, and (no translations on this clip)
    // its world position is its bind origin in Z-up.
    TEST_EXPECT(clip.bones[0].parent_index == -1);
    Vec3 root_rest = {bad.bones[0].position[0], bad.bones[0].position[1], bad.bones[0].position[2]};
    for (uint32_t f = 0; f < clip.frame_count; ++f) {
        const BoneSample &b0 = clip.frames[f][0];
        TEST_EXPECT(approx(b0.world_position.x, root_rest.x));
        TEST_EXPECT(approx(b0.world_position.y, root_rest.y));
        TEST_EXPECT(approx(b0.world_position.z, root_rest.z));
        TEST_EXPECT(quat_approx(b0.local_rotation, b0.world_rotation));
        // frame-0 root world rotation == the native-reordered channel-0 frame-0 quaternion
        if (f == 0 && bad.channels[0].rotations != nullptr) {
            const BadQuaternion &rq = bad.channels[0].rotations[0];
            TEST_EXPECT(quat_approx(b0.world_rotation, bad_channel_quat(rq.x, rq.y, rq.z, rq.w)));
        }
    }

    // world<->local self-consistency for every child bone: recomposing world from the
    // parent world + the derived local must return the original world (proves local is
    // the correct inverse, with matching multiply order/signs).
    for (uint32_t f = 0; f < clip.frame_count; ++f) {
        const auto &frame = clip.frames[f];
        for (size_t b = 0; b < clip.bones.size(); ++b) {
            const int parent = clip.bones[b].parent_index;
            if (parent < 0) {
                continue;
            }
            const BoneSample &pb = frame[static_cast<size_t>(parent)];
            const BoneSample &cb = frame[b];
            Quat recomposed_rot = quat_mul(pb.world_rotation, cb.local_rotation);
            TEST_EXPECT(quat_approx(recomposed_rot, cb.world_rotation));
            Vec3 recomposed_pos;
            {
                Vec3 r = quat_rotate(pb.world_rotation, cb.local_position);
                recomposed_pos = {pb.world_position.x + r.x, pb.world_position.y + r.y, pb.world_position.z + r.z};
            }
            TEST_EXPECT(approx(recomposed_pos.x, cb.world_position.x));
            TEST_EXPECT(approx(recomposed_pos.y, cb.world_position.y));
            TEST_EXPECT(approx(recomposed_pos.z, cb.world_position.z));
            // every quaternion stays normalized
            float n2 = cb.world_rotation.w * cb.world_rotation.w + cb.world_rotation.x * cb.world_rotation.x +
                       cb.world_rotation.y * cb.world_rotation.y + cb.world_rotation.z * cb.world_rotation.z;
            TEST_EXPECT(approx(n2, 1.0f, 1e-3f));
        }
    }

    // --- shared rest origins drive the FK skeleton (regression: per-clip bone positions ---
    // must NOT be used, or a clip whose .bad carries zero/different bone positions collapses
    // the whole pose; a model's clips share ONE skeleton's offsets). ---
    {
        std::vector<Vec3> own(clip.bones.size());
        std::vector<Vec3> zero(clip.bones.size(), Vec3{0.0f, 0.0f, 0.0f});
        for (size_t b = 0; b < clip.bones.size(); ++b) {
            own[b] = {bad.bones[b].position[0], bad.bones[b].position[1], bad.bones[b].position[2]};
        }
        Clip with_own = sample_clip(bad, own);
        Clip with_zero = sample_clip(bad, zero);
        bool same_as_default = true;  // shared == own reproduces the default (no-shared) sampling
        bool zero_collapsed = true;   // all-zero shared collapses every bone world position to origin
        bool real_differs = false;    // the real (own) sampling is NOT collapsed
        for (uint32_t f = 0; f < clip.frame_count; ++f) {
            for (size_t b = 0; b < clip.bones.size(); ++b) {
                const Vec3 d = clip.frames[f][b].world_position;
                const Vec3 o = with_own.frames[f][b].world_position;
                const Vec3 z = with_zero.frames[f][b].world_position;
                if (!approx(d.x, o.x) || !approx(d.y, o.y) || !approx(d.z, o.z)) same_as_default = false;
                if (!approx(z.x, 0.0f, 1e-3f) || !approx(z.y, 0.0f, 1e-3f) || !approx(z.z, 0.0f, 1e-3f)) zero_collapsed = false;
                if (!approx(d.x, z.x) || !approx(d.y, z.y) || !approx(d.z, z.z)) real_differs = true;
            }
        }
        TEST_EXPECT(same_as_default);
        TEST_EXPECT(zero_collapsed);
        TEST_EXPECT(real_differs);
    }

    // --- compressed clip: per-bone sparse keyframes with non-uniform durations (regression) ---
    // A flat rotations[frame] index snaps a bone to IDENTITY once frame >= its keyframe count; the
    // per-bone duration walk (BoneAnim_FindKeyframeAtTime) must instead hold/interpolate its own
    // keyframes across the whole clip. Build a 10-frame clip where bone0 is keyed ONCE.
    {
        BadBone sbones[2] = {};
        sbones[0].parent_index = -1;
        sbones[0].rotation[0] = 1.0f; sbones[0].rotation[4] = 1.0f; sbones[0].rotation[8] = 1.0f;
        sbones[1].parent_index = 0; sbones[1].position[1] = 1.0f;
        sbones[1].rotation[0] = 1.0f; sbones[1].rotation[4] = 1.0f; sbones[1].rotation[8] = 1.0f;

        uint16_t fl0[1] = {10};
        BadQuaternion rot0[1] = {{0.0f, 0.0f, 0.70710678f, 0.70710678f}};            // 90deg about Z
        uint16_t fl1[3] = {3, 3, 4};
        BadQuaternion rot1[3] = {{0, 0, 0, 1}, {0, 0, 0.70710678f, 0.70710678f}, {0, 0, 1, 0}};

        BadChannel schan[2] = {};
        schan[0].frame_count = 1; schan[0].frame_lengths = fl0; schan[0].rotations = rot0;
        schan[1].frame_count = 3; schan[1].frame_lengths = fl1; schan[1].rotations = rot1;

        BadFile sbad = {};
        sbad.fps = 30; sbad.frame_count = 10; sbad.flags = 1;
        sbad.bones = sbones; sbad.num_bones = 2;
        sbad.channels = schan; sbad.num_channels = 2;

        Clip sclip = sample_clip(sbad);
        TEST_EXPECT(sclip.frames.size() == 10);
        const Quat held = bad_channel_quat(0.0f, 0.0f, 0.70710678f, 0.70710678f);
        const Quat identity = {1.0f, 0.0f, 0.0f, 0.0f};
        // bone0 (one keyframe) is HELD for the whole clip -- never identity past frame 0.
        TEST_EXPECT(quat_approx(sclip.frames[0][0].world_rotation, held));
        TEST_EXPECT(quat_approx(sclip.frames[5][0].world_rotation, held));
        TEST_EXPECT(quat_approx(sclip.frames[9][0].world_rotation, held));
        TEST_EXPECT(!quat_approx(sclip.frames[5][0].world_rotation, identity));  // the old-code bug
        // bone1 mid-window (tick 5 spans keyframe 1->2): a valid normalized, non-identity slerp.
        const Quat &w1 = sclip.frames[5][1].world_rotation;
        TEST_EXPECT(approx(w1.w * w1.w + w1.x * w1.x + w1.y * w1.y + w1.z * w1.z, 1.0f, 1e-3f));
    }

    bad_free(&bad);
    return 0;
}
