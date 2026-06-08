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
//  (whose "X negated, Y/Z kept" is exactly the mesh's (-x,y,z) relationship). Exact
//  byte-fidelity to that chain (100-vs-108 bone stride, sign convention) is a tracked
//  grill-ida follow-up.]

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
    // Raw BadBone BIND pose (engine-native): the 3x3 world bind rotation (row-major) and the
    // parent-local bind position, straight from the .bad. The host builds the Skeleton3D REST from
    // THIS (matching oscarmike adm_import_plugin + the Blender importer's build_armature_from_bad),
    // not from a sampled frame -- the .3di mesh is skinned to this bind, so the skin must match it.
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
Clip sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins = {});

// --- quaternion / vector helpers, exposed for tests ---
Quat quat_normalize(Quat q);
Quat quat_mul(Quat a, Quat b);
Quat quat_inv(Quat q);          // conjugate of the normalized quat
Vec3 quat_rotate(Quat q, Vec3 v);
// BAD channel quaternion (stored x, y, z, w) -> our w-first {w,x,y,z}, normalized
// (a reorder only -- the engine consumes BAD quaternions natively, no axis swap).
Quat bad_channel_quat(float x, float y, float z, float w);

}  // namespace opennova::anim

#endif  // OPENNOVA_ANIM_SAMPLE_H
