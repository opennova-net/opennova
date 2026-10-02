// Model-level PANM pose evaluation — moved verbatim from the shell binding's
// ObjectData runtime evaluation (ADR 0028); the Godot binding's only
// residue there was Dictionary marshalling and Transform3D boxing.
#include <formats/threedi/threedi_panm_pose.h>

#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_panm_runtime.h>

#include <algorithm>

namespace opennova::threedi {

namespace {

bool track_is_live(const ThreediTransform &track) {
    return (track.control & 0xF0u) != 0;
}

bool track_uses_noise(const ThreediTransform &track) {
    return (track.control & 0xF0u) != 0 && (track.control & 0x0Fu) == 6;
}

void resolve_track_register(const Threedi3di3 &model, ThreediTransform &track) {
    if (!threedi_generator_names_register(track.control)) {
        return;
    }
    const char *name = nullptr;
    const size_t local_ordinal = track.control_param;
    if (model.ctrl.registers != nullptr && local_ordinal < model.ctrl.count) {
        name = model.ctrl.registers[local_ordinal].name;
    }
    // PANM stores a file-local CTRL index. Retail's model loader resolves the
    // parameter on every style above 0x70 before any track is sampled. Only
    // style 113 later reads the register bus; 114..117 keep the resolved ordinal
    // as their waveform phase. An absent or unknown authored name inherits
    // CtrlName_ToOrdinal's zero result and therefore aliases LOD_FRAC.
    // [orig: model CTRL loader @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290;
    //  PANM_SampleTrack @ 0x5B2270]
    track.control_param = threedi_ctrl_register_loader_ordinal(name);
}

} // namespace

bool threedi_panm_animation_is_live(const ThreediPartAnimation &anim) {
    const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
    // Spinner and the two view-derived modes are evaluated without sampling a
    // conventional track. In particular, spinner coefficients reinterpret raw
    // PANM bytes as floats and may have a zero control high nibble.
    if (rotation_type == 1 || rotation_type == 3 || rotation_type == 4)
        return true;
    if (rotation_type == 2 &&
            (track_is_live(anim.rotation_x) ||
             track_is_live(anim.rotation_y) ||
             track_is_live(anim.rotation_z)))
        return true;

    const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
    if (scale_type == 1 && track_is_live(anim.scale_x))
        return true;
    if (scale_type == 2 &&
            (track_is_live(anim.scale_x) ||
             track_is_live(anim.scale_y) ||
             track_is_live(anim.scale_z)))
        return true;

    return threedi_panm_translate_type(anim.flags) != THREEDI_TRANS_NONE &&
            track_is_live(anim.translation);
}

bool threedi_panm_animation_uses_noise(const ThreediPartAnimation &anim) {
    const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
    if (rotation_type == 2 &&
            (track_uses_noise(anim.rotation_x) ||
             track_uses_noise(anim.rotation_y) ||
             track_uses_noise(anim.rotation_z)))
        return true;
    const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
    if (scale_type == 1 && track_uses_noise(anim.scale_x))
        return true;
    if (scale_type == 2 &&
            (track_uses_noise(anim.scale_x) ||
             track_uses_noise(anim.scale_y) ||
             track_uses_noise(anim.scale_z)))
        return true;
    return threedi_panm_translate_type(anim.flags) != THREEDI_TRANS_NONE &&
            track_uses_noise(anim.translation);
}

bool threedi_panm_effective_for_lod(const Threedi3di3 &model, int lod_index,
                                    std::vector<ThreediPartAnimation> &r_nodes) {
    r_nodes.clear();
    if (model.lods == nullptr || lod_index < 0 ||
            static_cast<size_t>(lod_index) >= model.lod_count)
        return false;
    const ThreediLod &lod = model.lods[lod_index];
    if (lod.part_animation_count > 0 && lod.part_animations != nullptr) {
        r_nodes.assign(lod.part_animations,
                lod.part_animations + lod.part_animation_count);
    } else if (model.part_animation_count > 0 &&
            model.part_animations != nullptr) {
        r_nodes.assign(model.part_animations,
                model.part_animations + model.part_animation_count);
    }
    return !r_nodes.empty();
}

bool threedi_panm_lod_has_live(const Threedi3di3 &model, int lod_index) {
    if (model.lods == nullptr || lod_index < 0 ||
            static_cast<size_t>(lod_index) >= model.lod_count)
        return false;
    const ThreediLod &lod = model.lods[lod_index];
    if (lod.render_object_count == 0 || lod.render_objects == nullptr)
        return false;
    std::vector<ThreediPartAnimation> nodes;
    threedi_panm_effective_for_lod(model, lod_index, nodes);
    for (const ThreediPartAnimation &node : nodes)
        if (threedi_panm_animation_is_live(node)) return true;
    return false;
}

void threedi_panm_resolve_registers(const Threedi3di3 &model,
                                    std::vector<ThreediPartAnimation> &nodes) {
    for (ThreediPartAnimation &anim : nodes) {
        resolve_track_register(model, anim.rotation_x);
        resolve_track_register(model, anim.rotation_y);
        resolve_track_register(model, anim.rotation_z);
        resolve_track_register(model, anim.scale_x);
        resolve_track_register(model, anim.scale_y);
        resolve_track_register(model, anim.scale_z);
        resolve_track_register(model, anim.translation);
    }
}

uint32_t threedi_panm_runtime_time_ms(int64_t time_ms) {
    return time_ms < 0 ? 0u : static_cast<uint32_t>(time_ms);
}

bool threedi_panm_pose_parts(const Threedi3di3 &model, int lod_index,
                             uint32_t time_ms, const int32_t *ctrl_values,
                             std::vector<ThreediMatrix4x4> &r_matrices,
                             std::vector<uint8_t> *r_animated) {
    r_matrices.clear();
    if (r_animated != nullptr) r_animated->clear();
    if (model.lods == nullptr || lod_index < 0 ||
            static_cast<size_t>(lod_index) >= model.lod_count)
        return false;
    const ThreediLod &lod = model.lods[lod_index];
    if (lod.render_object_count == 0 || lod.render_objects == nullptr)
        return false;

    static const int32_t kZeroCtrl[THREEDI_CTRL_REGISTER_COUNT] = {};
    if (ctrl_values == nullptr) ctrl_values = kZeroCtrl;

    std::vector<ThreediPartAnimation> effective_anims;
    threedi_panm_effective_for_lod(model, lod_index, effective_anims);
    threedi_panm_resolve_registers(model, effective_anims);
    const ThreediPartAnimation *anims =
            effective_anims.empty() ? nullptr : effective_anims.data();
    const size_t node_count = effective_anims.size();

    size_t input_count = std::max(lod.render_object_count, node_count);
    for (size_t i = 0; i < node_count && anims != nullptr; ++i) {
        input_count = std::max(input_count,
                static_cast<size_t>(anims[i].subobject_index) + 1);
        input_count = std::max(input_count,
                static_cast<size_t>(anims[i].parent_subobject) + 1);
    }

    std::vector<ThreediMatrix4x4> base_transforms(input_count);
    std::vector<ThreediVec3> pivots(input_count, ThreediVec3{0, 0, 0});
    for (size_t i = 0; i < input_count; ++i) {
        threedi_mat4_identity(&base_transforms[i]);
    }
    for (size_t i = 0; i < lod.render_object_count; ++i) {
        const ThreediRenderObject &part = lod.render_objects[i];
        base_transforms[i].m[12] = part.abs[0];
        base_transforms[i].m[13] = part.abs[1];
        base_transforms[i].m[14] = part.abs[2];
        pivots[i] = ThreediVec3{part.abs[0], part.abs[1], part.abs[2]};
    }

    std::vector<ThreediMatrix4x4> panm_matrices(node_count);
    std::vector<int> part_to_node(lod.render_object_count, -1);
    if (node_count > 0 && anims != nullptr) {
        const int rc = threedi_panm_build_node_matrices(
                anims,
                node_count,
                pivots.data(),
                &model.mtrx,
                nullptr,
                base_transforms.data(),
                ThreediPanmInputs::kRestFrames,
                nullptr,
                time_ms,
                ctrl_values,
                panm_matrices.data());
        if (rc == 0) {
            for (size_t i = 0; i < node_count; ++i) {
                const uint8_t sub = anims[i].subobject_index;
                if (sub < lod.render_object_count) {
                    part_to_node[sub] = static_cast<int>(i);
                }
            }
        }
    }

    r_matrices.resize(lod.render_object_count);
    if (r_animated != nullptr) r_animated->assign(lod.render_object_count, 0);
    for (size_t i = 0; i < lod.render_object_count; ++i) {
        const int node_index = part_to_node[i];
        const bool animated = node_index >= 0 &&
                static_cast<size_t>(node_index) < panm_matrices.size();
        r_matrices[i] = animated ? panm_matrices[node_index] : base_transforms[i];
        if (r_animated != nullptr && animated) (*r_animated)[i] = 1;
    }
    return true;
}

bool threedi_panm_pose_parts_over(const Threedi3di3 &model, int lod_index,
                                  uint32_t time_ms, const int32_t *ctrl_values,
                                  const std::vector<ThreediMatrix4x4> &inputs,
                                  std::vector<ThreediMatrix4x4> &r_matrices,
                                  std::vector<uint8_t> *r_animated) {
    r_matrices = inputs;
    if (r_animated != nullptr) r_animated->clear();
    if (model.lods == nullptr || lod_index < 0 ||
            static_cast<size_t>(lod_index) >= model.lod_count)
        return false;
    const ThreediLod &lod = model.lods[lod_index];
    if (lod.render_object_count == 0 || lod.render_objects == nullptr)
        return false;
    ThreediMatrix4x4 identity;
    threedi_mat4_identity(&identity);
    r_matrices.resize(lod.render_object_count, identity);
    if (r_animated != nullptr) r_animated->assign(lod.render_object_count, 0);

    std::vector<ThreediPartAnimation> anims;
    if (!threedi_panm_effective_for_lod(model, lod_index, anims)) return false;
    threedi_panm_resolve_registers(model, anims);

    static const int32_t kZeroCtrl[THREEDI_CTRL_REGISTER_COUNT] = {};
    if (ctrl_values == nullptr) ctrl_values = kZeroCtrl;

    size_t input_count = std::max(lod.render_object_count, anims.size());
    for (const ThreediPartAnimation &anim : anims) {
        input_count = std::max(input_count, static_cast<size_t>(anim.subobject_index) + 1);
        input_count = std::max(input_count, static_cast<size_t>(anim.parent_subobject) + 1);
    }
    // The posed frame of every part, and each part's model-space pivot (the
    // ROBJ abs retail's loader stores at the part row's +36; GPM_LoadRenderModel
    // @ 0x5B5000, read @ 0x58F015..0x58F034).
    std::vector<ThreediMatrix4x4> frames(input_count, identity);
    std::vector<ThreediVec3> pivots(input_count, ThreediVec3{0, 0, 0});
    for (size_t i = 0; i < input_count && i < inputs.size(); ++i) frames[i] = inputs[i];
    for (size_t i = 0; i < lod.render_object_count; ++i) {
        const ThreediRenderObject &part = lod.render_objects[i];
        pivots[i] = ThreediVec3{part.abs[0], part.abs[1], part.abs[2]};
    }

    std::vector<ThreediMatrix4x4> built(anims.size());
    if (threedi_panm_build_node_matrices(anims.data(), anims.size(), pivots.data(), &model.mtrx,
                nullptr, frames.data(), ThreediPanmInputs::kPosedFrames, nullptr, time_ms,
                ctrl_values, built.data()) != 0)
        return false;
    // The last node naming a part drives it, as threedi_panm_pose_parts maps.
    for (size_t i = 0; i < anims.size(); ++i) {
        const uint8_t sub = anims[i].subobject_index;
        if (sub >= lod.render_object_count) continue;
        r_matrices[sub] = built[i];
        if (r_animated != nullptr) (*r_animated)[sub] = 1;
    }
    return true;
}

} // namespace opennova::threedi
