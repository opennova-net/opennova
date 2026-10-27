#pragma once

#include <runtime/renderer/material_eval.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace opennova::renderer {

class ModelPanmCache;

// The view a view-moded PANM part (rotation types 3 and 4) builds from, in its model's own
// frame (D-COL-10). Retail poses every part of a drawn entity from the entity's world matrix and
// builds those parts from the current view's inverse, the camera's axes in the world as rows
// (right, up, forward: D3D's view looks down +Z); a pose built in the model's frame takes the
// camera's axes in that frame instead. `right`, `up` and `back` are the camera's axes in the
// model root's frame as the render frame holds them (the camera looking down -back), which
// reflects the native frame's X; the result reflects them back.
// [orig: Model_TransformBoneMatrices @0x58E390 - the type-3 branch @0x58EB48..0x58EC82, the
//  type-4 branch @0x58EC8F..0x58ED91, both from matA @0x27219C0;
//  Render_SetViewAndProjectionMatrices - the view's inverse into matA @0x58D942..0x58D947;
//  BoneCallback_Generic @0x4E26D0 - the entity matrix into every input row @0x4E27AF]
threedi::ThreediMatrix4x4 panm_view_in_model_frame(const float right[3], const float up[3],
        const float back[3]);

class ModelPanmPose {
public:
    uint64_t revision() const { return revision_; }
    size_t part_count() const { return matrices_.size(); }
    // A new caller (revision <= 0) receives every part; a stale caller gets
    // the current pose of every part that moved since its last apply.
    const threedi::ThreediMatrix4x4 *changed_part(size_t part, int64_t applied_revision) const;

private:
    friend class ModelPanmCache;
    uint64_t revision_ = 0;
    std::vector<threedi::ThreediMatrix4x4> matrices_;
    std::vector<uint64_t> part_revisions_;
};

// Retail poses a drawn model's parts per submission into one shared scratch
// the next submission overwrites, so nothing per entity or per view holds a
// pose past its draw: an embedder's alternate view of a level its node does
// not show keeps that level's part pose as plain transforms off this cache.
// [orig: BoneCallback_Generic @ 0x4e26d0 - g_BoneMatrices from the entity's
//  matrix (the call @ 0x4e2780), Model_TransformBoneMatrices over it (the call
//  @ 0x4e27c1), back into g_BoneCallbackScratchMatrix (the call @ 0x4e27e5);
//  BoneCallback_AnimatedBones_World @ 0x4e3180, the same scratch (the call
//  @ 0x4e322e)]
//
// One per shared model document, main-thread only. Each authored LOD keeps
// its own evaluation. Deterministic tracks reuse time/bus matches; noise
// consumes a fresh CRT sample per submission, even at the same clock.
class ModelPanmCache {
public:
    // Invalidate on document replacement. The diagnostic serial survives.
    void clear() { lods_.clear(); }
    uint64_t evaluation_serial() const { return evaluation_serial_; }
    // The returned view is valid until the next evaluate/clear call. `view_inverse`
    // (optional, panm_view_in_model_frame) is the drawing instance's view: a level with a
    // view-moded part evaluates again whenever it differs from the last call's, so each
    // instance's draw takes its own pose, as retail's per-submission scratch does.
    const ModelPanmPose *evaluate(const threedi::Threedi3di3 &model, int lod_index,
                                 int64_t time_ms, const ControlRegisterValues &controls,
                                 const threedi::ThreediMatrix4x4 *view_inverse = nullptr);

private:
    struct LodCache {
        int64_t time_ms = std::numeric_limits<int64_t>::min();
        uint64_t ctrl_hash = 0;
        bool valid = false;
        bool has_noise = false;
        // A node of rotation type 3 or 4 (the two view modes), and the view the last
        // evaluation built from (none: identity).
        bool has_view_modes = false;
        bool view_set = false;
        threedi::ThreediMatrix4x4 view{};
        ModelPanmPose pose;
        std::vector<threedi::ThreediPartAnimation> anims;
        std::vector<threedi::ThreediMatrix4x4> base_transforms;
        std::vector<threedi::ThreediVec3> pivots;
        std::vector<threedi::ThreediMatrix4x4> node_matrices;
        std::vector<int> part_to_node;
    };
    LodCache *prepare(const threedi::Threedi3di3 &model, int lod_index);
    std::vector<LodCache> lods_;
    uint64_t evaluation_serial_ = 0;
};

} // namespace opennova::renderer
