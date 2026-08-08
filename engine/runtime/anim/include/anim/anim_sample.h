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
// The witnessed channel semantics (grilled 2026-07-08, CORRECTED 2026-07-09): the per-bone
// channel rotation composes against a bind 3x3 -- AnimChannel_ComputeBoneMatrices @0x410da0
// multiplies Transpose(bind 3x3) x sampled channel matrix -- but the bind operand is NOT the
// playing clip's own bone records. The channel struct carries a bind-source override
// (channel+44) that AnimMap_RegisterEntity @0x40bb60 pins ONCE to the .adm's slot-0 .bad
// (the reset/skeleton animation, e.g. ak47_RST); clip switches (AnimMap_PlayAnimBySlot
// @0x40bda0 / AnimMap_UpdateEntity @0x40b5f0) re-init only the playing anim and never touch
// it. Every clip of a rig is therefore measured against the ONE skeleton bind -- the playing
// clip's own records are only the fallback when no override is set (@0x410de3, the menu
// preview's standalone channel). Composing each clip against its own bind self-cancels at
// clip start by construction and freezes the FP rig at its authored T-pose -- that was the
// 2026-07-08 misreading. The composed builders (@0x40c400/@0x40c770) then treat the result
// as the bone's rotation with a PURE-TRANSLATION bind: the skinning bind-inverse is
// T(-pivot), no rotation. `model_bind` below enables that faithful interpretation; the
// default (absolute channel rotations + bind-matrix rest) is the legacy body pipeline,
// byte-identical for healthy exports where channel-at-reset == bind.

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
    // BIND pose the embedder builds the Skeleton3D REST from. Default mode: the raw BadBone 3x3
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
// model_bind: faithful channel semantics -- every sampled channel rotation composes against
// the SKELETON's per-bone bind 3x3 (bind_source, the .adm slot-0 / reset .bad -- the
// channel+44 override AnimMap_RegisterEntity pins @0x40bbe3), rest_rotation becomes
// identity (the bind is a pure translation: the skin bind-inverse is T(-pivot)), and the FK
// rotates pivots by the composed rotations. bind_source == nullptr falls back to this
// .bad's own bone records -- the original's no-override path (@0x410de3), correct only for
// a standalone channel; a rig's clips MUST pass the shared skeleton .bad or every clip
// self-cancels at its start frame and the rig freezes at the authored bind (T-pose). Use
// with the model's bone pivots as shared_rest_origins. Everything stays in the NATIVE model
// frame -- the embedder renders these rigs with a native-frame mesh (NovaObjectData
// native_frame submeshes) and maps the whole rig to the camera in one container transform;
// the original's S=diag(-1,1,1) conjugation + x-negated pivots/translations in its composed
// builders are its model->render frame map, realized here at that container boundary
// instead. [orig: AnimChannel_ComputeBoneMatrices @0x410da0 (Transpose(bind) x channel,
// bind = *(channel+44) ? *(channel+44) : playing anim); AnimMap_RegisterEntity @0x40bb60
// (channel+44 = .adm slot-0 .bad, set once); BoneAnim_BuildWorldMatrices @0x40c400
// (T(-pivot) bind-inverse, translation-only hierarchy, the S*A^T*S copy loops).]
//
// model_parents (paired with shared_rest_origins): the MODEL's bone table drives the rig.
// When non-empty and sized like shared_rest_origins, the rig's row count, parent indices,
// AND pivots all come from the model -- the original never reads the .bad's bone count,
// parents, or positions on this path; the .bad contributes rotations only, paired by row
// index [orig: BoneAnim_BuildWorldMatrices @0x40c400 -- FK bounded by modelDef+52, parent
// from the modelDef+56 row's +20, pivot from +36]. Rows at/past the .bad's channel count
// take row 0's already-composed rotation (the padding loop @0x40c5a1 copies bone 0's
// matrix into every extra slot) with zero per-frame translation (the original sums
// uninitialized stack floats there -- UB, ported as zeros; divergence ledger D-INF-15).
// A row's parent equal to itself normalizes to -1 (the table stores the root's parent as
// itself; the original's in-place multiply against the model-origin root pivot is a no-op).
// ClipBone.name is empty for rows past the .bad's records -- embedders synthesize names.
// This is what makes broken BadBone.position corpora irrelevant: 12 of 43 JO viewmodel
// rigs ship zeroed/stale positions and retail renders them all (data sweep 2026-07-09).
Clip sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins = {},
                 bool model_bind = false, const BadFile *bind_source = nullptr,
                 const std::vector<int> &model_parents = {});

// Reconstruct the BadBone.position table from the MODEL's bone table + the skeleton
// .bad's bind rotations -- the corpus-exact export relation (94 rigs, <=5e-4 on every
// non-stale bone; docs/net/novaworld-net-re.md section 5.40 position-derivation
// correction):
//
//     position[i] = bind_rows[parent(i)] . (-rel.x, rel.y, rel.z)
//
// bind_rows = the parent's stored bind 3x3 (BadBone.rotation, row-major -- identical to
// the reset clip's frame-0 channel transposed); rel = the model part's parent-relative
// pivot (engine frame); the x-negation is the engine's own model->render map. Root rows
// (parent < 0 / self / past the bind's records) take the x-negated rel unrotated (zero
// on every shipped rig). Index-paired (bone i <-> part i, the runtime pairing); rows
// past the bind's bone count fall back to the unrotated form. This is what lets a rig
// whose SHIPPED positions are zeroed/stale (12 of 43 JO viewmodel rigs) rebuild the
// exact healthy table from data that never rots: the model pivots and the bind
// rotations. Mirror of pyopennova/bad_build.py bad_positions_from_model.
std::vector<Vec3> positions_from_model(const BadFile &bind_bad,
                                       const std::vector<int> &model_parents,
                                       const std::vector<Vec3> &model_rel_positions);

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
