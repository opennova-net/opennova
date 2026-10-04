// ObjectData — runtime evaluation: the per-graphic PANM node-matrix cache
// behind apply_panm_to_nodes, material/anim-frame runtime, and light
// evaluation on the retail clock.
#include "object/object_data_internal.h"

#include <formats/env/env_weather.h>
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

// The weather's FLICKER / SWING registers (ObjectData::set_weather_ctrl_registers)
// and the ring copy models hash their own position into (set_weather_rings).
int32_t g_weather_ctrl_flicker = 0;
int32_t g_weather_ctrl_swing = 0;
opennova::env::WeatherOscillator g_weather_rings;
bool g_weather_rings_valid = false;

GlobalCtrlValues global_control_values_from_dict(const Dictionary &dict) {
	opennova::renderer::ModelControls controls;
	const Array keys = dict.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		controls.store(threedi_ctrl_register_ordinal(key.utf8().get_data()),
				static_cast<int64_t>(dict[keys[i]]));
	}
	return controls.runtime_values(g_weather_ctrl_flicker, g_weather_ctrl_swing);
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

void ObjectData::weather_ctrl_registers(int32_t &r_flicker, int32_t &r_swing) {
	r_flicker = g_weather_ctrl_flicker;
	r_swing = g_weather_ctrl_swing;
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

void ObjectData::set_weather_ctrl_registers(int32_t p_flicker, int32_t p_swing) {
	g_weather_ctrl_flicker = p_flicker;
	g_weather_ctrl_swing = p_swing;
}

void ObjectData::set_weather_rings(const opennova::env::WeatherOscillator &p_oscillator) {
	g_weather_rings = p_oscillator;
	g_weather_rings_valid = true;
}

void ObjectData::clear_weather_rings() {
	g_weather_rings_valid = false;
}

bool ObjectData::weather_ctrl_registers_at(int32_t p_x_q16, int32_t p_y_q16, int32_t p_z_q16,
		int32_t &r_flicker, int32_t &r_swing) {
	if (!g_weather_rings_valid) {
		return false;
	}
	const uint8_t slot = g_weather_rings.ring_slot(p_x_q16, p_y_q16, p_z_q16);
	r_flicker = g_weather_rings.amp_ring[slot];
	r_swing = g_weather_rings.osc_ring[slot];
	return true;
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
	if (!source_model_ || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= native_model().lod_count) return out;
	const auto &lod = native_model().lods[p_lod_index];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr) return out;
	const auto controls = global_control_values_from_dict(p_ctrl_values);
	std::vector<ThreediMatrix4x4> matrices;
	if (threedi_panm_pose_parts(native_model(), p_lod_index,
			threedi_panm_runtime_time_ms(p_time_ms), controls.data(), matrices, nullptr)) {
		for (size_t i = 0; i < matrices.size(); ++i)
			out[static_cast<int>(i)] = panm_transform(matrices[i]);
	}
	return out;
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
		const Array &p_nodes, int64_t p_applied_revision) const {
	if (!source_model_) return 0;
	const auto *pose = panm_cache_.evaluate(native_model(), p_lod_index, p_time_ms, p_ctrl_table);
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

int64_t ObjectData::get_panm_evaluation_serial() const {
	return static_cast<int64_t>(panm_cache_.evaluation_serial());
}

Array ObjectData::evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Array out;
	if (!source_model_ || native_model().light_count == 0) {
		return out;
	}
	const GlobalCtrlValues ctrl_values =
			global_control_values_from_dict(p_ctrl_values);
	for (size_t i = 0; i < native_model().light_count; ++i) {
		const ThreediLight &light = native_model().lights[i];
		if ((light.flags & THREEDI_LIGHT_FLAG_DISABLE_OBJECTS) != 0) {
			continue;
		}
		// The loader fixup below rewrites the phase byte, so evaluate a copy.
		ThreediLight runtime_light = light;
		if (runtime_light.style > 0x70) {
			const char *name = nullptr;
			const size_t local_ordinal = runtime_light.phase;
			if (native_model().ctrl.registers != nullptr &&
					local_ordinal < native_model().ctrl.count) {
				name = native_model().ctrl.registers[local_ordinal].name;
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
