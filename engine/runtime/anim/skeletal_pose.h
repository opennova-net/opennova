// Time-domain skeletal pose evaluation over sample_clip's baked frame table —
// the frame-window selection, interpolation, blend, BN## bone-tag parsing, and
// .bad bind-rest math the shell binding's skeletal evaluator carried
// (ADR 0028). The clip sampling itself stays anim_sample.h's; this header owns
// turning a playhead into one parent-local pose.
//
// [orig: the runtime window walk is BoneAnim_FindKeyframeAtTime @0x410220
//  (hold-last past the summed durations) feeding BoneAnim_TransformBones
//  @0x410360; the bind layer is Bone_BuildWorldMatrices @0x40c770.]
#pragma once

#include <runtime/anim/anim_sample.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::anim {

// One parent-local rigid bone transform of an evaluated pose. Clip channels
// carry no scale, so a pose bone is exactly {rotation, origin}.
struct PoseBone {
    Quat rotation;
    Vec3 origin;
};

// Sample a clip at a playhead (seconds) into parent-local bones. The pose
// table holds frame_count + 1 keys (the header counts INTERVALS; the sampler
// bakes every channel key). Loops cycle the frame_count interval windows (the
// seam key ~= key 0; the original's loop-wrap window is unwalked); one-shots
// play every window and HOLD the true final key — a one-frame clip is one
// full window of motion, not a static pose. A clip with no frames clears the
// output. [orig: BoneAnim_FindKeyframeAtTime @0x410220 — hold-last past the
// summed durations]
void eval_clip_pose(const Clip &clip, double playhead_seconds,
                    std::vector<PoseBone> &r_pose);

// Blend two evaluated poses (rotation slerp + origin lerp), weight toward the
// target. A size mismatch yields the target pose verbatim; weights outside
// (0,1) return the corresponding endpoint. Semantic states can map to the
// same clip while retaining independent playheads, so equal inputs still
// blend.
void blend_poses(const std::vector<PoseBone> &source,
                 const std::vector<PoseBone> &target, float weight,
                 std::vector<PoseBone> &r_pose);

// "BN01 Hips" -> model bone index 0; -1 when the name carries no BN## tag.
// The model bone order IS the BN order (world-wac-ai-re §14.2), but parsing
// the tag keeps husk/accessory variants correct without positional trust.
int model_bone_index_from_name(const std::string &name);

// The bone's aim-overlay class from its BN## tag (kOverlayBody when untagged
// or out of the 19-entry table — the retail default case).
uint8_t overlay_class_for_bone_name(const std::string &name);

// The parent-local BIND rest from the .bad BadBone bind matrix: origin = the
// bone's rest position raw; rotation = orthonormalized TRANSPOSE of
// (bone_bind3x3 * parent_bind3x3^-1) — the .bad bind is a row-vector engine
// matrix, and the transpose is the column-vector conversion the binding
// realized by feeding the product's rows into Godot's column-axes Basis
// constructor. Identity when degenerate; a non-invertible parent matrix
// contributes a zero product (matching the binding's ignored-invert result),
// which the determinant guard then turns into identity. r_rotation_rows is
// row-major, column-vector convention (the same rows a Godot Basis exposes).
// [orig: Bone_BuildWorldMatrices @0x40c770 bind layer]
void bind_rest_local(const ClipBone &bone, const ClipBone *parent,
                     float r_rotation_rows[9], Vec3 &r_origin);

// Rotation quat -> row-major 3x3 (column-vector convention), the exact
// counterpart of mat3_to_quat for composing evaluated poses into matrices.
void quat_to_mat3_rows(const Quat &q, float r_rows[9]);

// A person's hidden sections (entity+0x134, the dismemberment masks) as the
// bone builder applies them, for a parent-linked skeleton. Retail zeroes the
// clip matrix of every row whose bit (1 << row, x86 count) is set before the
// overlay multiply, so the row's rotation is zero and the re-anchor leaves
// only its joint: its vertices meet at the joint its parent places
// (parentMatrix * pivot), and a child's joint lands there too, since a zero
// row moves nothing by its pivot. The root re-anchors on its own zeroed row,
// whose only translation is the overlay's, the model origin. The row at the
// clip's bone count above 14 then copies row 14's finished matrix, so it
// follows the head's bit instead of its own. Every other rotation is the
// row's own clip/overlay rotation, never the parent's.
// [orig: Entity_BuildBoneTransformMatrices @0x4B1290 -- the bit test
//  @0x4B1F10..0x4B1F23, the zeroed row @0x4B1F25..0x4B1F31, the overlay
//  multiply @0x4B1F4A..0x4B1FE7, the head copy @0x4B2002..0x4B2018, the
//  re-anchor on the parent row @0x4B201E..0x4B2162]
// A Godot-style skeleton composes a child under its parent, so the rule maps
// onto three per-bone flags:
inline constexpr uint8_t kSectionRowHidden = 0x1u;   // the row's vertices meet at its joint
inline constexpr uint8_t kSectionRowCollapse = 0x2u; // hidden with every descendant hidden:
                                                     // the whole subtree meets at the joint
inline constexpr uint8_t kSectionRowAnchored = 0x4u; // the joint moves onto the parent's
                                                     // (a hidden parent, or the hidden root
                                                     // onto the model origin)
// `parents` is the skeleton's parent table (-1 = root); `clip_bones` the
// playing clip's bone count, or a value >= parents.size() for no head copy
// (a first-person view model, which the builder never sees). Returns false,
// leaving r_rows all zero, when no row is hidden.
bool section_mask_rows(const std::vector<int> &parents, uint32_t section_mask,
                       size_t clip_bones, std::vector<uint8_t> &r_rows);

} // namespace opennova::anim
