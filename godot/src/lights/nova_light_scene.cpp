#include "lights/nova_light_scene.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "env/nova_weather.h"
#include "object/nova_object_model.h"

namespace godot {

namespace {

// godot (x, y, z) <-> mission (x, -z, y): the same conversion the mission
// placer and the render fixtures use.
int32_t clamp_fixed(double value) {
	if (std::isnan(value)) {
		return 0;
	}
	if (value <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
		return std::numeric_limits<int32_t>::min();
	}
	if (value >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
		return std::numeric_limits<int32_t>::max();
	}
	return static_cast<int32_t>(std::round(value));
}

int32_t clamp_int64(int64_t value) {
	if (value <= static_cast<int64_t>(std::numeric_limits<int32_t>::min())) {
		return std::numeric_limits<int32_t>::min();
	}
	if (value >= static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
		return std::numeric_limits<int32_t>::max();
	}
	return static_cast<int32_t>(value);
}

std::array<int32_t, 3> mission_fixed_from_godot(const Vector3 &world) {
	return {
		clamp_fixed(static_cast<double>(world.x) * 65536.0),
		clamp_fixed(-static_cast<double>(world.z) * 65536.0),
		clamp_fixed(static_cast<double>(world.y) * 65536.0),
	};
}

Vector3 godot_from_mission_float(const std::array<float, 3> &mission) {
	return Vector3(mission[0], mission[2], -mission[1]);
}

uint8_t color_byte(float channel) {
	const float clamped = CLAMP(channel, 0.0f, 1.0f);
	return static_cast<uint8_t>(Math::round(clamped * 255.0f));
}

int64_t encode_handle(renderer::LightHandle handle) {
	if (handle.is_null()) {
		return 0;
	}
	return (static_cast<int64_t>(handle.generation) << 16) |
			static_cast<int64_t>(handle.retail_value);
}

renderer::LightHandle decode_handle(int64_t token) {
	if (token <= 0) {
		return {};
	}
	const uint64_t bits = static_cast<uint64_t>(token);
	if ((bits >> 48) != 0) {
		return {};
	}
	return renderer::LightHandle{
		static_cast<uint16_t>(bits & 0xffffu),
		static_cast<uint32_t>(bits >> 16),
	};
}

} // namespace

int64_t LightScene::spawn_model_light(const Dictionary &p_config) {
	renderer::LightSpawnParams params;
	params.position_fixed = mission_fixed_from_godot(
			p_config.get("position", Vector3()));
	// radius = atten_end * 65536 (the spawner scale; witness map in
	// engine/runtime/renderer/light_scene.h).
	params.radius_fixed = static_cast<int32_t>(Math::round(
			static_cast<float>(p_config.get("atten_end", 0.0f)) * 65536.0f));
	// The model-light spawn passes a white record color; the authored colors
	// live in the gen block (light_scene.h witness map).
	params.rgb = {255, 255, 255};
	params.intensity = static_cast<float>(p_config.get("intensity", 1.0f));
	const int style = static_cast<int>(p_config.get("style", 0));
	if (style > 0) {
		params.has_gen = true;
		params.gen.style = static_cast<uint8_t>(style);
		params.gen.phase = static_cast<uint8_t>(
				static_cast<int>(p_config.get("phase", 0)));
		params.gen.rate = static_cast<uint16_t>(
				static_cast<int>(p_config.get("rate", 0)));
		const Color start = p_config.get("color_start", Color(1, 1, 1));
		const Color end = p_config.get("color_end", Color(1, 1, 1));
		params.gen.color_start = {color_byte(start.b), color_byte(start.g),
				color_byte(start.r), 255};
		params.gen.color_end = {color_byte(end.b), color_byte(end.g),
				color_byte(end.r), 255};
	}
	params.owner_entity = static_cast<uint64_t>(
			static_cast<int64_t>(p_config.get("owner_entity", 0)));
	params.owner_section = static_cast<int32_t>(
			static_cast<int>(p_config.get("owner_section", 0)));
	params.disable_corona = p_config.get("disable_corona", false);
	params.disable_terrain = p_config.get("disable_terrain", false);
	params.disable_objects = p_config.get("disable_objects", false);
	return encode_handle(scene_.spawn(params));
}

int64_t LightScene::spawn_glow(const Dictionary &p_config) {
	renderer::LightSpawnParams params;
	params.position_fixed = mission_fixed_from_godot(
			p_config.get("position", Vector3()));
	params.radius_fixed = static_cast<int32_t>(Math::round(
			static_cast<float>(p_config.get("radius", 0.0f)) * 65536.0f));
	const Color color = p_config.get("color", Color(1, 1, 1));
	params.rgb = {color_byte(color.r), color_byte(color.g), color_byte(color.b)};
	params.fade_mode = static_cast<int>(p_config.get("fade_mode", 1));
	params.fade_duration = static_cast<int>(p_config.get("fade_duration", -1));
	params.owner_entity = static_cast<uint64_t>(
			static_cast<int64_t>(p_config.get("owner_entity", 0)));
	params.owner_section = static_cast<int32_t>(
			static_cast<int>(p_config.get("owner_section", 0)));
	params.disable_corona = p_config.get("disable_corona", false);
	params.disable_terrain = p_config.get("disable_terrain", false);
	params.disable_objects = p_config.get("disable_objects", false);
	// Retail render flag 0x100 (the impact flash): the corona re-centers
	// radius/2 below the light (retail: AmmoDef_ProcessImpactEffect @0x40a2b3
	// sets it; EffectWorld_RenderLightCoronas @0x5ab037 reads it, see
	// docs/render/render-lighting-re.md).
	params.corona_lower_half_radius =
			p_config.get("corona_lower_half_radius", false);
	return encode_handle(scene_.spawn(params));
}

void LightScene::despawn(int64_t p_handle) {
	scene_.despawn(decode_handle(p_handle));
}

void LightScene::set_light_fade(int64_t p_handle, int p_mode, int p_duration) {
	scene_.set_fade(decode_handle(p_handle), p_mode, p_duration);
}

void LightScene::set_light_blend(int64_t p_handle, float p_amount) {
	scene_.set_blend(decode_handle(p_handle), p_amount);
}

void LightScene::advance_fixed_tick() {
	scene_.tick();
}

void LightScene::set_light_position(int64_t p_handle, const Vector3 &p_world) {
	scene_.set_position(decode_handle(p_handle), mission_fixed_from_godot(p_world));
}

bool LightScene::is_alive(int64_t p_handle) const {
	return scene_.alive(decode_handle(p_handle));
}

void LightScene::clear() {
	scene_.clear();
	clear_render_output();
}

void LightScene::clear_render_output() {
	selected_.fill(renderer::SelectedLight{});
	selected_count_ = 0;
	selection_mode_ = "none";
	owner_isolation_ = "none";
	last_models_ = 0;
	last_lit_models_ = 0;
}

int LightScene::render_frame(const Vector3 &p_camera_world,
		float p_query_radius, const Vector3 &p_ambient_scale, int p_time_ms,
		Object *p_weather) {
	// Report/debug: ONE camera-global select. The gameplay object pass is
	// render_model_frame (per-draw contexts, the witnessed shape); this path
	// keeps the owned-light-unscoped approximation for census only and
	// publishes nothing to materials.
	const std::array<int32_t, 3> center =
			mission_fixed_from_godot(p_camera_world);
	const double radius = std::isfinite(static_cast<double>(p_query_radius)) ?
			MAX(static_cast<double>(p_query_radius), 0.0) :
			static_cast<double>(std::numeric_limits<int32_t>::max()) / 65536.0;
	const int64_t half = static_cast<int64_t>(clamp_fixed(radius * 65536.0));
	std::array<int32_t, 3> qmin{};
	std::array<int32_t, 3> qmax{};
	for (int axis = 0; axis < 3; ++axis) {
		qmin[axis] = clamp_int64(static_cast<int64_t>(center[axis]) - half);
		qmax[axis] = clamp_int64(static_cast<int64_t>(center[axis]) + half);
	}
	std::array<renderer::LightHandle, renderer::LightScene::kQueryLimit> handles{};
	const size_t found = scene_.query_camera_global(qmin, qmax, handles);

	renderer::LightFlickerInputs flicker;
	flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	const Weather *weather = Object::cast_to<Weather>(p_weather);
	if (weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				weather->runtime().core().oscillator;
		flicker.amp_ring = oscillator.amp_ring;
		flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		flicker.ring_index = oscillator.ring_index;
	}
	const std::array<float, 3> ambient = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	renderer::LightSelectionOptions options;
	options.target = renderer::LightSelectionTarget::Objects;
	options.admit_owned_unscoped = true;
	selected_count_ = scene_.select(handles.data(), found,
			renderer::LightActiveGroups{}, options, ambient, flicker,
			/*d3d_light_path=*/true, selected_);
	selection_mode_ = "camera_global_objects";
	owner_isolation_ = "unavailable";
	return static_cast<int>(selected_count_);
}

int LightScene::render_model_frame(const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities,
		const Vector3 &p_ambient_scale, int p_time_ms, Object *p_weather) {
	renderer::LightFlickerInputs flicker;
	flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	const Weather *weather = Object::cast_to<Weather>(p_weather);
	if (weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				weather->runtime().core().oscillator;
		flicker.amp_ring = oscillator.amp_ring;
		flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		flicker.ring_index = oscillator.ring_index;
	}
	const std::array<float, 3> ambient = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	renderer::LightSelectionOptions options;
	options.target = renderer::LightSelectionTarget::Objects;
	options.admit_owned_unscoped = false;

	const int64_t model_count = p_models.size();
	std::vector<ObjectModel *> models;
	std::vector<renderer::LightDrawContext> draws;
	models.reserve(static_cast<size_t>(model_count));
	draws.reserve(static_cast<size_t>(model_count));
	for (int64_t i = 0; i < model_count; ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(
				static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		const AABB world_bounds = model->get_world_bounds();
		renderer::LightDrawContext draw;
		const std::array<int32_t, 3> min_fixed = mission_fixed_from_godot(
				world_bounds.position);
		const std::array<int32_t, 3> max_fixed = mission_fixed_from_godot(
				world_bounds.position + world_bounds.size);
		for (int axis = 0; axis < 3; ++axis) {
			// The godot->mission fold negates one axis; re-order per axis.
			draw.aabb_min_fixed[axis] = MIN(min_fixed[axis], max_fixed[axis]);
			draw.aabb_max_fixed[axis] = MAX(min_fixed[axis], max_fixed[axis]);
		}
		// Owners are parallel to the INPUT array, not the filtered list.
		draw.groups.owner_group_entity = i < p_owner_entities.size()
				? static_cast<uint64_t>(
						static_cast<int64_t>(p_owner_entities[i]))
				: 0;
		models.push_back(model);
		draws.push_back(draw);
	}
	std::vector<renderer::LightDrawSelection> selections(draws.size());
	scene_.select_for_draws(draws.data(), draws.size(), options, ambient,
			flicker, /*d3d_light_path=*/true, selections.data());
	int lit_models = 0;
	for (size_t i = 0; i < models.size(); ++i) {
		const renderer::LightDrawSelection &selection = selections[i];
		Vector4 posr[renderer::LightScene::kSelectLimit];
		Vector4 color[renderer::LightScene::kSelectLimit];
		for (size_t light = 0; light < selection.count; ++light) {
			const renderer::SelectedLight &selected = selection.lights[light];
			const Vector3 world = godot_from_mission_float(selected.position);
			posr[light] = Vector4(world.x, world.y, world.z,
					selected.attenuation[2]);
			color[light] = Vector4(selected.color[0], selected.color[1],
					selected.color[2], selected.range);
		}
		models[i]->apply_point_light_selection(
				static_cast<int>(selection.count), posr, color);
		if (selection.count > 0) {
			++lit_models;
		}
	}
	selection_mode_ = "per_model_objects";
	owner_isolation_ = "per_model";
	last_models_ = static_cast<int>(models.size());
	last_lit_models_ = lit_models;
	return lit_models;
}

TypedArray<Dictionary> LightScene::collect_corona_rows(
		const Vector3 &p_camera_pos, const Vector3 &p_camera_forward,
		const Vector3 &p_ambient_scale, int p_time_ms, int p_frame_index,
		Object *p_weather, const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Dictionary &p_fog) {
	renderer::LightCoronaFrameInputs inputs;
	// Owner visible-section masks from the same model/owner walk the
	// per-model light pass runs: a model with an occlusion verdict (mask
	// != -1) contributes its owner row; everything else passes the gate
	// like retail's non-pool-2 owners (retail: Terrain_IsBuildingSectionBitSet
	// @0x5c6960 returns TRUE outside the mask array, see
	// docs/render/render-lighting-re.md).
	std::vector<renderer::LightCoronaOwnerMask> owner_masks;
	const int64_t model_count = p_models.size();
	owner_masks.reserve(static_cast<size_t>(model_count));
	for (int64_t i = 0; i < model_count && i < p_owner_entities.size(); ++i) {
		const ObjectModel *model = Object::cast_to<ObjectModel>(
				static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		const int64_t mask = model->get_section_visibility_mask();
		if (mask == -1) {
			continue;
		}
		renderer::LightCoronaOwnerMask row;
		row.owner_entity = static_cast<uint64_t>(
				static_cast<int64_t>(p_owner_entities[i]));
		row.section_mask = static_cast<uint32_t>(mask & 0xFFFFFFFF);
		owner_masks.push_back(row);
	}
	inputs.owner_masks = owner_masks.data();
	inputs.owner_mask_count = owner_masks.size();
	// The fog-to-black fold (retail: CD3DDevice_SetFogAndBlendMode(dev, 2)
	// @0x5aafb6, see docs/render/render-lighting-re.md): primary fog params,
	// color forced black engine-side.
	inputs.fog_enabled = p_fog.get("enabled", false);
	inputs.fog_type = static_cast<int32_t>(
			static_cast<int>(p_fog.get("type", 0)));
	inputs.fog_start = static_cast<float>(p_fog.get("start", 0.0f));
	inputs.fog_end = static_cast<float>(p_fog.get("end", 0.0f));
	inputs.camera_fixed = mission_fixed_from_godot(p_camera_pos);
	// The camera depth plane in mission space: depth grows in front of the
	// camera, zero at the camera origin (the batch-sort plane retail feeds
	// the fade (retail: @0x5ab2f8..0x5ab33c); the small near-plane offset is
	// folded into the clamp).
	const Vector3 forward = p_camera_forward.normalized();
	const std::array<float, 3> normal_mission = {
		static_cast<float>(forward.x),
		static_cast<float>(-forward.z),
		static_cast<float>(forward.y),
	};
	const std::array<int32_t, 3> &cam = inputs.camera_fixed;
	inputs.depth_plane_normal = normal_mission;
	inputs.depth_plane_w =
			-(normal_mission[0] * static_cast<float>(cam[0]) / 65536.0f +
					normal_mission[1] * static_cast<float>(cam[1]) / 65536.0f +
					normal_mission[2] * static_cast<float>(cam[2]) / 65536.0f);
	inputs.ambient_scale = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	inputs.frame_index = static_cast<uint32_t>(p_frame_index);
	inputs.flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	const Weather *weather = Object::cast_to<Weather>(p_weather);
	if (weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				weather->runtime().core().oscillator;
		inputs.flicker.amp_ring = oscillator.amp_ring;
		inputs.flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		inputs.flicker.ring_index = oscillator.ring_index;
	}
	std::vector<renderer::LightCoronaQuad> quads;
	scene_.collect_corona_quads(inputs, quads);
	TypedArray<Dictionary> rows;
	for (const renderer::LightCoronaQuad &quad : quads) {
		Dictionary row;
		row["position"] = godot_from_mission_float(quad.center);
		row["half_size"] = quad.half_size;
		row["color"] = Color(quad.rgb[0], quad.rgb[1], quad.rgb[2]);
		rows.push_back(row);
	}
	return rows;
}

int LightScene::live_count() const {
	return static_cast<int>(scene_.inspect().live);
}

Dictionary LightScene::get_report() const {
	const renderer::LightSceneReport report = scene_.inspect();
	Dictionary out;
	out["live"] = static_cast<int>(report.live);
	out["high_water"] = static_cast<int>(report.high_water);
	out["last_query"] = static_cast<int>(report.last_query);
	out["selected"] = static_cast<int>(selected_count_);
	out["selection_mode"] = selection_mode_;
	out["owner_isolation"] = owner_isolation_;
	out["models"] = last_models_;
	out["lit_models"] = last_lit_models_;
	TypedArray<Dictionary> rows;
	for (size_t i = 0; i < selected_count_; ++i) {
		const renderer::SelectedLight &light = selected_[i];
		Dictionary row;
		row["position"] = godot_from_mission_float(light.position);
		row["color"] = Color(light.color[0], light.color[1], light.color[2]);
		row["range"] = light.range;
		row["atten2"] = light.attenuation[2];
		row["handle"] = encode_handle(light.handle);
		row["retail_handle"] = static_cast<int>(light.handle.retail_value);
		rows.push_back(row);
	}
	out["rows"] = rows;
	return out;
}

void LightScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("spawn_model_light", "config"),
			&LightScene::spawn_model_light);
	ClassDB::bind_method(D_METHOD("spawn_glow", "config"),
			&LightScene::spawn_glow);
	ClassDB::bind_method(D_METHOD("despawn", "handle"), &LightScene::despawn);
	ClassDB::bind_method(D_METHOD("set_light_position", "handle", "world"),
			&LightScene::set_light_position);
	ClassDB::bind_method(D_METHOD("set_light_fade", "handle", "mode", "duration"),
			&LightScene::set_light_fade);
	ClassDB::bind_method(D_METHOD("set_light_blend", "handle", "amount"),
			&LightScene::set_light_blend);
	ClassDB::bind_method(D_METHOD("is_alive", "handle"), &LightScene::is_alive);
	ClassDB::bind_method(D_METHOD("clear"), &LightScene::clear);
	ClassDB::bind_method(D_METHOD("clear_render_output"),
			&LightScene::clear_render_output);
	ClassDB::bind_method(D_METHOD("advance_fixed_tick"),
			&LightScene::advance_fixed_tick);
	ClassDB::bind_method(D_METHOD("render_frame", "camera_world",
			"query_radius", "ambient_scale", "time_ms", "weather"),
			&LightScene::render_frame);
	ClassDB::bind_method(D_METHOD("render_model_frame", "models",
			"owner_entities", "ambient_scale", "time_ms", "weather"),
			&LightScene::render_model_frame);
	ClassDB::bind_method(D_METHOD("collect_corona_rows", "camera_pos",
			"camera_forward", "ambient_scale", "time_ms", "frame_index",
			"weather", "models", "owner_entities", "fog"),
			&LightScene::collect_corona_rows);
	ClassDB::bind_method(D_METHOD("live_count"), &LightScene::live_count);
	ClassDB::bind_method(D_METHOD("get_report"), &LightScene::get_report);
}

} // namespace godot
