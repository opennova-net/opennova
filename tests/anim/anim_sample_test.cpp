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
    // frame_count counts INTERVALS; the pose table bakes every channel key
    // (frame_count + 1 — the fence-post witness, BoneAnim_FindKeyframeAtTime
    // @0x410220 walks all keys; dropping the last one erased one-frame clips'
    // whole motion, the REVVY M4 fire kick).
    TEST_EXPECT(clip.frames.size() == 4);

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

    // --- shared origins also drive the Skeleton3D REST (rest_position follows the shared source, ---
    // not BadBone.position). This is what lets the .3di model's bone pivots override the lossy .bad
    // positions for BOTH the FK and the bind rest, so they agree (the FP viewmodel fix). ---
    {
        std::vector<Vec3> custom(clip.bones.size());
        for (size_t b = 0; b < clip.bones.size(); ++b) {
            // distinct, independent per-bone origins unlike anything in the .bad
            custom[b] = {static_cast<float>(b) * 0.5f, static_cast<float>(b) * -0.25f, 1.0f + static_cast<float>(b)};
        }
        Clip with_custom = sample_clip(bad, custom);
        bool rest_follows_shared = true;
        bool default_rest_is_bad = true;
        for (size_t b = 0; b < with_custom.bones.size(); ++b) {
            const auto &rp = with_custom.bones[b].rest_position;
            if (!approx(rp[0], custom[b].x) || !approx(rp[1], custom[b].y) || !approx(rp[2], custom[b].z))
                rest_follows_shared = false;
            // With NO shared origins, rest_position stays the raw .bad position (legacy path).
            const auto &dp = clip.bones[b].rest_position;
            if (!approx(dp[0], bad.bones[b].position[0]) || !approx(dp[1], bad.bones[b].position[1]) ||
                    !approx(dp[2], bad.bones[b].position[2]))
                default_rest_is_bad = false;
        }
        TEST_EXPECT(rest_follows_shared);
        TEST_EXPECT(default_rest_is_bad);
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
        TEST_EXPECT(sclip.frames.size() == 11);  // frame_count intervals + the final key
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

    // --- a ONE-frame clip is one window of motion, not a static pose (the REVVY M4 ---
    // fire kick, m4_1f: header frame_count 1, channels keyed rest -> kicked). Both keys
    // must land in the pose table; the pre-fix truncation collapsed the clip to key 0
    // and the viewmodel froze through every volley.
    {
        BadBone obone[1] = {};
        obone[0].parent_index = -1;
        obone[0].rotation[0] = obone[0].rotation[4] = obone[0].rotation[8] = 1.0f;

        uint16_t ofl[2] = {1, 1};
        BadQuaternion orot[2] = {{0, 0, 0, 1},                                  // key 0: rest
                                 {0.0f, 0.0f, 0.70710678f, 0.70710678f}};       // key 1: kicked
        BadChannel ochan[1] = {};
        ochan[0].frame_count = 2; ochan[0].frame_lengths = ofl; ochan[0].rotations = orot;

        BadFile obad = {};
        obad.fps = 30; obad.frame_count = 1; obad.flags = 0;  // one-shot
        obad.bones = obone; obad.num_bones = 1;
        obad.channels = ochan; obad.num_channels = 1;

        Clip oclip = sample_clip(obad);
        TEST_EXPECT(oclip.frame_count == 1);
        TEST_EXPECT(oclip.frames.size() == 2);
        const Quat rest = bad_channel_quat(0.0f, 0.0f, 0.0f, 1.0f);
        const Quat kicked = bad_channel_quat(0.0f, 0.0f, 0.70710678f, 0.70710678f);
        TEST_EXPECT(quat_approx(oclip.frames[0][0].world_rotation, rest));
        TEST_EXPECT(quat_approx(oclip.frames[1][0].world_rotation, kicked));
        TEST_EXPECT(!quat_approx(oclip.frames[0][0].world_rotation,
                                 oclip.frames[1][0].world_rotation));
    }

    // --- model_bind: the witnessed faithful channel semantics (the FP viewmodel fix). ---
    // Channels are re-based against the .bad's own bind 3x3 (a delta from the bind), rest
    // rotations become identity (the original's skin bind-inverse is the pure translation
    // T(-pivot)). The file stores the bind 3x3 TRANSPOSED relative to the channel quats (the
    // real-data finding on ak47_RST: mat3(bind) * channel == identity at the reset clip), so
    // the synthetic bind below stores the transpose of the channel rotation's matrix. Oracle:
    // the reset-equivalent channel must sample to IDENTITY -- for ANY bind, including a
    // degenerate permutation like ak47_RST's. [orig: AnimChannel_ComputeBoneMatrices
    // @0x410da0; BoneAnim_BuildWorldMatrices @0x40c400.]
    {
        BadBone mbones[2] = {};
        mbones[0].parent_index = -1;
        // bone0 bind: a wild permutation (x->z, y->-y, z->x flavor -- symmetric, its own
        // transpose), like the FP rigs carry.
        mbones[0].rotation[2] = 1.0f; mbones[0].rotation[4] = -1.0f; mbones[0].rotation[6] = 1.0f;
        mbones[1].parent_index = 0; mbones[1].position[0] = 2.0f;
        // bone1 channel rotation is 90deg about Z (row-major {0,-1,0, 1,0,0, 0,0,1});
        // the FILE stores its TRANSPOSE: {0,1,0, -1,0,0, 0,0,1}.
        mbones[1].rotation[1] = 1.0f; mbones[1].rotation[3] = -1.0f; mbones[1].rotation[8] = 1.0f;

        // Channels store the bind ROTATIONS (as quats, file (x,y,z,w) order) -- the transpose
        // of the stored 3x3s.
        const float chan0_mat[9] = {0, 0, 1, 0, -1, 0, 1, 0, 0};   // == its own transpose
        const float chan1_mat[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};   // Rz(90), transpose of stored
        const Quat qb0 = mat3_to_quat(chan0_mat);
        const Quat qb1 = mat3_to_quat(chan1_mat);
        BadQuaternion mrot0[1] = {{qb0.x, qb0.y, qb0.z, qb0.w}};
        BadQuaternion mrot1[1] = {{qb1.x, qb1.y, qb1.z, qb1.w}};
        uint16_t mfl[1] = {1};
        BadChannel mchan[2] = {};
        mchan[0].frame_count = 1; mchan[0].frame_lengths = mfl; mchan[0].rotations = mrot0;
        mchan[1].frame_count = 1; mchan[1].frame_lengths = mfl; mchan[1].rotations = mrot1;

        BadFile mbad = {};
        mbad.fps = 30; mbad.frame_count = 1; mbad.flags = 0;
        mbad.bones = mbones; mbad.num_bones = 2;
        mbad.channels = mchan; mbad.num_channels = 2;

        const std::vector<Vec3> pivots = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
        const Clip mb = sample_clip(mbad, pivots, /*model_bind=*/true);
        const Quat identity = {1.0f, 0.0f, 0.0f, 0.0f};
        // Reset oracle: bind-relative of a channel equal to its own bind == identity
        // (up to quat double-cover: -identity is the same rotation).
        for (int b = 0; b < 2; ++b) {
            Quat w = mb.frames[0][static_cast<size_t>(b)].world_rotation;
            if (w.w < 0.0f) {
                w = {-w.w, -w.x, -w.y, -w.z};
            }
            TEST_EXPECT(quat_approx(w, identity, 1e-3f));
            // Rest rotations forced to identity (pure-translation bind).
            const float *rr = mb.bones[static_cast<size_t>(b)].rest_rotation;
            TEST_EXPECT(approx(rr[0], 1) && approx(rr[4], 1) && approx(rr[8], 1) &&
                    approx(rr[1], 0) && approx(rr[2], 0) && approx(rr[3], 0) &&
                    approx(rr[5], 0) && approx(rr[6], 0) && approx(rr[7], 0));
        }
        // Native frame: with identity deltas the FK lands children at the plain pivot chain
        // (the authored joints).
        TEST_EXPECT(approx(mb.bones[1].rest_position[0], 2.0f, 1e-3f));
        TEST_EXPECT(approx(mb.frames[0][1].world_position.x, 2.0f, 1e-3f));
        TEST_EXPECT(approx(mb.frames[0][1].world_position.y, 0.0f, 1e-3f));
        TEST_EXPECT(approx(mb.frames[0][1].world_position.z, 0.0f, 1e-3f));
        // Legacy mode on the same data: world rotation stays the raw channel (the bind), NOT
        // identity.
        const Clip legacy = sample_clip(mbad, pivots, /*model_bind=*/false);
        Quat lw = legacy.frames[0][0].world_rotation;
        if (lw.w < 0.0f) {
            lw = {-lw.w, -lw.x, -lw.y, -lw.z};
        }
        TEST_EXPECT(!quat_approx(lw, identity, 1e-3f));

        // A NON-identity delta stays native (no conjugation): bind = identity (transpose
        // of identity is identity), channel = +90deg about Z -> the sampled rotation IS
        // the channel.
        BadBone cbones[1] = {};
        cbones[0].parent_index = -1;
        cbones[0].rotation[0] = 1.0f; cbones[0].rotation[4] = 1.0f; cbones[0].rotation[8] = 1.0f;
        const float s45 = 0.70710678f;
        BadQuaternion crot[1] = {{0.0f, 0.0f, s45, s45}};  // +90 about Z (x,y,z,w)
        BadChannel cchan[1] = {};
        cchan[0].frame_count = 1; cchan[0].frame_lengths = mfl; cchan[0].rotations = crot;
        BadFile cbad = {};
        cbad.fps = 30; cbad.frame_count = 1; cbad.flags = 0;
        cbad.bones = cbones; cbad.num_bones = 1;
        cbad.channels = cchan; cbad.num_channels = 1;
        const Clip cc = sample_clip(cbad, {{0.0f, 0.0f, 0.0f}}, /*model_bind=*/true);
        TEST_EXPECT(quat_approx(cc.frames[0][0].world_rotation, {s45, 0.0f, 0.0f, s45}, 1e-3f));

        // --- bind_source: the witnessed channel+44 mechanism. A rig's clips compose against
        // the SKELETON .bad's bind (the .adm slot-0 / reset animation), never their own. A
        // clip whose own stored bind mirrors its channel self-cancels ONLY under the no-source
        // fallback; against the skeleton bind the channel pose survives -- this is exactly the
        // difference between the FP viewmodel frozen at its T-pose and holding the weapon.
        // [orig: AnimMap_RegisterEntity @0x40bb60 (channel+44 = slot-0 .bad, set once);
        // AnimChannel_ComputeBoneMatrices @0x410da0 (fallback to the playing anim @0x410de3).]
        {
            // Clip: channel = +90 about Z, own stored bind = its transpose (self-canceling).
            BadBone kbones[1] = {};
            kbones[0].parent_index = -1;
            kbones[0].rotation[1] = 1.0f; kbones[0].rotation[3] = -1.0f; kbones[0].rotation[8] = 1.0f;
            BadQuaternion krot[1] = {{0.0f, 0.0f, s45, s45}};
            BadChannel kchan[1] = {};
            kchan[0].frame_count = 1; kchan[0].frame_lengths = mfl; kchan[0].rotations = krot;
            BadFile kclip = {};
            kclip.fps = 30; kclip.frame_count = 1; kclip.flags = 0;
            kclip.bones = kbones; kclip.num_bones = 1;
            kclip.channels = kchan; kclip.num_channels = 1;

            // Skeleton (reset) .bad: identity bind.
            BadBone skbones[1] = {};
            skbones[0].parent_index = -1;
            skbones[0].rotation[0] = 1.0f; skbones[0].rotation[4] = 1.0f; skbones[0].rotation[8] = 1.0f;
            BadFile skel = {};
            skel.fps = 30; skel.frame_count = 1; skel.flags = 0;
            skel.bones = skbones; skel.num_bones = 1;
            skel.channels = kchan; skel.num_channels = 1;

            const std::vector<Vec3> org = {{0.0f, 0.0f, 0.0f}};
            // No bind source (the original's no-override fallback): self-cancels to identity.
            Quat self_bound = sample_clip(kclip, org, /*model_bind=*/true).frames[0][0].world_rotation;
            if (self_bound.w < 0.0f) {
                self_bound = {-self_bound.w, -self_bound.x, -self_bound.y, -self_bound.z};
            }
            TEST_EXPECT(quat_approx(self_bound, {1.0f, 0.0f, 0.0f, 0.0f}, 1e-3f));
            // Against the skeleton bind: the channel pose SURVIVES (identity skeleton bind
            // makes every composition sense equal to the raw channel).
            const Clip held = sample_clip(kclip, org, /*model_bind=*/true, &skel);
            TEST_EXPECT(quat_approx(held.frames[0][0].world_rotation, {s45, 0.0f, 0.0f, s45}, 1e-3f));
        }
    }

    // --- model-table mode: the MODEL's bone table defines the rig (count, hierarchy, pivots); ---
    // the .bad contributes rotations only, paired by row index; rows past the .bad's channels
    // take row 0's already-composed rotation; BadBone.position is NEVER read (12 of 43 JO
    // viewmodel rigs ship broken values -- retail renders them all). [orig:
    // BoneAnim_BuildWorldMatrices @0x40c400 (FK bounded by modelDef+52, parent/pivot from the
    // modelDef+56 rows); the padding loop @0x40c5a1.]
    {
        const float s45 = 0.70710678f;
        BadBone tbones[2] = {};
        tbones[0].parent_index = -1;
        tbones[0].rotation[0] = 1.0f; tbones[0].rotation[4] = 1.0f; tbones[0].rotation[8] = 1.0f;
        // Garbage .bad positions -- the model table must win.
        tbones[0].position[0] = 9.0f; tbones[0].position[1] = 9.0f; tbones[0].position[2] = 9.0f;
        tbones[1] = tbones[0];
        tbones[1].parent_index = 0;

        BadQuaternion trot0[1] = {{0.0f, 0.0f, s45, s45}};  // +90 about Z
        BadQuaternion trot1[1] = {{0.0f, 0.0f, 0.0f, 1.0f}};  // identity
        uint16_t tfl[1] = {1};
        BadChannel tchan[2] = {};
        tchan[0].frame_count = 1; tchan[0].frame_lengths = tfl; tchan[0].rotations = trot0;
        tchan[1].frame_count = 1; tchan[1].frame_lengths = tfl; tchan[1].rotations = trot1;

        BadFile tbad = {};
        tbad.fps = 30; tbad.frame_count = 1; tbad.flags = 0;
        tbad.bones = tbones; tbad.num_bones = 2;
        tbad.channels = tchan; tbad.num_channels = 2;

        // Model table: 4 rows (2 more than the .bad), root's parent stored as ITSELF like the
        // real modelDef+56 rows.
        const std::vector<int> parents = {0, 0, 1, 0};
        const std::vector<Vec3> pivots = {
                {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 2.0f}};
        const Clip tc = sample_clip(tbad, pivots, /*model_bind=*/false, nullptr, parents);
        const Quat rz90 = bad_channel_quat(0.0f, 0.0f, s45, s45);

        TEST_EXPECT(tc.bones.size() == 4);                    // the MODEL's count, not the .bad's
        TEST_EXPECT(tc.bones[0].parent_index == -1);          // self-parent normalized
        TEST_EXPECT(tc.bones[1].parent_index == 0);
        TEST_EXPECT(tc.bones[2].parent_index == 1);
        TEST_EXPECT(tc.bones[3].parent_index == 0);
        TEST_EXPECT(tc.bones[2].name.empty());                // no .bad record -> embedder names it
        for (size_t b = 0; b < 4; ++b) {                      // pivots from the table, never .bad pos
            TEST_EXPECT(approx(tc.bones[b].rest_position[0], pivots[b].x) &&
                    approx(tc.bones[b].rest_position[1], pivots[b].y) &&
                    approx(tc.bones[b].rest_position[2], pivots[b].z));
        }
        const auto &fr = tc.frames[0];
        TEST_EXPECT(quat_approx(fr[0].world_rotation, rz90, 1e-3f));
        TEST_EXPECT(quat_approx(fr[1].world_rotation, {1, 0, 0, 0}, 1e-3f));
        // Padded rows (>= .bad channel count) carry row 0's composed rotation...
        TEST_EXPECT(quat_approx(fr[2].world_rotation, rz90, 1e-3f));
        TEST_EXPECT(quat_approx(fr[3].world_rotation, rz90, 1e-3f));
        // ...and FK through the MODEL hierarchy/pivots: row1 = Rz90*(1,0,0) = (0,1,0);
        // row2 under row1 (identity rot) adds (0,1,0); row3 under root adds Rz90*(0,0,2).
        TEST_EXPECT(approx(fr[1].world_position.x, 0.0f, 1e-3f) &&
                approx(fr[1].world_position.y, 1.0f, 1e-3f) &&
                approx(fr[1].world_position.z, 0.0f, 1e-3f));
        TEST_EXPECT(approx(fr[2].world_position.x, 0.0f, 1e-3f) &&
                approx(fr[2].world_position.y, 2.0f, 1e-3f) &&
                approx(fr[2].world_position.z, 0.0f, 1e-3f));
        TEST_EXPECT(approx(fr[3].world_position.x, 0.0f, 1e-3f) &&
                approx(fr[3].world_position.y, 0.0f, 1e-3f) &&
                approx(fr[3].world_position.z, 2.0f, 1e-3f));

        // Model table SMALLER than the .bad (the AKM_1st shape: 46 bones, 45 parts): the rig is
        // the model's size; the extra channel is simply never sampled.
        const std::vector<int> small_parents = {0};
        const std::vector<Vec3> small_pivots = {{0.0f, 0.0f, 0.0f}};
        const Clip sc = sample_clip(tbad, small_pivots, /*model_bind=*/false, nullptr, small_parents);
        TEST_EXPECT(sc.bones.size() == 1);
        TEST_EXPECT(quat_approx(sc.frames[0][0].world_rotation, rz90, 1e-3f));

        // model_bind + padding compose: identity bind -> padded row copies row 0's COMPOSED
        // rotation (bind (x) channel), not the raw channel.
        const Clip mbp = sample_clip(tbad, pivots, /*model_bind=*/true, &tbad, parents);
        TEST_EXPECT(quat_approx(mbp.frames[0][2].world_rotation,
                mbp.frames[0][0].world_rotation, 1e-3f));
    }

    // --- positions_from_model: reconstruct BadBone.position from the model table + the ---
    // skeleton .bad's bind rotations -- position[i] = bind_rows[parent(i)] . (-rel.x, rel.y,
    // rel.z); roots / rows whose parent is past the bind's records take the x-negated rel
    // unrotated. Synthetic exactness (mirrors pytest tests/test_bad_pos_derivation.py; the
    // corpus legs are asset-gated there). [docs/net/novaworld-net-re.md section 5.40
    // position-derivation correction.]
    {
        BadBone pbones[2] = {};
        pbones[0].parent_index = -1;
        // bone0 bind: Rz(90) row-major {0,-1,0, 1,0,0, 0,0,1}.
        pbones[0].rotation[1] = -1.0f;
        pbones[0].rotation[3] = 1.0f;
        pbones[0].rotation[8] = 1.0f;
        pbones[1].parent_index = 0;
        BadFile pbad = {};
        pbad.bones = pbones;
        pbad.num_bones = 2;

        const std::vector<int> parents = {0, 0, 1};  // root self-parented like the real rows
        const std::vector<Vec3> rels = {
                {0.5f, 0.25f, -0.75f},   // root: x-negated, unrotated
                {1.0f, 2.0f, 3.0f},      // parent 0: Rz90 . (-1, 2, 3)
                {4.0f, 5.0f, 6.0f},      // parent 1: identity bind (zero matrix rows? no --
        };                               //   bone1 bind is zeroed => acts as all-zero rows
        const std::vector<Vec3> got = positions_from_model(pbad, parents, rels);
        TEST_EXPECT(got.size() == 3);
        TEST_EXPECT(approx(got[0].x, -0.5f) && approx(got[0].y, 0.25f) && approx(got[0].z, -0.75f));
        // Rz90 rows applied to (-1, 2, 3): row0 = (0,-1,0) -> -2; row1 = (1,0,0) -> -1; row2 = (0,0,1) -> 3.
        TEST_EXPECT(approx(got[1].x, -2.0f) && approx(got[1].y, -1.0f) && approx(got[1].z, 3.0f));
        // bone1's bind rows are all zero (uninitialized record) -> zero vector out; the REAL
        // guarantee under test is the row-major multiply + index pairing, not this degenerate row.
        TEST_EXPECT(approx(got[2].x, 0.0f) && approx(got[2].y, 0.0f) && approx(got[2].z, 0.0f));
        // A parent past the bind's records falls back to the unrotated x-negated rel.
        const std::vector<int> far_parents = {5};
        const std::vector<Vec3> one_rel = {{1.0f, 2.0f, 3.0f}};
        const std::vector<Vec3> far = positions_from_model(pbad, far_parents, one_rel);
        TEST_EXPECT(far.size() == 1);
        TEST_EXPECT(approx(far[0].x, -1.0f) && approx(far[0].y, 2.0f) && approx(far[0].z, 3.0f));
    }

    bad_free(&bad);
    return 0;
}
