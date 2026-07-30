// NovaObjectData — runtime evaluation: the per-graphic PANM node-matrix cache
// behind apply_panm_to_nodes, material/anim-frame runtime, and light
// evaluation on the retail clock.
#include "object/nova_object_data_internal.h"

#include <renderer/light_runtime.h>
#include <renderer/material_eval.h>
#include <threedi/threedi_panm_runtime.h>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace novaobj;

namespace {

template <typename Animation>
bool panm_animation_is_live(const Animation &anim) {
	const auto track_is_live = [](const auto &track) {
		return (track.control & 0xF0u) != 0;
	};
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

template <typename Animation>
bool panm_animation_uses_noise(const Animation &anim) {
	const auto track_uses_noise = [](const auto &track) {
		return (track.control & 0xF0u) != 0 &&
				(track.control & 0x0Fu) == 6;
	};
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

uint32_t retail_runtime_time_ms(int64_t time_ms) {
	return time_ms < 0 ? 0u : static_cast<uint32_t>(time_ms);
}

std::vector<std::string> control_register_names(const ThreediModelIR &ir) {
	std::vector<std::string> names;
	names.reserve(ir.control_register_count);
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		names.emplace_back(ir.control_registers[i].name);
	}
	return names;
}

int32_t control_value_from_variant(const Variant &value) {
	const int64_t raw = static_cast<int64_t>(value);
	const uint32_t low_dword = static_cast<uint32_t>(raw);
	int32_t signed_value = 0;
	std::memcpy(&signed_value, &low_dword, sizeof(signed_value));
	return signed_value;
}

using GlobalCtrlValues = renderer::ControlRegisterValues;

GlobalCtrlValues global_control_values_from_dict(const Dictionary &dict) {
	GlobalCtrlValues values = {};
	const Array keys = dict.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		const CharString utf8 = key.utf8();
		const int ordinal = threedi_ctrl_register_ordinal(utf8.get_data());
		if (ordinal == THREEDI_CTRL_REGISTER_NOT_FOUND) {
			continue;
		}
		values[static_cast<size_t>(ordinal)] =
				control_value_from_variant(dict[keys[i]]);
	}
	return values;
}

void resolve_panm_track_register(const ThreediModelIR &ir, ThreediTransform &track) {
	if (track.control <= 0x70) {
		return;
	}
	const char *name = nullptr;
	const size_t local_ordinal = track.control_param;
	if (ir.control_registers != nullptr &&
			local_ordinal < ir.control_register_count) {
		name = ir.control_registers[local_ordinal].name;
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

void resolve_panm_registers(const ThreediModelIR &ir,
		std::vector<ThreediPartAnimation> &animations) {
	for (ThreediPartAnimation &anim : animations) {
		resolve_panm_track_register(ir, anim.rotation_x);
		resolve_panm_track_register(ir, anim.rotation_y);
		resolve_panm_track_register(ir, anim.rotation_z);
		resolve_panm_track_register(ir, anim.scale_x);
		resolve_panm_track_register(ir, anim.scale_y);
		resolve_panm_track_register(ir, anim.scale_z);
		resolve_panm_track_register(ir, anim.translation);
	}
}

Transform3D panm_matrix_to_transform(const ThreediMatrix4x4 &m) {
	const float *r = m.m;
	Transform3D t;
	t.basis[0] = Vector3(r[0], -r[4], -r[8]);
	t.basis[1] = Vector3(-r[1], r[5], r[9]);
	t.basis[2] = Vector3(-r[2], r[6], r[10]);
	t.origin = Vector3(-r[12], r[13], r[14]);
	return t;
}

} // namespace

bool NovaObjectData::_effective_panm_for_lod(int p_lod_index,
		std::vector<ThreediPartAnimation> &r_nodes) const {
	r_nodes.clear();
	if (!has_ir || ir.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= ir.lod_count)
		return false;
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_animation_count > 0 && lod.part_animations != nullptr) {
		r_nodes.resize(lod.part_animation_count);
		for (size_t i = 0; i < lod.part_animation_count; ++i)
			copy_ir_part_animation(lod.part_animations[i], r_nodes[i]);
	} else if (has_source_model &&
			source_model.part_animation_count > 0 &&
			source_model.part_animations != nullptr) {
		r_nodes.assign(source_model.part_animations,
				source_model.part_animations + source_model.part_animation_count);
	}
	return !r_nodes.empty();
}

bool NovaObjectData::has_live_panm_for_lod(int p_lod_index) const {
	if (!has_ir || ir.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= ir.lod_count)
		return false;
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_count == 0 || lod.parts == nullptr) return false;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		if (panm_animation_is_live(node)) return true;
	return false;
}

int NovaObjectData::get_live_panm_lod() const {
	if (!has_ir || ir.lods == nullptr) return -1;
	for (size_t lod_index = 0; lod_index < ir.lod_count; ++lod_index)
		if (has_live_panm_for_lod(static_cast<int>(lod_index)))
			return static_cast<int>(lod_index);
	return -1;
}

bool NovaObjectData::has_live_panm() const {
	return get_live_panm_lod() >= 0;
}

PackedInt32Array NovaObjectData::get_effective_panm_targets(int p_lod_index) const {
	PackedInt32Array out;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		out.push_back(static_cast<int32_t>(node.subobject_index));
	return out;
}

Dictionary NovaObjectData::eval_material_runtime(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return out;
	}
	ThreediMaterial material = {};
	copy_ir_material(ir.materials[p_index], material);
	const std::vector<std::string> ctrl_names = control_register_names(ir);
	const GlobalCtrlValues ctrl_values =
			global_control_values_from_dict(p_ctrl_values);
	const renderer::MaterialRuntime runtime = renderer::eval_material_runtime(
			material, retail_runtime_time_ms(p_time_ms), ctrl_names, ctrl_values);
	out["uv_transform_u"] = Vector3(runtime.uv.m00, runtime.uv.m10, runtime.uv.m20);
	out["uv_transform_v"] = Vector3(runtime.uv.m01, runtime.uv.m11, runtime.uv.m21);
	out["rgb_mod"] = Vector3(runtime.rgb_r, runtime.rgb_g, runtime.rgb_b);
	out["alpha_mod"] = runtime.alpha;
	return out;
}

int NovaObjectData::compute_anim_frame(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return 0;
	}
	ThreediMaterial material = {};
	copy_ir_material(ir.materials[p_index], material);
	const std::vector<std::string> ctrl_names = control_register_names(ir);
	const GlobalCtrlValues ctrl_values =
			global_control_values_from_dict(p_ctrl_values);
	return renderer::compute_anim_frame(material, 0,
			retail_runtime_time_ms(p_time_ms), ctrl_names, ctrl_values);
}

Dictionary NovaObjectData::evaluate_panm(int p_lod_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_count == 0 || lod.parts == nullptr) {
		return out;
	}

	const GlobalCtrlValues ctrl_table =
			global_control_values_from_dict(p_ctrl_values);

	std::vector<ThreediPartAnimation> effective_anims;
	_effective_panm_for_lod(p_lod_index, effective_anims);
	resolve_panm_registers(ir, effective_anims);
	const ThreediPartAnimation *anims =
			effective_anims.empty() ? nullptr : effective_anims.data();
	const size_t node_count = effective_anims.size();

	size_t input_count = std::max(lod.part_count, node_count);
	for (size_t i = 0; i < node_count && anims != nullptr; ++i) {
		input_count = std::max(input_count, static_cast<size_t>(anims[i].subobject_index) + 1);
		input_count = std::max(input_count, static_cast<size_t>(anims[i].parent_subobject) + 1);
	}

	std::vector<ThreediMatrix4x4> base_transforms(input_count);
	std::vector<ThreediVec3> pivots(input_count, ThreediVec3{0, 0, 0});
	for (size_t i = 0; i < input_count; ++i) {
		threedi_mat4_identity(&base_transforms[i]);
	}
	for (size_t i = 0; i < lod.part_count; ++i) {
		const ThreediIRPart &part = lod.parts[i];
		base_transforms[i].m[12] = part.abs_position[0];
		base_transforms[i].m[13] = part.abs_position[1];
		base_transforms[i].m[14] = part.abs_position[2];
		pivots[i] = ThreediVec3{part.abs_position[0], part.abs_position[1], part.abs_position[2]};
	}

	std::vector<ThreediMatrix4x4> panm_matrices(node_count);
	std::vector<int> part_to_node(lod.part_count, -1);
	if (node_count > 0 && anims != nullptr) {
		const int rc = threedi_panm_build_node_matrices(
				anims,
				node_count,
				pivots.data(),
				nullptr,
				base_transforms.data(),
				nullptr,
				retail_runtime_time_ms(p_time_ms),
				ctrl_table.data(),
				panm_matrices.data());
		if (rc == 0) {
			for (size_t i = 0; i < node_count; ++i) {
				const uint8_t sub = anims[i].subobject_index;
				if (sub < lod.part_count) {
					part_to_node[sub] = static_cast<int>(i);
				}
			}
		}
	}

	for (size_t i = 0; i < lod.part_count; ++i) {
		const int node_index = part_to_node[i];
		const ThreediMatrix4x4 &src = (node_index >= 0 && static_cast<size_t>(node_index) < panm_matrices.size())
				? panm_matrices[node_index]
				: base_transforms[i];
		out[static_cast<int>(i)] = panm_matrix_to_transform(src);
	}
	return out;
}

// Hash the effective retail bus, not the caller's spelling. Unknown keys,
// case variants, and duplicate aliases therefore share cache semantics with
// the values PANM actually consumes.
static uint64_t panm_ctrl_hash(const GlobalCtrlValues &values) {
	uint64_t h = 1469598103934665603ull;
	bool any_nonzero = false;
	for (size_t ordinal = 0; ordinal < values.size(); ++ordinal) {
		const uint32_t bits = static_cast<uint32_t>(values[ordinal]);
		any_nonzero |= bits != 0;
		h = (h ^ static_cast<uint8_t>(ordinal)) * 1099511628211ull;
		for (unsigned shift = 0; shift < 32; shift += 8) {
			h = (h ^ static_cast<uint8_t>(bits >> shift)) *
					1099511628211ull;
		}
	}
	return any_nonzero ? h : 0;
}

// (Re)build the lod-fixed evaluation state after an invalidation or LOD swap.
// The next evaluation after this re-arms a full apply (time sentinel).
bool NovaObjectData::_panm_cache_prepare(int p_lod_index) const {
	if (!has_ir || ir.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_count == 0 || lod.parts == nullptr) {
		return false;
	}
	PanmEvalCache &c = panm_cache_;
	if (c.valid && c.lod == p_lod_index) {
		return true;
	}
	c.lod = p_lod_index;
	c.time_ms = INT64_MIN; // sentinel: next evaluation marks every part changed
	c.ctrl_hash = 0;
	c.valid = true;
	_effective_panm_for_lod(p_lod_index, c.anims);
	resolve_panm_registers(ir, c.anims);
	c.has_noise = false;
	for (const ThreediPartAnimation &anim : c.anims) {
		c.has_noise = c.has_noise || panm_animation_uses_noise(anim);
	}
	size_t input_count = std::max(static_cast<size_t>(lod.part_count), c.anims.size());
	for (const ThreediPartAnimation &anim : c.anims) {
		input_count = std::max(input_count, static_cast<size_t>(anim.subobject_index) + 1);
		input_count = std::max(input_count, static_cast<size_t>(anim.parent_subobject) + 1);
	}
	c.base_transforms.assign(input_count, ThreediMatrix4x4{});
	c.pivots.assign(input_count, ThreediVec3{0, 0, 0});
	for (size_t i = 0; i < input_count; ++i) {
		threedi_mat4_identity(&c.base_transforms[i]);
	}
	for (size_t i = 0; i < lod.part_count; ++i) {
		const ThreediIRPart &part = lod.parts[i];
		c.base_transforms[i].m[12] = part.abs_position[0];
		c.base_transforms[i].m[13] = part.abs_position[1];
		c.base_transforms[i].m[14] = part.abs_position[2];
		c.pivots[i] = ThreediVec3{part.abs_position[0], part.abs_position[1], part.abs_position[2]};
	}
	c.node_matrices.assign(c.anims.size(), ThreediMatrix4x4{});
	c.part_to_node.assign(lod.part_count, -1);
	for (size_t i = 0; i < c.anims.size(); ++i) {
		const uint8_t sub = c.anims[i].subobject_index;
		if (sub < lod.part_count) {
			c.part_to_node[sub] = static_cast<int>(i);
		}
	}
	c.part_transforms.assign(lod.part_count, Transform3D());
	c.part_revision.assign(lod.part_count, 0);
	return true;
}

int64_t NovaObjectData::apply_panm_to_nodes(int p_lod_index, int64_t p_time_ms,
		const Dictionary &p_ctrl_values, const Array &p_nodes,
		int64_t p_applied_revision) const {
	if (!_panm_cache_prepare(p_lod_index)) {
		return 0;
	}
	PanmEvalCache &c = panm_cache_;
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	const GlobalCtrlValues ctrl_table =
			global_control_values_from_dict(p_ctrl_values);
	const uint64_t ctrl_hash = panm_ctrl_hash(ctrl_table);
	const bool first_eval = c.time_ms == INT64_MIN;
	if (first_eval || c.has_noise ||
			c.time_ms != p_time_ms || c.ctrl_hash != ctrl_hash) {
		++c.evaluation_serial;
		if (!c.anims.empty()) {
			threedi_panm_build_node_matrices(c.anims.data(), c.anims.size(),
					c.pivots.data(), nullptr, c.base_transforms.data(), nullptr,
					retail_runtime_time_ms(p_time_ms), ctrl_table.data(),
					c.node_matrices.data());
		}
		bool any_changed = false;
		const uint64_t next_revision = c.revision + 1;
		for (size_t i = 0; i < lod.part_count; ++i) {
			const int node_index = c.part_to_node[i];
			const Transform3D next = panm_matrix_to_transform(
					(node_index >= 0) ? c.node_matrices[node_index] : c.base_transforms[i]);
			if (first_eval || next != c.part_transforms[i]) {
				c.part_transforms[i] = next;
				c.part_revision[i] = next_revision;
				any_changed = true;
			}
		}
		if (any_changed) {
			c.revision = next_revision;
		}
		c.time_ms = p_time_ms;
		c.ctrl_hash = ctrl_hash;
	}
	const uint64_t applied =
			p_applied_revision <= 0 ? 0 : static_cast<uint64_t>(p_applied_revision);
	if (applied == c.revision) {
		return static_cast<int64_t>(c.revision);
	}
	const int node_limit =
			std::min(static_cast<int>(lod.part_count), static_cast<int>(p_nodes.size()));
	for (int i = 0; i < node_limit; ++i) {
		if (c.part_revision[i] <= applied) {
			continue;
		}
		Node3D *node = Object::cast_to<Node3D>(static_cast<Object *>(p_nodes[i]));
		if (node != nullptr) {
			node->set_transform(c.part_transforms[i]);
		}
	}
	return static_cast<int64_t>(c.revision);
}

int64_t NovaObjectData::get_panm_evaluation_serial() const {
	return static_cast<int64_t>(panm_cache_.evaluation_serial);
}

Array NovaObjectData::evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Array out;
	if (!has_ir || ir.light_count == 0) {
		return out;
	}
	const GlobalCtrlValues ctrl_values =
			global_control_values_from_dict(p_ctrl_values);
	for (size_t i = 0; i < ir.light_count; ++i) {
		const ThreediIRLight &light = ir.lights[i];
		if ((light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_OBJECTS) != 0) {
			continue;
		}
		ThreediLight runtime_light = {};
		copy_ir_light(light, runtime_light);
		if (runtime_light.style > 0x70) {
			const char *name = nullptr;
			const size_t local_ordinal = runtime_light.phase;
			if (ir.control_registers != nullptr &&
					local_ordinal < ir.control_register_count) {
				name = ir.control_registers[local_ordinal].name;
			}
			// The light loader applies the same local-name -> global-ordinal
			// rewrite as PANM. Controlled styles read that bus slot; waveform
			// styles retain the resolved ordinal as their phase byte.
			// [orig: model light fixups @ 0x5B5F4F..0x5B5F62;
			//  RgbGen_EvaluateColor @ 0x5B23D0]
			runtime_light.phase =
					threedi_ctrl_register_loader_ordinal(name);
		}
		const std::array<uint8_t, 4> color_start = {
			runtime_light.color_start[0], runtime_light.color_start[1], runtime_light.color_start[2], runtime_light.color_start[3]
		};
		const std::array<uint8_t, 4> color_end = {
			runtime_light.color_end[0], runtime_light.color_end[1], runtime_light.color_end[2], runtime_light.color_end[3]
		};
		int32_t ctrl_value = 0;
		// Light RgbGen shares the material RGB evaluator: only 113/114 read
		// the resolved global CTRL slot.
		// [orig: Light_GetPointLightParams @ 0x5A9180;
		//  RgbGen_EvaluateColor @ 0x5B23D0]
		if (runtime_light.style == 113 || runtime_light.style == 114) {
			ctrl_value = ctrl_values[runtime_light.phase];
		}
		const renderer::LightRuntime runtime = renderer::eval_light_runtime(
				runtime_light.style,
				runtime_light.phase,
				runtime_light.rate,
				color_start,
				color_end,
				retail_runtime_time_ms(p_time_ms),
				ctrl_value);
		Dictionary entry;
		entry["position"] = godot_vec3(light.offset);
		entry["color"] = Color(runtime.r, runtime.g, runtime.b, 1.0f);
		entry["intensity"] = runtime.intensity;
		entry["atten_start"] = light.attenuation_start;
		entry["atten_end"] = light.attenuation_end;
		entry["subobject"] = light.part_index;
		entry["disable_corona"] = (light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_CORONA) != 0;
		entry["disable_lightterrain"] = (light.flags & THREEDI_IR_LIGHT_FLAG_DISABLE_TERRAIN) != 0;
		entry["disable_lightobjects"] = false;
		out.push_back(entry);
	}
	return out;
}
