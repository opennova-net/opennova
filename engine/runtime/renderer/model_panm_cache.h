#pragma once

#include <runtime/renderer/material_eval.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace opennova::renderer {

class ModelPanmCache;

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

// One per shared model document, main-thread only. Each authored LOD keeps
// its own evaluation. Deterministic tracks reuse time/bus matches; noise
// consumes a fresh CRT sample per submission, even at the same clock.
class ModelPanmCache {
public:
    // Invalidate on document replacement. The diagnostic serial survives.
    void clear() { lods_.clear(); }
    uint64_t evaluation_serial() const { return evaluation_serial_; }
    // The returned view is valid until the next evaluate/clear call.
    const ModelPanmPose *evaluate(const threedi::Threedi3di3 &model, int lod_index,
                                 int64_t time_ms, const ControlRegisterValues &controls);

private:
    struct LodCache {
        int64_t time_ms = std::numeric_limits<int64_t>::min();
        uint64_t ctrl_hash = 0;
        bool valid = false;
        bool has_noise = false;
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
