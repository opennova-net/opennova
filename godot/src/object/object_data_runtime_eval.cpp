// ObjectData — runtime evaluation: the per-graphic PANM node-matrix cache
// behind apply_panm_to_nodes, material/anim-frame runtime, and light
// evaluation on the retail clock.
#include "object/object_data_internal.h"

#include <runtime/renderer/light_runtime.h>
#include <runtime/renderer/material_eval.h>
#include <formats/threedi/threedi_panm_pose.h> // liveness / noise / clock (one impl with the engine)
#include <formats/threedi/threedi_panm_runtime.h>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace novaobj;

namespace {

std::vector<std::string> control_register_names(const Threedi3di3 &model) {
	std::vector<std::string> names;
	names.reserve(model.ctrl.count);
	for (uint32_t i = 0; i < model.ctrl.count; ++i) {
		names.emplace_back(model.ctrl.registers[i].name);
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

using GlobalCtrlValues = opennova::renderer::ControlRegisterValues;

// The weather's FLICKER / SWING registers (ObjectData::set_weather_ctrl_registers).
int32_t g_weather_ctrl_flicker = 0;
int32_t g_weather_ctrl_swing = 0;

GlobalCtrlValues global_control_values_from_dict(const Dictionary &dict) {
	GlobalCtrlValues values = {};
	values[THREEDI_CTRL_FLICKER] = g_weather_ctrl_flicker;
	values[THREEDI_CTRL_SWING] = g_weather_ctrl_swing;
	if (dict.is_empty()) {
		return values;
	}
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

void resolve_panm_track_register(const Threedi3di3 &model, ThreediTransform &track) {
	if (track.control <= 0x70) {
		return;
	}
	const char *name = nullptr;
	const size_t local_ordinal = track.control_param;
	if (model.ctrl.registers != nullptr &&
			local_ordinal < model.ctrl.count) {
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

void resolve_panm_registers(const Threedi3di3 &model,
		std::vector<ThreediPartAnimation> &animations) {
	for (ThreediPartAnimation &anim : animations) {
		resolve_panm_track_register(model, anim.rotation_x);
		resolve_panm_track_register(model, anim.rotation_y);
		resolve_panm_track_register(model, anim.rotation_z);
		resolve_panm_track_register(model, anim.scale_x);
		resolve_panm_track_register(model, anim.scale_y);
		resolve_panm_track_register(model, anim.scale_z);
		resolve_panm_track_register(model, anim.translation);
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

bool ObjectData::_effective_panm_for_lod(int p_lod_index,
		std::vector<ThreediPartAnimation> &r_nodes) const {
	r_nodes.clear();
	if (!has_source_model || source_model.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count)
		return false;
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (lod.part_animation_count > 0 && lod.part_animations != nullptr) {
		r_nodes.assign(lod.part_animations,
				lod.part_animations + lod.part_animation_count);
	} else if (source_model.part_animation_count > 0 &&
			source_model.part_animations != nullptr) {
		r_nodes.assign(source_model.part_animations,
				source_model.part_animations + source_model.part_animation_count);
	}
	return !r_nodes.empty();
}

bool ObjectData::has_live_panm_for_lod(int p_lod_index) const {
	if (!has_source_model || source_model.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count)
		return false;
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr) return false;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		if (threedi_panm_animation_is_live(node)) return true;
	return false;
}

int ObjectData::get_live_panm_lod() const {
	if (!has_source_model || source_model.lods == nullptr) return -1;
	for (size_t lod_index = 0; lod_index < source_model.lod_count; ++lod_index)
		if (has_live_panm_for_lod(static_cast<int>(lod_index)))
			return static_cast<int>(lod_index);
	return -1;
}

bool ObjectData::has_live_panm() const {
	return get_live_panm_lod() >= 0;
}

PackedInt32Array ObjectData::get_effective_panm_targets(int p_lod_index) const {
	PackedInt32Array out;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		out.push_back(static_cast<int32_t>(node.subobject_index));
	return out;
}

Dictionary ObjectData::eval_material_runtime(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	opennova::renderer::MaterialRuntime runtime;
	if (!eval_material_runtime_native(p_index, p_time_ms,
				runtime_control_values(p_ctrl_values), runtime)) {
		return out;
	}
	out["uv_transform_u"] = Vector3(runtime.uv.m00, runtime.uv.m10, runtime.uv.m20);
	out["uv_transform_v"] = Vector3(runtime.uv.m01, runtime.uv.m11, runtime.uv.m21);
	out["rgb_mod"] = Vector3(runtime.rgb_r, runtime.rgb_g, runtime.rgb_b);
	out["alpha_mod"] = runtime.alpha;
	return out;
}

const std::vector<std::string> &ObjectData::_runtime_control_names() const {
	if (!runtime_control_names_valid_) {
		runtime_control_names_cache_ = control_register_names(source_model);
		runtime_control_names_valid_ = true;
	}
	return runtime_control_names_cache_;
}

opennova::renderer::ControlRegisterValues ObjectData::runtime_control_values(
		const Dictionary &p_ctrl_values) {
	return global_control_values_from_dict(p_ctrl_values);
}

void ObjectData::set_weather_ctrl_registers(int32_t p_flicker, int32_t p_swing) {
	g_weather_ctrl_flicker = p_flicker;
	g_weather_ctrl_swing = p_swing;
}

bool ObjectData::eval_material_runtime_native(int p_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_values,
		opennova::renderer::MaterialRuntime &r_runtime) const {
	if (!has_source_model || p_index < 0 || static_cast<size_t>(p_index) >= source_model.material_count) {
		return false;
	}
	r_runtime = opennova::renderer::eval_material_runtime(source_model.materials[p_index],
			threedi_panm_runtime_time_ms(p_time_ms), _runtime_control_names(),
			p_ctrl_values);
	return true;
}

int ObjectData::compute_anim_frame_native(int p_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_values) const {
	if (!has_source_model || p_index < 0 ||
			static_cast<size_t>(p_index) >= source_model.material_count) {
		return 0;
	}
	return opennova::renderer::compute_anim_frame(source_model.materials[p_index], 0,
			threedi_panm_runtime_time_ms(p_time_ms), _runtime_control_names(),
			p_ctrl_values);
}

Dictionary ObjectData::evaluate_panm(int p_lod_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	if (!has_source_model || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return out;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr) {
		return out;
	}

	const GlobalCtrlValues ctrl_table =
			global_control_values_from_dict(p_ctrl_values);

	std::vector<ThreediPartAnimation> effective_anims;
	_effective_panm_for_lod(p_lod_index, effective_anims);
	resolve_panm_registers(source_model, effective_anims);
	const ThreediPartAnimation *anims =
			effective_anims.empty() ? nullptr : effective_anims.data();
	const size_t node_count = effective_anims.size();

	size_t input_count = std::max(lod.render_object_count, node_count);
	for (size_t i = 0; i < node_count && anims != nullptr; ++i) {
		input_count = std::max(input_count, static_cast<size_t>(anims[i].subobject_index) + 1);
		input_count = std::max(input_count, static_cast<size_t>(anims[i].parent_subobject) + 1);
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
				nullptr,
				base_transforms.data(),
				nullptr,
				threedi_panm_runtime_time_ms(p_time_ms),
				ctrl_table.data(),
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

	for (size_t i = 0; i < lod.render_object_count; ++i) {
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
ObjectData::PanmEvalCache *ObjectData::_panm_cache_prepare(
		int p_lod_index) const {
	if (!has_source_model || source_model.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= source_model.lod_count) {
		return nullptr;
	}
	const ThreediLod &lod = source_model.lods[p_lod_index];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr) {
		return nullptr;
	}
	if (panm_caches_.size() < source_model.lod_count) {
		panm_caches_.resize(source_model.lod_count);
	}
	PanmEvalCache &c = panm_caches_[static_cast<size_t>(p_lod_index)];
	if (c.valid && c.lod == p_lod_index) {
		return &c;
	}
	c.lod = p_lod_index;
	c.time_ms = INT64_MIN; // sentinel: next evaluation marks every part changed
	c.ctrl_hash = 0;
	c.valid = true;
	_effective_panm_for_lod(p_lod_index, c.anims);
	resolve_panm_registers(source_model, c.anims);
	c.has_noise = false;
	for (const ThreediPartAnimation &anim : c.anims) {
		c.has_noise = c.has_noise || threedi_panm_animation_uses_noise(anim);
	}
	size_t input_count = std::max(static_cast<size_t>(lod.render_object_count), c.anims.size());
	for (const ThreediPartAnimation &anim : c.anims) {
		input_count = std::max(input_count, static_cast<size_t>(anim.subobject_index) + 1);
		input_count = std::max(input_count, static_cast<size_t>(anim.parent_subobject) + 1);
	}
	c.base_transforms.assign(input_count, ThreediMatrix4x4{});
	c.pivots.assign(input_count, ThreediVec3{0, 0, 0});
	for (size_t i = 0; i < input_count; ++i) {
		threedi_mat4_identity(&c.base_transforms[i]);
	}
	for (size_t i = 0; i < lod.render_object_count; ++i) {
		const ThreediRenderObject &part = lod.render_objects[i];
		c.base_transforms[i].m[12] = part.abs[0];
		c.base_transforms[i].m[13] = part.abs[1];
		c.base_transforms[i].m[14] = part.abs[2];
		c.pivots[i] = ThreediVec3{part.abs[0], part.abs[1], part.abs[2]};
	}
	c.node_matrices.assign(c.anims.size(), ThreediMatrix4x4{});
	c.part_to_node.assign(lod.render_object_count, -1);
	for (size_t i = 0; i < c.anims.size(); ++i) {
		const uint8_t sub = c.anims[i].subobject_index;
		if (sub < lod.render_object_count) {
			c.part_to_node[sub] = static_cast<int>(i);
		}
	}
	c.part_transforms.assign(lod.render_object_count, Transform3D());
	c.part_revision.assign(lod.render_object_count, 0);
	return &c;
}

int64_t ObjectData::apply_panm_to_nodes(int p_lod_index, int64_t p_time_ms,
		const Dictionary &p_ctrl_values, const Array &p_nodes,
		int64_t p_applied_revision) const {
	PanmEvalCache *cache = _panm_cache_prepare(p_lod_index);
	if (cache == nullptr) {
		return 0;
	}
	PanmEvalCache &c = *cache;
	const ThreediLod &lod = source_model.lods[p_lod_index];
	const GlobalCtrlValues ctrl_table =
			global_control_values_from_dict(p_ctrl_values);
	const uint64_t ctrl_hash = panm_ctrl_hash(ctrl_table);
	const bool first_eval = c.time_ms == INT64_MIN;
	if (first_eval || c.has_noise ||
			c.time_ms != p_time_ms || c.ctrl_hash != ctrl_hash) {
		++panm_evaluation_serial_;
		if (!c.anims.empty()) {
			threedi_panm_build_node_matrices(c.anims.data(), c.anims.size(),
					c.pivots.data(), nullptr, c.base_transforms.data(), nullptr,
					threedi_panm_runtime_time_ms(p_time_ms), ctrl_table.data(),
					c.node_matrices.data());
		}
		bool any_changed = false;
		const uint64_t next_revision = c.revision + 1;
		for (size_t i = 0; i < lod.render_object_count; ++i) {
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
			std::min(static_cast<int>(lod.render_object_count), static_cast<int>(p_nodes.size()));
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

int64_t ObjectData::get_panm_evaluation_serial() const {
	return static_cast<int64_t>(panm_evaluation_serial_);
}

Array ObjectData::evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Array out;
	if (!has_source_model || source_model.light_count == 0) {
		return out;
	}
	const GlobalCtrlValues ctrl_values =
			global_control_values_from_dict(p_ctrl_values);
	for (size_t i = 0; i < source_model.light_count; ++i) {
		const ThreediLight &light = source_model.lights[i];
		if ((light.flags & THREEDI_LIGHT_FLAG_DISABLE_OBJECTS) != 0) {
			continue;
		}
		// The loader fixup below rewrites the phase byte, so evaluate a copy.
		ThreediLight runtime_light = light;
		if (runtime_light.style > 0x70) {
			const char *name = nullptr;
			const size_t local_ordinal = runtime_light.phase;
			if (source_model.ctrl.registers != nullptr &&
					local_ordinal < source_model.ctrl.count) {
				name = source_model.ctrl.registers[local_ordinal].name;
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
		const opennova::renderer::LightRuntime runtime = opennova::renderer::eval_light_runtime(
				runtime_light.style,
				runtime_light.phase,
				runtime_light.rate,
				color_start,
				color_end,
				threedi_panm_runtime_time_ms(p_time_ms),
				ctrl_value);
		Dictionary entry;
		entry["position"] = godot_vec3(light.offset);
		entry["color"] = Color(runtime.r, runtime.g, runtime.b, 1.0f);
		entry["intensity"] = runtime.intensity;
		entry["atten_start"] = light.atten_start;
		entry["atten_end"] = light.atten_end;
		entry["subobject"] = static_cast<int>(light.subobj_index);
		entry["disable_corona"] = (light.flags & THREEDI_LIGHT_FLAG_DISABLE_CORONA) != 0;
		entry["disable_lightterrain"] = (light.flags & THREEDI_LIGHT_FLAG_DISABLE_TERRAIN) != 0;
		entry["disable_lightobjects"] = false;
		out.push_back(entry);
	}
	return out;
}
