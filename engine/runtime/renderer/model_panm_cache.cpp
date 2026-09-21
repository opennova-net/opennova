// Host-side evaluation memoization. Retail track and loader rules remain in
// formats/threedi; this cache owns reuse and changed-part bookkeeping.
#include "model_panm_cache.h"

#include <base/io/hash.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <formats/threedi/threedi_panm_runtime.h>

#include <algorithm>

namespace opennova::renderer {
namespace {

using namespace threedi;

// Hash the effective retail bus, not the caller's spelling. Unknown keys,
// case variants and duplicate aliases have already been resolved by the host.
uint64_t ctrl_hash(const ControlRegisterValues &values) {
    uint64_t h = io::kFnv1a64Offset;
    bool any_nonzero = false;
    for (size_t ordinal = 0; ordinal < values.size(); ++ordinal) {
        const uint32_t bits = static_cast<uint32_t>(values[ordinal]);
        any_nonzero |= bits != 0;
        h = io::fnv1a64_byte(h, static_cast<uint8_t>(ordinal));
        for (unsigned shift = 0; shift < 32; shift += 8)
            h = io::fnv1a64_byte(h, static_cast<uint8_t>(bits >> shift));
    }
    return any_nonzero ? h : 0;
}

bool same_affine_transform(const ThreediMatrix4x4 &a, const ThreediMatrix4x4 &b) {
    // Match the exact affine components consumed by the render adapter;
    // homogeneous-row differences do not constitute a changed node pose.
    for (size_t col = 0; col < 4; ++col)
        for (size_t row = 0; row < 3; ++row)
            if (a.m[col * 4 + row] != b.m[col * 4 + row]) return false;
    return true;
}

} // namespace

const threedi::ThreediMatrix4x4 *ModelPanmPose::changed_part(size_t part, int64_t applied_revision) const {
    const uint64_t applied = applied_revision <= 0 ? 0 : static_cast<uint64_t>(applied_revision);
    return part < matrices_.size() && part_revisions_[part] > applied ? &matrices_[part] : nullptr;
}

ModelPanmCache::LodCache *ModelPanmCache::prepare(const threedi::Threedi3di3 &model, int lod_index) {
    if (model.lods == nullptr || lod_index < 0 || static_cast<size_t>(lod_index) >= model.lod_count)
        return nullptr;
    const auto &lod = model.lods[lod_index];
    if (lod.render_object_count == 0 || lod.render_objects == nullptr) return nullptr;
    if (lods_.size() < model.lod_count) lods_.resize(model.lod_count);
    auto &c = lods_[static_cast<size_t>(lod_index)];
    if (c.valid) return &c;
    c.valid = true;
    threedi_panm_effective_for_lod(model, lod_index, c.anims);
    threedi_panm_resolve_registers(model, c.anims);
    for (const auto &anim : c.anims)
        c.has_noise = c.has_noise || threedi_panm_animation_uses_noise(anim);
    size_t input_count = std::max(static_cast<size_t>(lod.render_object_count), c.anims.size());
    for (const auto &anim : c.anims) {
        input_count = std::max(input_count, static_cast<size_t>(anim.subobject_index) + 1);
        input_count = std::max(input_count, static_cast<size_t>(anim.parent_subobject) + 1);
    }
    c.base_transforms.assign(input_count, threedi::ThreediMatrix4x4{});
    c.pivots.assign(input_count, threedi::ThreediVec3{0, 0, 0});
    for (auto &base : c.base_transforms) threedi_mat4_identity(&base);
    for (size_t i = 0; i < lod.render_object_count; ++i) {
        const auto &part = lod.render_objects[i];
        c.base_transforms[i].m[12] = part.abs[0];
        c.base_transforms[i].m[13] = part.abs[1];
        c.base_transforms[i].m[14] = part.abs[2];
        c.pivots[i] = {part.abs[0], part.abs[1], part.abs[2]};
    }
    c.node_matrices.assign(c.anims.size(), threedi::ThreediMatrix4x4{});
    c.part_to_node.assign(lod.render_object_count, -1);
    for (size_t i = 0; i < c.anims.size(); ++i) {
        const uint8_t sub = c.anims[i].subobject_index;
        if (sub < lod.render_object_count) c.part_to_node[sub] = static_cast<int>(i);
    }
    c.pose.matrices_.assign(lod.render_object_count, threedi::ThreediMatrix4x4{});
    c.pose.part_revisions_.assign(lod.render_object_count, 0);
    return &c;
}

const ModelPanmPose *ModelPanmCache::evaluate(const threedi::Threedi3di3 &model, int lod_index,
        int64_t time_ms, const ControlRegisterValues &controls) {
    auto *cache = prepare(model, lod_index);
    if (cache == nullptr) return nullptr;
    auto &c = *cache;
    const uint64_t hash = ctrl_hash(controls);
    const bool first = c.time_ms == std::numeric_limits<int64_t>::min();
    if (first || c.has_noise || c.time_ms != time_ms || c.ctrl_hash != hash) {
        ++evaluation_serial_;
        if (!c.anims.empty())
            threedi_panm_build_node_matrices(c.anims.data(), c.anims.size(), c.pivots.data(),
                    nullptr, c.base_transforms.data(), nullptr, threedi_panm_runtime_time_ms(time_ms),
                    controls.data(), c.node_matrices.data());
        bool any_changed = false;
        const uint64_t next_revision = c.pose.revision_ + 1;
        for (size_t i = 0; i < c.pose.matrices_.size(); ++i) {
            const int node_index = c.part_to_node[i];
            const auto &next = node_index >= 0 ? c.node_matrices[node_index] : c.base_transforms[i];
            if (first || !same_affine_transform(next, c.pose.matrices_[i])) {
                c.pose.matrices_[i] = next;
                c.pose.part_revisions_[i] = next_revision;
                any_changed = true;
            }
        }
        if (any_changed) c.pose.revision_ = next_revision;
        c.time_ms = time_ms;
        c.ctrl_hash = hash;
    }
    return &c.pose;
}

} // namespace opennova::renderer
