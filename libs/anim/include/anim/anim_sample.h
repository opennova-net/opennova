// Portable skeletal-animation evaluator for NovaLogic .bad clips.
//
// Samples a parsed .bad into per-frame, per-bone transforms in the engine's NATIVE
// space (Y-up, same as the original NovaLogic engine and as Godot). This is NOT the
// Blender Z-up convention in blender/opennova/animation_build.py -- that conversion only
// exists because Blender is Z-up. The proven Godot port (oscarmike opennova_adm) reads
// BAD bone data natively: bone position as-is, the channel quaternion reordered only
// (no axis swap); the mesh is the one carrying the (-x,y,z) handedness flip, and the
// skin's global-rest-inverse bind keeps mesh and bones consistent.
//
// [orig: the runtime pose chain is BoneAnim_TransformBones @0x410360 ->
//  AnimChannel_ComputeBoneMatrices @0x410da0 -> build_world_bone_matrices @0x40c770
//  (world entities) / BoneAnim_BuildWorldMatrices @0x40c400 (first-person viewmodel)
//  (whose "X negated, Y/Z kept" is exactly the mesh's (-x,y,z) relationship).]
//
// The witnessed channel semantics (grilled 2026-07-08): the per-bone channel rotation is
// NOT an absolute bone orientation -- AnimChannel_ComputeBoneMatrices @0x410da0 multiplies
// Transpose(bind 3x3) x sampled channel matrix, i.e. every channel is measured RELATIVE to
// the same .bad's per-bone bind rotation, and the composed builders (@0x40c400/@0x40c770)
// then treat the result as the bone's rotation with a PURE-TRANSLATION bind: the skinning
// bind-inverse is T(-pivot), no rotation. At the reset clip the delta is identity, so the
// authored mesh renders verbatim regardless of how degenerate the stored bind matrices
// look in isolation (ak47_RST.bad's are wild permutations; the deltas are sane).
// `model_bind` below enables that faithful interpretation; the default (absolute channel
// rotations + bind-matrix rest) is the legacy body pipeline, byte-identical for healthy
// exports where channel-at-reset == bind.

#ifndef OPENNOVA_ANIM_SAMPLE_H
#define OPENNOVA_ANIM_SAMPLE_H

#include <cstdint>
#include <string>
#include <vector>

struct BadFile;  // bad/bad.h

namespace opennova::anim {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Quaternion, w-first (matches animation_build.py's (w, x, y, z) order).
struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

struct BoneSample {
    Quat local_rotation;   // parent-relative
    Vec3 local_position;   // parent-relative
    Quat world_rotation;   // model-space
    Vec3 world_position;   // model-space
};

struct ClipBone {
    std::string name;
    int parent_index = -1;
    // BIND pose the host builds the Skeleton3D REST from. Default mode: the raw BadBone 3x3
    // world bind rotation (row-major) + bind position, straight from the .bad (matching oscarmike
    // adm_import_plugin + the Blender importer's build_armature_from_bad). model_bind mode:
    // IDENTITY rotation -- the original's bind is a pure translation (the skin bind-inverse is
    // T(-pivot)), and the channel rotations are re-based to be relative to the .bad bind
    // [orig: BoneAnim_BuildWorldMatrices @0x40c400 + AnimChannel_ComputeBoneMatrices @0x410da0].
    float rest_rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float rest_position[3] = {0, 0, 0};
};

struct Clip {
    uint32_t fps = 0;
    uint32_t flags = 0;            // bit0 = loop, bit1 = translation present
    uint32_t frame_count = 0;
    std::vector<ClipBone> bones;   // skeleton: names + parent indices (shared by all frames)
    // frames[frame][bone]. Empty bones list + zero frames on a malformed clip.
    std::vector<std::vector<BoneSample>> frames;

    bool loops() const { return (flags & 0x01u) != 0; }
    size_t bone_count() const { return bones.size(); }
};

// Sample a parsed .bad into engine-native (Y-up) space. Per-frame orientation comes from
// the rotation channels; per-frame translation is applied when (flags & 2). Bone rest
// ORIGINS (the skeleton offsets used in the FK accumulation) come from shared_rest_origins
// when its size matches the bone count, else from this clip's own BadBone positions. The
// shared origins MUST be passed for non-bind clips: a model's clips share ONE skeleton, but
// each clip's .bad may carry different/zero bone positions -- using a clip's own positions
// collapses the pose. (Matches oscarmike, which accumulates with skeleton->get_bone_rest.)
//
// model_bind: faithful channel semantics -- every sampled channel rotation is re-based
// against this .bad's own per-bone bind 3x3 (a delta from the bind), rest_rotation becomes
// identity (the bind is a pure translation: the skin bind-inverse is T(-pivot)), and the FK
// rotates pivots by the re-based rotations. Use with the model's bone pivots as
// shared_rest_origins. Everything stays in the NATIVE model frame -- the host renders these
// rigs with a native-frame mesh (NovaObjectData native_frame submeshes) and maps the whole
// rig to the camera in one container transform; the original's S=diag(-1,1,1) conjugation +
// x-negated pivots/translations in its composed builders are its model->render frame map,
// realized here at that container boundary instead. At the reset clip the pose is identity
// + absolute pivots, i.e. the authored mesh verbatim, for ANY export -- including rigs whose
// stored bind matrices are degenerate (ak47_RST). [orig: AnimChannel_ComputeBoneMatrices
// @0x410da0 (Transpose(bind) x channel); BoneAnim_BuildWorldMatrices @0x40c400 (T(-pivot)
// bind-inverse, translation-only hierarchy, the S*A^T*S copy loops).]
Clip sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins = {},
                 bool model_bind = false);

// TEMPORARY (see anim_sample.cpp): model_bind delta composition under test.
// 1 = bind^-1*ch, 2 = bind*ch, 3 = (bind^-1*ch)^-1, 4 = (bind*ch)^-1.
extern int g_model_bind_delta_variant;

// --- quaternion / vector helpers, exposed for tests ---
Quat quat_normalize(Quat q);
Quat quat_mul(Quat a, Quat b);
Quat quat_inv(Quat q);          // conjugate of the normalized quat
Vec3 quat_rotate(Quat q, Vec3 v);
// Shortest-path slerp (lerp fast-path when near-parallel), mirrors Math_QuaternionSlerp @0x615e20.
Quat quat_slerp(Quat a, Quat b, float t);
// BAD channel quaternion (stored x, y, z, w) -> our w-first {w,x,y,z}, normalized
// (a reorder only -- the engine consumes BAD quaternions natively, no axis swap).
Quat bad_channel_quat(float x, float y, float z, float w);
// Row-major 3x3 (the BadBone.rotation layout) -> quaternion, Shepperd's method.
Quat mat3_to_quat(const float m[9]);

}  // namespace opennova::anim

#endif  // OPENNOVA_ANIM_SAMPLE_H
