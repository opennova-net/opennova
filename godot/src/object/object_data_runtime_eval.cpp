// ObjectData — runtime evaluation: the per-graphic PANM node-matrix cache
// behind apply_panm_to_nodes, material/anim-frame runtime, and light
// evaluation on the retail clock.
#include "object/object_data_internal.h"

#include <runtime/environment/weather_ctrl_registers.h>
#include <runtime/renderer/material_eval.h>
#include <runtime/renderer/model_controls.h>
#include <formats/threedi/threedi_panm_pose.h> // liveness / noise / clock (one impl with the engine)
#include <formats/threedi/threedi_panm_runtime.h>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <vector>

using namespace novaobj;
using namespace opennova::threedi;

namespace {

std::vector<std::string> control_register_names(const Threedi3di3 &model) {
	std::vector<std::string> names;
	names.reserve(model.ctrl.count);
	for (uint32_t i = 0; i < model.ctrl.count; ++i) {
		names.emplace_back(model.ctrl.registers[i].name);
	}
	return names;
}

using GlobalCtrlValues = opennova::renderer::ControlRegisterValues;

GlobalCtrlValues global_control_values_from_dict(const Dictionary &dict) {
	opennova::renderer::ModelControls controls;
	const Array keys = dict.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		controls.store(threedi_ctrl_register_ordinal(key.utf8().get_data()),
				static_cast<int64_t>(dict[keys[i]]));
	}
	// The weather's FLICKER / SWING pair (engine environment/weather_ctrl_registers.h).
	int32_t flicker = 0;
	int32_t swing = 0;
	opennova::env::weather_ctrl_registers().pair(flicker, swing);
	return controls.runtime_values(flicker, swing);
}

} // namespace

Transform3D ObjectData::panm_transform(const ThreediMatrix4x4 &p_matrix) {
	const float *r = p_matrix.m;
	Transform3D t;
	t.basis[0] = Vector3(r[0], -r[4], -r[8]);
	t.basis[1] = Vector3(-r[1], r[5], r[9]);
	t.basis[2] = Vector3(-r[2], r[6], r[10]);
	t.origin = Vector3(-r[12], r[13], r[14]);
	return t;
}

ThreediMatrix4x4 ObjectData::panm_matrix(const Transform3D &p_transform) {
	// The inverse of panm_transform: basis row i, column j is the native
	// element (j, i) with X flipped on both sides.
	static const float kFlip[3] = {-1.0f, 1.0f, 1.0f};
	ThreediMatrix4x4 m;
	threedi_mat4_identity(&m);
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			m.m[j * 4 + i] = kFlip[i] * kFlip[j] *
					static_cast<float>(p_transform.basis.rows[i][j]);
		}
	}
	m.m[12] = -static_cast<float>(p_transform.origin.x);
	m.m[13] = static_cast<float>(p_transform.origin.y);
	m.m[14] = static_cast<float>(p_transform.origin.z);
	return m;
}

bool ObjectData::has_live_panm_for_lod(int p_lod_index) const {
	return source_model_ && threedi_panm_lod_has_live(native_model(), p_lod_index);
}

int ObjectData::get_live_panm_lod() const {
	if (!source_model_ || native_model().lods == nullptr) return -1;
	for (size_t lod_index = 0; lod_index < native_model().lod_count; ++lod_index)
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
	if (source_model_) threedi_panm_effective_for_lod(native_model(), p_lod_index, nodes);
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
	out["reflect_color"] = Vector4(runtime.reflect[0], runtime.reflect[1],
			runtime.reflect[2], runtime.reflect[3]);
	out["alpha_mod"] = runtime.alpha;
	return out;
}

const std::vector<std::string> &ObjectData::_runtime_control_names() const {
	if (!runtime_control_names_valid_) {
		runtime_control_names_cache_ = control_register_names(native_model());
		runtime_control_names_valid_ = true;
	}
	return runtime_control_names_cache_;
}

opennova::renderer::ControlRegisterValues ObjectData::runtime_control_values(
		const Dictionary &p_ctrl_values) {
	return global_control_values_from_dict(p_ctrl_values);
}

bool ObjectData::uses_weather_ctrl_registers() const {
	for (const std::string &name : _runtime_control_names()) {
		if (name == threedi_ctrl_register_name(THREEDI_CTRL_FLICKER) ||
				name == threedi_ctrl_register_name(THREEDI_CTRL_SWING)) {
			return true;
		}
	}
	return false;
}

bool ObjectData::eval_material_runtime_native(int p_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_values,
		opennova::renderer::MaterialRuntime &r_runtime) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().material_count) {
		return false;
	}
	r_runtime = opennova::renderer::eval_material_runtime(native_model().materials[p_index],
			threedi_panm_runtime_time_ms(p_time_ms), _runtime_control_names(),
			p_ctrl_values);
	return true;
}

bool ObjectData::material_static_runtime_native(int p_index,
		opennova::renderer::MaterialRuntime &r_runtime) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().material_count) {
		return false;
	}
	r_runtime = opennova::renderer::material_static_runtime(native_model().materials[p_index]);
	return true;
}

bool ObjectData::material_runtime_dynamic_native(int p_index) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().material_count) {
		return false;
	}
	return opennova::renderer::material_runtime_is_dynamic(native_model().materials[p_index]);
}

int ObjectData::compute_anim_frame_native(int p_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_values) const {
	if (!source_model_ || p_index < 0 ||
			static_cast<size_t>(p_index) >= native_model().material_count) {
		return 0;
	}
	return opennova::renderer::compute_anim_frame(native_model().materials[p_index], 0,
			threedi_panm_runtime_time_ms(p_time_ms), _runtime_control_names(),
			p_ctrl_values);
}

Dictionary ObjectData::evaluate_panm(int p_lod_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	std::vector<Transform3D> parts;
	if (evaluate_panm_viewed(p_lod_index, p_time_ms, p_ctrl_values, nullptr, parts)) {
		for (size_t i = 0; i < parts.size(); ++i)
			out[static_cast<int>(i)] = parts[i];
	}
	return out;
}

bool ObjectData::evaluate_panm_viewed(int p_lod_index, int64_t p_time_ms,
		const Dictionary &p_ctrl_values, const ThreediMatrix4x4 *p_view_inverse,
		std::vector<Transform3D> &r_parts) const {
	r_parts.clear();
	if (!source_model_ || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= native_model().lod_count) return false;
	const auto &lod = native_model().lods[p_lod_index];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr) return false;
	const auto controls = global_control_values_from_dict(p_ctrl_values);
	std::vector<ThreediMatrix4x4> matrices;
	if (!threedi_panm_pose_parts(native_model(), p_lod_index,
			threedi_panm_runtime_time_ms(p_time_ms), controls.data(), matrices, nullptr,
			p_view_inverse))
		return false;
	r_parts.reserve(matrices.size());
	for (const ThreediMatrix4x4 &matrix : matrices) r_parts.push_back(panm_transform(matrix));
	return true;
}

int64_t ObjectData::apply_panm_to_nodes(int p_lod_index, int64_t p_time_ms,
		const Dictionary &p_ctrl_values, const Array &p_nodes,
		int64_t p_applied_revision) const {
	return apply_panm_to_nodes_table(p_lod_index, p_time_ms,
			global_control_values_from_dict(p_ctrl_values), p_nodes,
			p_applied_revision);
}

int64_t ObjectData::apply_panm_to_nodes_table(int p_lod_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_table,
		const Array &p_nodes, int64_t p_applied_revision,
		const ThreediMatrix4x4 *p_view_inverse) const {
	if (!source_model_) return 0;
	const auto *pose = panm_cache_.evaluate(native_model(), p_lod_index, p_time_ms, p_ctrl_table,
			p_view_inverse);
	if (pose == nullptr) return 0;
	const size_t count = std::min(pose->part_count(), static_cast<size_t>(p_nodes.size()));
	for (size_t i = 0; i < count; ++i) {
		const auto *matrix = pose->changed_part(i, p_applied_revision);
		if (matrix == nullptr) continue;
		Node3D *node = Object::cast_to<Node3D>(static_cast<Object *>(p_nodes[static_cast<int64_t>(i)]));
		if (node != nullptr) node->set_transform(panm_transform(*matrix));
	}
	return static_cast<int64_t>(pose->revision());
}

int64_t ObjectData::apply_panm_to_transforms(int p_lod_index, int64_t p_time_ms,
		const opennova::renderer::ControlRegisterValues &p_ctrl_table,
		std::vector<Transform3D> &r_parts, int64_t p_applied_revision,
		const ThreediMatrix4x4 *p_view_inverse) const {
	if (!source_model_) return 0;
	const auto *pose = panm_cache_.evaluate(native_model(), p_lod_index, p_time_ms, p_ctrl_table,
			p_view_inverse);
	if (pose == nullptr) return 0;
	const size_t count = std::min(pose->part_count(), r_parts.size());
	for (size_t i = 0; i < count; ++i) {
		const auto *matrix = pose->changed_part(i, p_applied_revision);
		if (matrix != nullptr) r_parts[i] = panm_transform(*matrix);
	}
	return static_cast<int64_t>(pose->revision());
}

int64_t ObjectData::get_panm_evaluation_serial() const {
	return static_cast<int64_t>(panm_cache_.evaluation_serial());
}

Array ObjectData::evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	// The LGHT walk, the loader's phase fixup and the evaluation are the
	// engine's (renderer/material_eval.h eval_model_lights); the binding
	// marshals each light's record fields beside its colour.
	Array out;
	if (!source_model_ || native_model().light_count == 0) {
		return out;
	}
	for (const opennova::renderer::ModelLightEval &eval : opennova::renderer::eval_model_lights(
				 native_model(), p_time_ms, global_control_values_from_dict(p_ctrl_values))) {
		const ThreediLight &light = native_model().lights[eval.index];
		Dictionary entry;
		entry["position"] = godot_vec3(light.offset);
		entry["color"] = Color(eval.runtime.r, eval.runtime.g, eval.runtime.b, 1.0f);
		entry["intensity"] = eval.runtime.intensity;
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
