#include "lights/light_scene.h"
#include "util/color_convert.h"
#include "lights/effect_light_report.h"
#include "util/axes.h"

#include <array>
#include <base/io/fixed.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <godot_cpp/classes/rendering_server.hpp>

#include "env/weather.h"
#include "object/env_light_state.h"
#include "object/object_model.h"

#include <runtime/renderer/light_draw_frame.h>

namespace godot {

namespace {

// The objects-target select's frame inputs: the env light-state gain and the
// weather's flicker ring (renderer::LightFrameInputs).
opennova::renderer::LightFrameInputs frame_inputs(int p_time_ms, const Weather *p_weather,
		const Vector3 &p_ambient_scale) {
	opennova::renderer::LightFrameInputs inputs;
	inputs.flicker = opennova::renderer::light_flicker_inputs(
			p_weather != nullptr ? &p_weather->runtime().core().oscillator : nullptr,
			static_cast<uint32_t>(p_time_ms));
	inputs.ambient = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	return inputs;
}

void collect_entity_light_models(ObjectModel *model, std::vector<ObjectModel *> &out) {
	if (!model->is_visible_in_tree()) return;
	out.push_back(model);
	// Avatar heads and individual husks are separate render models beneath
	// one entity. Retail queries before these submits, never from their poses.
	// The native EntityLightQuery contract owns this shared source.
	for (int i = 0; i < model->get_child_count(); ++i)
		if (ObjectModel *part = Object::cast_to<ObjectModel>(model->get_child(i)))
			collect_entity_light_models(part, out);
}

// The same entity's models the weapon Inset pass draws through their own
// twins; a model it draws on its node shares the main pass's selection.
void collect_inset_entity_light_models(ObjectModel *model, std::vector<ObjectModel *> &out) {
	if (model->get_view_twin_count() > 0) out.push_back(model);
	for (int i = 0; i < model->get_child_count(); ++i)
		if (ObjectModel *part = Object::cast_to<ObjectModel>(model->get_child(i)))
			collect_inset_entity_light_models(part, out);
}

// A building's per-ROBJ rows in one view: the parts its node draws, or the
// parts its Inset twins draw (the ROBJ alone selects; the bounds are unread).
void collect_view_draw_parts(ObjectModel *model, bool inset_view,
		std::vector<ObjectModel::PointLightDrawPart> &out) {
	if (!inset_view) {
		model->collect_point_light_draw_parts(out);
		return;
	}
	std::vector<int32_t> robjs;
	model->collect_inset_point_light_draw_parts(robjs);
	out.clear();
	for (const int32_t robj : robjs)
		out.push_back(ObjectModel::PointLightDrawPart{robj, AABB()});
}

uint8_t color_byte(float channel) {
	const float clamped = CLAMP(channel, 0.0f, 1.0f);
	return static_cast<uint8_t>(Math::round(clamped * 255.0f));
}

int64_t encode_handle(opennova::renderer::LightHandle handle) {
	if (handle.is_null()) {
		return 0;
	}
	return (static_cast<int64_t>(handle.generation) << 16) |
			static_cast<int64_t>(handle.retail_value);
}

opennova::renderer::LightHandle decode_handle(int64_t token) {
	if (token <= 0) {
		return {};
	}
	const uint64_t bits = static_cast<uint64_t>(token);
	if ((bits >> 48) != 0) {
		return {};
	}
	return opennova::renderer::LightHandle{
		static_cast<uint16_t>(bits & 0xffffu),
		static_cast<uint32_t>(bits >> 16),
	};
}

} // namespace

int64_t LightScene::spawn_model_light(const Ref<ModelLightSpawn> &p_config) {
	if (p_config.is_null()) return 0;
	opennova::renderer::LightSpawnParams params;
	params.position_fixed = godot_to_mission_q16(p_config->get_position());
	// radius = atten_end * 65536 (the spawner scale; witness map in
	// engine/runtime/renderer/light_scene.h).
	params.radius_fixed = static_cast<int32_t>(Math::round(
			p_config->get_atten_end() * opennova::io::kFp16One));
	// The model-light spawn passes a white record color; the authored colors
	// live in the gen block (light_scene.h witness map).
	params.rgb = {255, 255, 255};
	params.intensity = p_config->get_intensity();
	const int style = p_config->get_style();
	if (style > 0) {
		params.has_gen = true;
		params.gen.style = static_cast<uint8_t>(style);
		params.gen.phase = static_cast<uint8_t>(p_config->get_phase());
		params.gen.rate = static_cast<uint16_t>(p_config->get_rate());
		const Color start = p_config->get_color_start();
		const Color end = p_config->get_color_end();
		params.gen.color_start = {color_byte(start.b), color_byte(start.g),
				color_byte(start.r), 255};
		params.gen.color_end = {color_byte(end.b), color_byte(end.g),
				color_byte(end.r), 255};
	}
	// The owner attach is the portable policy (opennova::renderer::resolve_model_light_owner
	// carries the witness): the authored subobject wins, else the containing
	// blink box, else the record lights the world. The caller supplies facts
	// only — the record's attach bone, the spawning entity and whether it is a
	// building, and the blink owner its position resolved to.
	opennova::renderer::ModelLightOwnerInputs owner_inputs;
	owner_inputs.attach_bone = static_cast<uint8_t>(p_config->get_attach_bone() & 0xFF);
	owner_inputs.spawning_entity = static_cast<uint64_t>(p_config->get_spawning_entity());
	owner_inputs.spawner_is_building = p_config->get_spawner_is_building();
	owner_inputs.blink_owner_entity = static_cast<uint64_t>(p_config->get_blink_owner_entity());
	owner_inputs.blink_section = static_cast<int32_t>(p_config->get_blink_section());
	owner_inputs.blink_hit = owner_inputs.blink_owner_entity != 0;
	const opennova::renderer::ModelLightOwner owner =
			opennova::renderer::resolve_model_light_owner(owner_inputs);
	params.owner_entity = owner.entity;
	params.owner_section = owner.section;
	params.disable_corona = p_config->get_disable_corona();
	params.disable_terrain = p_config->get_disable_terrain();
	params.disable_objects = p_config->get_disable_objects();
	return encode_handle(scene_.spawn(params));
}

int64_t LightScene::spawn_glow(const Ref<GlowSpawn> &p_config) {
	if (p_config.is_null()) return 0;
	opennova::renderer::LightSpawnParams params;
	params.position_fixed = godot_to_mission_q16(p_config->get_position());
	params.radius_fixed = static_cast<int32_t>(Math::round(
			p_config->get_radius() * opennova::io::kFp16One));
	const Color color = p_config->get_color();
	params.rgb = {color_byte(color.r), color_byte(color.g), color_byte(color.b)};
	params.fade_mode = p_config->get_fade_mode();
	params.fade_duration = p_config->get_fade_duration();
	params.owner_entity = static_cast<uint64_t>(p_config->get_owner_entity());
	params.owner_section = static_cast<int32_t>(p_config->get_owner_section());
	params.disable_corona = p_config->get_disable_corona();
	params.disable_terrain = p_config->get_disable_terrain();
	params.disable_objects = p_config->get_disable_objects();
	// Retail render flag 0x100 (the impact flash): the corona re-centers
	// radius/2 below the light (renderer::LightSpawnParams::corona_lower_half_radius
	// carries the witness).
	params.corona_lower_half_radius = p_config->get_corona_lower_half_radius();
	return encode_handle(scene_.spawn(params));
}

int64_t LightScene::fire_muzzle_glow(int64_t p_cached, const Vector3 &p_muzzle, int64_t p_shooter) {
	return encode_handle(scene_.fire_muzzle_glow(decode_handle(p_cached),
			godot_to_mission_q16(p_muzzle), static_cast<uint64_t>(p_shooter)));
}

int64_t LightScene::spawn_impact_flash(const Vector3 &p_impact, float p_radius, const Color &p_color,
		int p_ticks) {
	if (p_radius <= 0.0f) return 0;
	return encode_handle(scene_.spawn(opennova::renderer::impact_flash_spawn(
			godot_to_mission_q16(p_impact),
			static_cast<int32_t>(Math::round(p_radius * opennova::io::kFp16One)),
			{ color_byte(p_color.r), color_byte(p_color.g), color_byte(p_color.b) }, p_ticks)));
}

int64_t LightScene::spawn_death_flash(const Vector3 &p_position, float p_radius) {
	if (p_radius <= 0.0f) return 0;
	return encode_handle(scene_.spawn(opennova::renderer::death_flash_spawn(
			godot_to_mission_q16(p_position),
			static_cast<int32_t>(Math::round(p_radius * opennova::io::kFp16One)))));
}

void LightScene::despawn(int64_t p_handle) {
	scene_.despawn(decode_handle(p_handle));
}

void LightScene::set_light_fade(int64_t p_handle, int p_mode, int p_duration) {
	scene_.set_fade(decode_handle(p_handle), p_mode, p_duration);
}

void LightScene::set_light_owner(int64_t p_handle, int64_t p_owner_entity,
		int p_owner_section) {
	scene_.set_owner(decode_handle(p_handle),
			static_cast<uint64_t>(p_owner_entity),
			static_cast<int32_t>(p_owner_section));
}

void LightScene::set_light_blend(int64_t p_handle, float p_amount) {
	scene_.set_blend(decode_handle(p_handle), p_amount);
}

void LightScene::advance_fixed_tick() {
	scene_.tick();
}

void LightScene::set_light_position(int64_t p_handle, const Vector3 &p_world) {
	scene_.set_position(decode_handle(p_handle), godot_to_mission_q16(p_world));
}

bool LightScene::is_alive(int64_t p_handle) const {
	return scene_.alive(decode_handle(p_handle));
}

void LightScene::clear() {
	scene_.clear();
	clear_render_output();
}

void LightScene::clear_render_output() {
	selected_.fill(opennova::renderer::SelectedLight{});
	selected_count_ = 0;
	selection_mode_ = "none";
	owner_isolation_ = "none";
	last_models_ = 0;
	last_lit_models_ = 0;
	static_row_count_ = 0;
	last_static_draws_ = 0;
	last_lit_static_draws_ = 0;
	// The engine zeroes its payload and drops the claim that it mirrors the
	// cached rows, so the next static frame rewrites every active row
	// (renderer::StaticLightRows::clear_output); the zeros reach the device
	// the texture already holds.
	static_rows_.clear_output();
	if (static_light_rows_image_.is_valid() && static_light_rows_texture_.is_valid()) {
		upload_static_rows();
	}
}

int LightScene::render_frame(const Vector3 &p_camera_world,
		float p_query_radius, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather) {
	const int selected = camera_global_select(p_camera_world, p_query_radius,
			p_ambient_scale, p_time_ms, p_weather);
	selection_mode_ = "camera_global_objects";
	owner_isolation_ = "unavailable";
	return selected;
}

int LightScene::census_frame(const Vector3 &p_camera_world,
		float p_query_radius, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather) {
	// The same camera-global select, run to refresh the census rows on
	// demand (a report read with F3 stats off) WITHOUT restamping the mode
	// the gameplay pass reported: render_model_frame's "per_model_objects"
	// stays what the report says.
	return camera_global_select(p_camera_world, p_query_radius, p_ambient_scale,
			p_time_ms, p_weather);
}

int LightScene::camera_global_select(const Vector3 &p_camera_world,
		float p_query_radius, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather) {
	// Report/debug: ONE camera-global select. The gameplay object pass is
	// render_model_frame (per-draw contexts, the witnessed shape); this path
	// keeps the owned-light-unscoped approximation for census only and
	// publishes nothing to materials.
	const std::array<int32_t, 3> center =
			godot_to_mission_q16(p_camera_world);
	const double radius = std::isfinite(static_cast<double>(p_query_radius)) ?
			MAX(static_cast<double>(p_query_radius), 0.0) :
			static_cast<double>(std::numeric_limits<int32_t>::max()) / opennova::io::kFp16OneD;
	const int64_t half = static_cast<int64_t>(opennova::io::float_to_fp16_16_round_sat(radius));
	std::array<int32_t, 3> qmin{};
	std::array<int32_t, 3> qmax{};
	for (int axis = 0; axis < 3; ++axis) {
		qmin[axis] = opennova::io::saturate_i32(static_cast<int64_t>(center[axis]) - half);
		qmax[axis] = opennova::io::saturate_i32(static_cast<int64_t>(center[axis]) + half);
	}
	std::array<opennova::renderer::LightHandle, opennova::renderer::LightScene::kQueryLimit> handles{};
	const size_t found = scene_.query_camera_global(qmin, qmax, handles);

	const opennova::renderer::LightFrameInputs inputs =
			frame_inputs(p_time_ms, p_weather, p_ambient_scale);
	opennova::renderer::LightSelectionOptions options;
	options.target = opennova::renderer::LightSelectionTarget::Objects;
	options.admit_owned_unscoped = true;
	selected_count_ = scene_.select(handles.data(), found,
			opennova::renderer::LightActiveGroups{}, options, inputs.ambient, inputs.flicker,
			opennova::renderer::kObjectLightD3DFill, selected_);
	return static_cast<int>(selected_count_);
}

int64_t LightScene::owner_id_for_wire(int64_t p_wire_handle) {
	return static_cast<int64_t>(opennova::renderer::LightOwnerIds::wire(p_wire_handle));
}

int64_t LightScene::owner_id_for_static_source(int64_t p_source_index) {
	return static_cast<int64_t>(opennova::renderer::LightOwnerIds::static_source(p_source_index));
}

int LightScene::corona_texture_size() {
	return opennova::renderer::kCoronaTextureSize;
}

PackedByteArray LightScene::corona_texture_rgba8() {
	PackedByteArray bytes;
	const int size = opennova::renderer::kCoronaTextureSize;
	bytes.resize(static_cast<int64_t>(size) * size * 4);
	uint8_t *out = bytes.ptrw();
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const uint32_t argb = opennova::renderer::corona_texture_argb(x, y);
			uint8_t *texel = out + (static_cast<size_t>(y) * size + x) * 4;
			texel[0] = static_cast<uint8_t>((argb >> 16) & 0xFFu);
			texel[1] = static_cast<uint8_t>((argb >> 8) & 0xFFu);
			texel[2] = static_cast<uint8_t>(argb & 0xFFu);
			texel[3] = static_cast<uint8_t>(argb >> 24);
		}
	}
	return bytes;
}

void LightScene::slot_shadow_lights(const Vector3 &p_world_pos,
		float p_radius, int64_t p_interior_owner, int p_interior_section,
		const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather, std::vector<opennova::renderer::SlotPointLight> &r_out) {
	// The render-slot dominant-light query: the witnessed per-entity collect
	// over the ENTITY origin +- its bound radius, the nearest four, gated by
	// the entity's interior group alone (no objects-disable gate, no three-cap,
	// no D3D-fill boost) [engine: runtime/renderer/light_scene.h --
	// the collect and the pick live portable in
	// opennova::renderer::LightScene::slot_light_candidates and
	// opennova::renderer::pick_dominant_light, see
	// docs/render/render-lighting-re.md].
	r_out.clear();
	const std::array<int32_t, 3> center = godot_to_mission_q16(p_world_pos);
	const int64_t half =
			static_cast<int64_t>(opennova::io::float_to_fp16_16_round_sat(MAX(p_radius, 0.0f)));
	std::array<int32_t, 3> qmin{};
	std::array<int32_t, 3> qmax{};
	for (int axis = 0; axis < 3; ++axis) {
		qmin[axis] = opennova::io::saturate_i32(static_cast<int64_t>(center[axis]) - half);
		qmax[axis] = opennova::io::saturate_i32(static_cast<int64_t>(center[axis]) + half);
	}
	// The objects-target frame inputs for the flicker and the ambient scale;
	// the slot path takes no target gate.
	const opennova::renderer::LightFrameInputs sel =
			frame_inputs(p_time_ms, p_weather, p_ambient_scale);
	opennova::renderer::LightActiveGroups groups;
	groups.interior_group_entity = static_cast<uint64_t>(p_interior_owner);
	groups.interior_group_section = p_interior_owner != 0 ? p_interior_section : 0;
	std::array<opennova::renderer::SelectedLight, opennova::renderer::LightScene::kSlotPickLimit>
			selected{};
	const size_t count = scene_.slot_light_candidates(qmin, qmax, groups, sel.ambient,
			sel.flicker, selected);
	r_out.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		const opennova::renderer::SelectedLight &light = selected[i];
		opennova::renderer::SlotPointLight point;
		const Vector3 position = mission_to_godot(light.position);
		point.position = { float(position.x), float(position.y),
			float(position.z) };
		point.color = { light.color[0], light.color[1], light.color[2] };
		point.attenuation = { light.attenuation[0], light.attenuation[1],
			light.attenuation[2], light.attenuation[3] };
		// The D3D range the attached-light drape fills light 4 with (retail
		// Light_FillD3DPointLight @0x5aa532..0x5aa53b).
		point.range = light.range;
		point.handle = static_cast<uint32_t>(light.handle.retail_value) |
				(static_cast<uint32_t>(light.handle.generation) << 16);
		r_out.push_back(point);
	}
}

int LightScene::render_model_frame(const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities,
		const PackedInt64Array &p_interior_owners,
		const PackedInt32Array &p_interior_sections,
		const PackedByteArray &p_robj_scoped,
		const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
		const PackedVector3Array &p_entity_positions,
		const PackedInt32Array &p_entity_bound_radii_q16) {
	return select_model_frame(p_models, p_owner_entities, p_interior_owners,
			p_interior_sections, p_robj_scoped, p_ambient_scale, p_time_ms, p_weather,
			p_entity_positions, p_entity_bound_radii_q16, false);
}

int LightScene::render_inset_model_frame(const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities,
		const PackedInt64Array &p_interior_owners,
		const PackedInt32Array &p_interior_sections,
		const PackedByteArray &p_robj_scoped,
		const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
		const PackedVector3Array &p_entity_positions,
		const PackedInt32Array &p_entity_bound_radii_q16) {
	return select_model_frame(p_models, p_owner_entities, p_interior_owners,
			p_interior_sections, p_robj_scoped, p_ambient_scale, p_time_ms, p_weather,
			p_entity_positions, p_entity_bound_radii_q16, true);
}

// One view's per-draw selection. The selection is a function of the entity
// query and the groups alone, so a ROBJ selects the same in either view; the
// Inset pass only covers the draws the main view does not make (its twins).
int LightScene::select_model_frame(const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities,
		const PackedInt64Array &p_interior_owners,
		const PackedInt32Array &p_interior_sections,
		const PackedByteArray &p_robj_scoped,
		const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
		const PackedVector3Array &p_entity_positions,
		const PackedInt32Array &p_entity_bound_radii_q16, bool p_inset_view) {
	// The drawn models' facts (the entity query, the two groups, the skinned
	// level and the ROBJs the view draws) into the engine's subjects; the
	// draw-context policy and the select are the engine's
	// (renderer::LightDrawFrame).
	std::vector<ObjectModel *> subject_models;
	std::vector<opennova::renderer::LightDrawSubject> subjects;
	std::vector<std::pair<size_t, size_t>> robj_spans;
	std::vector<int32_t> robjs;
	std::vector<ObjectModel::PointLightDrawPart> parts;
	const int64_t model_count = p_models.size();
	for (int64_t i = 0; i < model_count; ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		// Owners are parallel to the INPUT array, not the filtered draw list.
		opennova::renderer::LightDrawSubject subject;
		subject.owner_entity = i < p_owner_entities.size()
				? static_cast<uint64_t>(static_cast<int64_t>(p_owner_entities[i])) : 0;
		subject.interior_entity = i < p_interior_owners.size()
				? static_cast<uint64_t>(static_cast<int64_t>(p_interior_owners[i])) : 0;
		subject.interior_section = i < p_interior_sections.size()
				? static_cast<int32_t>(p_interior_sections[i]) : 0;
		subject.building = i < p_robj_scoped.size() && p_robj_scoped[i] != 0;
		ObjectModel *entity_model = model->get_entity_light_owner();
		if (entity_model == nullptr) entity_model = model;
		const bool explicit_query = i < p_entity_positions.size() &&
				i < p_entity_bound_radii_q16.size();
		subject.query.position_fixed = godot_to_mission_q16(
				explicit_query ? p_entity_positions[i] : entity_model->get_global_position());
		subject.query.bound_radius_fixed = explicit_query ? p_entity_bound_radii_q16[i]
				: entity_model->get_entity_bound_radius_q16();
		std::vector<ObjectModel *> entity_models;
		if (p_inset_view) {
			collect_inset_entity_light_models(model, entity_models);
		} else {
			collect_entity_light_models(model, entity_models);
		}
		for (ObjectModel *part_model : entity_models) {
			opennova::renderer::LightDrawSubject part = subject;
			part.person_wave = part_model->is_slot_shadow_person();
			part.skinned = p_inset_view ? part_model->is_inset_view_level_skinned()
										: part_model->is_active_level_skinned();
			const size_t first = robjs.size();
			if (part.building || (!part.skinned && part.interior_entity != 0 &&
										   part.interior_section == 0)) {
				parts.clear();
				collect_view_draw_parts(part_model, p_inset_view, parts);
				for (const ObjectModel::PointLightDrawPart &drawn : parts)
					robjs.push_back(drawn.robj_index);
			}
			robj_spans.emplace_back(first, robjs.size() - first);
			subjects.push_back(part);
			subject_models.push_back(part_model);
		}
	}
	draw_frame_.clear();
	for (size_t i = 0; i < subjects.size(); ++i) {
		subjects[i].robjs = robjs.data() + robj_spans[i].first;
		subjects[i].robj_count = robj_spans[i].second;
		draw_frame_.add(subjects[i]);
	}
	draw_frame_.select(scene_, frame_inputs(p_time_ms, p_weather, p_ambient_scale));
	const std::vector<opennova::renderer::LightDrawTarget> &targets = draw_frame_.targets();
	const std::vector<opennova::renderer::LightDrawSelection> &selections =
			draw_frame_.selections();
	std::unordered_set<ObjectModel *> lit_model_set;
	for (size_t i = 0; i < targets.size(); ++i) {
		const opennova::renderer::LightDrawSelection &selection = selections[i];
		Vector4 posr[opennova::renderer::LightScene::kSelectLimit]{};
		Vector4 color[opennova::renderer::LightScene::kSelectLimit]{};
		for (size_t light = 0; light < selection.count; ++light) {
			const opennova::renderer::SelectedLight &selected = selection.lights[light];
			const Vector3 world = mission_to_godot(selected.position);
			posr[light] = Vector4(world.x, world.y, world.z, selected.attenuation[2]);
			color[light] = Vector4(selected.color[0], selected.color[1], selected.color[2],
					selected.range);
		}
		const int count = static_cast<int>(selection.count);
		ObjectModel *model = subject_models[targets[i].subject];
		if (p_inset_view && targets[i].per_robj) {
			model->apply_inset_point_light_selection_to_robj(targets[i].robj_index, count, posr,
					color);
		} else if (p_inset_view) {
			model->apply_inset_point_light_selection(count, posr, color);
		} else if (targets[i].per_robj) {
			model->apply_point_light_selection_to_robj(targets[i].robj_index, count, posr, color);
		} else {
			model->apply_point_light_selection(count, posr, color);
		}
		if (selection.count > 0) {
			lit_model_set.insert(model);
		}
	}
	if (p_inset_view) {
		return static_cast<int>(lit_model_set.size());
	}
	selection_mode_ = "per_model_objects";
	owner_isolation_ = draw_frame_.any_per_robj_building() ? "per_robj_buildings" : "per_model";
	last_models_ = static_cast<int>(subjects.size());
	last_lit_models_ = static_cast<int>(lit_model_set.size());
	return last_lit_models_;
}

int LightScene::render_static_frame(
		const PackedVector3Array &p_entity_positions,
		const PackedInt32Array &p_entity_bound_radii_q16,
		const PackedInt64Array &p_owner_entities,
		const PackedInt32Array &p_owner_sections,
		const PackedInt64Array &p_interior_owners,
		const PackedInt32Array &p_interior_sections,
		const PackedByteArray &p_active,
		const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
		int64_t p_rows_revision, const PackedVector4Array &p_entity_lights) {
	// The bound form of set_static_rows + select_static_rows: the rows are
	// handed over again only when their revision or count moved (a negative
	// revision is uncached: every frame re-selects from scratch).
	const int64_t row_count = p_entity_positions.size();
	if (p_rows_revision < 0 || p_rows_revision != static_rows_revision_ ||
			row_count != static_rows_.row_count()) {
		std::vector<opennova::renderer::StaticLightRow> rows(static_cast<size_t>(row_count));
		for (int64_t row = 0; row < row_count; ++row) {
			opennova::renderer::StaticLightRow &out = rows[static_cast<size_t>(row)];
			out.active = row < p_active.size() && p_active[row] != 0;
			out.query.position_fixed = godot_to_mission_q16(p_entity_positions[row]);
			out.query.bound_radius_fixed =
					row < p_entity_bound_radii_q16.size() ? p_entity_bound_radii_q16[row] : 0;
			out.groups.owner_group_entity = row < p_owner_entities.size()
					? static_cast<uint64_t>(static_cast<int64_t>(p_owner_entities[row])) : 0;
			out.groups.owner_group_section =
					row < p_owner_sections.size() ? static_cast<int32_t>(p_owner_sections[row]) : 0;
			out.groups.interior_group_entity = row < p_interior_owners.size()
					? static_cast<uint64_t>(static_cast<int64_t>(p_interior_owners[row])) : 0;
			out.groups.interior_group_section = row < p_interior_sections.size()
					? static_cast<int32_t>(p_interior_sections[row]) : 0;
			if (row < p_entity_lights.size()) {
				const Vector4 lane = p_entity_lights[row];
				out.lane = {lane.x, lane.y, lane.z, lane.w};
			}
		}
		set_static_rows(p_rows_revision, std::move(rows));
	}
	return select_static_rows(p_ambient_scale, p_time_ms, p_weather);
}

void LightScene::set_static_rows(int64_t p_revision,
		std::vector<opennova::renderer::StaticLightRow> p_rows) {
	static_rows_revision_ = p_revision;
	static_rows_.set_rows(p_revision, std::move(p_rows));
}

int LightScene::select_static_rows(const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather) {
	const int lit = static_rows_.select(scene_, frame_inputs(p_time_ms, p_weather, p_ambient_scale));
	upload_static_rows();
	static_row_count_ = static_rows_.row_count();
	last_static_draws_ = static_rows_.active_draws();
	last_lit_static_draws_ = lit;
	return lit;
}

// The engine's atlas onto the device: the RGBAF texture the MultiMesh rows
// read through INSTANCE_CUSTOM.x, published as opennova_static_point_light_rows
// when it is (re)made.
void LightScene::upload_static_rows() {
	if (!static_rows_.take_dirty()) {
		return;
	}
	const std::vector<float> &texels = static_rows_.texels();
	const int texture_rows = static_rows_.texture_rows();
	static_light_rows_bytes_.resize(static_cast<int64_t>(texels.size() * sizeof(float)));
	if (!texels.empty()) {
		std::memcpy(static_light_rows_bytes_.ptrw(), texels.data(), texels.size() * sizeof(float));
	}
	const bool recreate = static_light_rows_image_.is_null() ||
			static_light_rows_image_->get_height() != texture_rows ||
			static_light_rows_texture_.is_null();
	if (recreate) {
		static_light_rows_image_ = Image::create_from_data(opennova::renderer::kStaticLightRowTexels,
				texture_rows, false, Image::FORMAT_RGBAF, static_light_rows_bytes_);
		static_light_rows_texture_ = ImageTexture::create_from_image(static_light_rows_image_);
		publish_static_rows();
		return;
	}
	static_light_rows_image_->set_data(opennova::renderer::kStaticLightRowTexels, texture_rows,
			false, Image::FORMAT_RGBAF, static_light_rows_bytes_);
	static_light_rows_texture_->update(static_light_rows_image_);
}

void LightScene::publish_static_rows() const {
	if (static_light_rows_texture_.is_null()) return;
	if (RenderingServer *rs = RenderingServer::get_singleton()) {
		rs->global_shader_parameter_set("opennova_static_point_light_rows",
				static_light_rows_texture_);
	}
}

void LightScene::build_corona_inputs(const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog,
		CoronaView p_view,
		std::vector<opennova::renderer::LightCoronaOwnerMask> &r_owner_masks,
		opennova::renderer::LightCoronaFrameInputs &r_inputs) const {
	// The owners' visible-section masks of this view's own collect: a model
	// with an occlusion verdict (mask != -1) contributes its owner row; every
	// other owner passes the walk's gate (renderer::LightCoronaFrameInputs).
	r_owner_masks.clear();
	const int64_t model_count = p_models.size();
	r_owner_masks.reserve(static_cast<size_t>(model_count));
	for (int64_t i = 0; i < model_count && i < p_owner_entities.size(); ++i) {
		const ObjectModel *model = Object::cast_to<ObjectModel>(static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		const int64_t mask = p_view == CoronaView::Inset ? model->get_inset_view_section_mask()
				: p_view == CoronaView::Mirror           ? model->get_mirror_view_section_mask(0)
				: p_view == CoronaView::InsetMirror      ? model->get_mirror_view_section_mask(1)
														 : model->get_occlusion_section_mask();
		if (mask == -1) {
			continue;
		}
		opennova::renderer::LightCoronaOwnerMask row;
		row.owner_entity = static_cast<uint64_t>(static_cast<int64_t>(p_owner_entities[i]));
		row.section_mask = static_cast<uint32_t>(mask & 0xFFFFFFFF);
		r_owner_masks.push_back(row);
	}
	opennova::renderer::LightCoronaFog fog;
	fog.enabled = p_fog.is_valid() && p_fog->fog_enabled;
	fog.type = p_fog.is_valid() ? static_cast<int32_t>(p_fog->fog_type) : 0;
	fog.start = p_fog.is_valid() ? p_fog->fog_start : 0.0f;
	fog.end = p_fog.is_valid() ? p_fog->fog_end : 0.0f;
	// Godot (x, y-up, z) -> mission (x, -z, y).
	const std::array<float, 3> forward_mission = {
		static_cast<float>(p_camera_forward.x),
		static_cast<float>(-p_camera_forward.z),
		static_cast<float>(p_camera_forward.y),
	};
	r_inputs = opennova::renderer::light_corona_frame_inputs(godot_to_mission_q16(p_camera_pos),
			forward_mission, frame_inputs(p_time_ms, p_weather, p_ambient_scale),
			static_cast<uint32_t>(p_frame_index), fog, r_owner_masks);
}

int LightScene::collect_corona_rows(const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog) {
	opennova::renderer::LightCoronaFrameInputs inputs;
	build_corona_inputs(p_camera_pos, p_camera_forward, p_ambient_scale,
			p_time_ms, p_frame_index, p_weather, p_models, p_owner_entities,
			p_fog, CoronaView::Main, corona_masks_scratch_, inputs);
	corona_quads_scratch_.clear();
	scene_.collect_corona_quads(inputs, corona_quads_scratch_);
	return static_cast<int>(corona_quads_scratch_.size());
}

int LightScene::collect_inset_corona_rows(const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog,
		std::vector<opennova::renderer::LightCoronaQuad> &r_quads) {
	opennova::renderer::LightCoronaFrameInputs inputs;
	build_corona_inputs(p_camera_pos, p_camera_forward, p_ambient_scale,
			p_time_ms, p_frame_index, p_weather, p_models, p_owner_entities,
			p_fog, CoronaView::Inset, corona_masks_scratch_, inputs);
	r_quads.clear();
	scene_.collect_corona_quads(inputs, r_quads);
	return static_cast<int>(r_quads.size());
}

int LightScene::collect_mirror_corona_rows(int p_mirror_view, const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog,
		std::vector<opennova::renderer::LightCoronaQuad> &r_quads) {
	opennova::renderer::LightCoronaFrameInputs inputs;
	build_corona_inputs(p_camera_pos, p_camera_forward, p_ambient_scale,
			p_time_ms, p_frame_index, p_weather, p_models, p_owner_entities, p_fog,
			p_mirror_view == 1 ? CoronaView::InsetMirror : CoronaView::Mirror,
			corona_masks_scratch_, inputs);
	r_quads.clear();
	scene_.collect_corona_quads(inputs, r_quads);
	return static_cast<int>(r_quads.size());
}

PackedFloat32Array LightScene::get_last_corona_rows() const {
	PackedFloat32Array rows;
	rows.resize(static_cast<int64_t>(corona_quads_scratch_.size()) * kCoronaRowFloats);
	float *w = rows.ptrw();
	for (const opennova::renderer::LightCoronaQuad &quad : corona_quads_scratch_) {
		const Vector3 center = mission_to_godot(quad.center);
		w[0] = static_cast<float>(center.x);
		w[1] = static_cast<float>(center.y);
		w[2] = static_cast<float>(center.z);
		w[3] = quad.half_size;
		w[4] = quad.rgb[0];
		w[5] = quad.rgb[1];
		w[6] = quad.rgb[2];
		w += kCoronaRowFloats;
	}
	return rows;
}

size_t LightScene::collect_terrain_light_rows(
		const opennova::renderer::TerrainLightPatchBounds *p_patches,
		size_t p_patch_count, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather, uint32_t p_recip_packed,
		opennova::renderer::TerrainLightPatchRows *r_rows) const {
	opennova::renderer::TerrainLightPassInputs inputs;
	// The env light-state gain the object pass already feeds, and the loaded
	// recip unpacked (renderer::TerrainLightPassInputs carries the witness).
	inputs.ambient_scale = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	inputs.terrain_factor =
			opennova::renderer::terrain_per_channel_factor(p_recip_packed);
	inputs.flicker = opennova::renderer::light_flicker_inputs(
			p_weather != nullptr ? &p_weather->runtime().core().oscillator : nullptr,
			static_cast<uint32_t>(p_time_ms));
	// The reference adapter: the pixel-shader terrain path, the per-light loop
	// never skipped, and caps bit 0x100 with four stages, so the ps.1.1 light
	// pass (light_terrain_pass.h carries the witness).
	inputs.ps_light_pass = true;
	inputs.pixel_shader_path = true;
	inputs.light_pass_disabled = false;
	return scene_.collect_terrain_pass_rows(p_patches, p_patch_count, inputs,
			r_rows);
}

Array LightScene::collect_terrain_light_rows_for_bounds(
		const TypedArray<AABB> &p_world_aabbs, const Vector3 &p_ambient_scale,
		int p_time_ms, Weather *p_weather, int p_recip_packed) const {
	const int64_t patch_count = p_world_aabbs.size();
	std::vector<opennova::renderer::TerrainLightPatchBounds> patches;
	patches.reserve(static_cast<size_t>(patch_count));
	for (int64_t i = 0; i < patch_count; ++i) {
		const AABB box = p_world_aabbs[i];
		const Vector3 lo = box.position;
		const Vector3 hi = box.position + box.size;
		const float aabb_min[3] = {
			static_cast<float>(lo.x), static_cast<float>(lo.y),
			static_cast<float>(lo.z)
		};
		const float aabb_max[3] = {
			static_cast<float>(hi.x), static_cast<float>(hi.y),
			static_cast<float>(hi.z)
		};
		patches.push_back(opennova::renderer::terrain_patch_light_bounds(
				aabb_min, aabb_max, 0.0f, 0.0f));
	}
	std::vector<opennova::renderer::TerrainLightPatchRows> rows(patches.size());
	collect_terrain_light_rows(patches.data(), patches.size(), p_ambient_scale,
			p_time_ms, p_weather, static_cast<uint32_t>(p_recip_packed),
			rows.data());
	Array out;
	for (const opennova::renderer::TerrainLightPatchRows &patch : rows) {
		Array patch_rows;
		for (size_t i = 0; i < patch.count; ++i) {
			const opennova::renderer::TerrainLightRow &row = patch.rows[i];
			Dictionary d;
			d["position"] = mission_to_godot(row.position);
			d["inv_scale"] = row.inv_scale;
			d["color"] = Vector3(row.pixel_rgb[0], row.pixel_rgb[1],
					row.pixel_rgb[2]);
			d["handle"] = encode_handle(row.handle);
			patch_rows.push_back(d);
		}
		out.push_back(patch_rows);
	}
	return out;
}

int LightScene::terrain_light_texture_size() {
	return opennova::renderer::kFalloffTextureSize;
}

namespace {

PackedByteArray argb_words_to_rgba8(int width, int height,
		uint32_t (*texel)(int, int)) {
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(width) * height * 4);
	uint8_t *out = bytes.ptrw();
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const uint32_t argb = texel(x, y);
			uint8_t *px = out + (static_cast<size_t>(y) * width + x) * 4;
			px[0] = static_cast<uint8_t>((argb >> 16) & 0xFFu);
			px[1] = static_cast<uint8_t>((argb >> 8) & 0xFFu);
			px[2] = static_cast<uint8_t>(argb & 0xFFu);
			px[3] = static_cast<uint8_t>(argb >> 24);
		}
	}
	return bytes;
}

} // namespace

PackedByteArray LightScene::terrain_light_disc_rgba8() {
	const int size = opennova::renderer::kFalloffTextureSize;
	return argb_words_to_rgba8(size, size,
			&opennova::renderer::falloff_texture_light2d_argb);
}

int LightScene::terrain_light_cube_size(int p_word) {
	return opennova::renderer::cube_normalize_size(p_word);
}

PackedByteArray LightScene::terrain_light_cube_face_rgba8(int p_face, int p_size) {
	PackedByteArray bytes;
	if (p_face < 0 || p_face >= opennova::renderer::kCubeNormalizeFaces || p_size <= 0) {
		return bytes;
	}
	const int size = p_size;
	bytes.resize(static_cast<int64_t>(size) * size * 4);
	uint8_t *out = bytes.ptrw();
	for (int row = 0; row < size; ++row) {
		for (int col = 0; col < size; ++col) {
			const uint32_t argb =
					opennova::renderer::cube_normalize_texel_argb(p_face, col, row, size);
			uint8_t *px = out + (static_cast<size_t>(row) * size + col) * 4;
			px[0] = static_cast<uint8_t>((argb >> 16) & 0xFFu);
			px[1] = static_cast<uint8_t>((argb >> 8) & 0xFFu);
			px[2] = static_cast<uint8_t>(argb & 0xFFu);
			px[3] = static_cast<uint8_t>(argb >> 24);
		}
	}
	return bytes;
}

Ref<EffectLightReport> LightScene::get_report() const {
	const opennova::renderer::LightSceneReport report = scene_.inspect();
	Ref<EffectLightReport> out;
	out.instantiate();
	out->set_live(static_cast<int>(report.live));
	out->set_high_water(static_cast<int>(report.high_water));
	out->set_last_query(static_cast<int>(report.last_query));
	out->set_selected(static_cast<int>(selected_count_));
	out->set_selection_mode(selection_mode_);
	out->set_owner_isolation(owner_isolation_);
	out->set_models(last_models_);
	out->set_lit_models(last_lit_models_);
	out->set_static_rows(static_row_count_);
	out->set_static_draws(last_static_draws_);
	out->set_lit_static_draws(last_lit_static_draws_);
	for (size_t i = 0; i < selected_count_; ++i) {
		const opennova::renderer::SelectedLight &light = selected_[i];
		Ref<EffectLightRow> row;
		row.instantiate();
		row->set_position(mission_to_godot(light.position));
		row->set_color(Color(light.color[0], light.color[1], light.color[2]));
		row->set_range(light.range);
		row->set_attenuation_quadratic(light.attenuation[2]);
		row->set_handle(encode_handle(light.handle));
		row->set_retail_handle(static_cast<int>(light.handle.retail_value));
		out->add_row(row);
	}
	return out;
}

Ref<Image> LightScene::get_static_light_rows_image() const {
	if (static_light_rows_image_.is_null()) {
		return Ref<Image>();
	}
	return Image::create_from_data(static_light_rows_image_->get_width(),
			static_light_rows_image_->get_height(), false, Image::FORMAT_RGBAF,
			static_light_rows_bytes_);
}

namespace {

} // namespace


void LightScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("spawn_model_light", "config"),
			&LightScene::spawn_model_light);
	ClassDB::bind_method(D_METHOD("spawn_glow", "config"),
			&LightScene::spawn_glow);
	ClassDB::bind_method(D_METHOD("despawn", "handle"), &LightScene::despawn);
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
			"owner_entities", "interior_owners", "interior_sections",
			"robj_scoped", "ambient_scale", "time_ms", "weather",
			"entity_positions", "entity_bound_radii_q16"),
			&LightScene::render_model_frame, DEFVAL(PackedVector3Array()), DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("render_inset_model_frame", "models",
			"owner_entities", "interior_owners", "interior_sections",
			"robj_scoped", "ambient_scale", "time_ms", "weather",
			"entity_positions", "entity_bound_radii_q16"),
			&LightScene::render_inset_model_frame);
	ClassDB::bind_method(D_METHOD("render_static_frame",
			"entity_positions", "entity_bound_radii_q16", "owner_entities", "owner_sections",
			"interior_owners", "interior_sections", "active",
			"ambient_scale", "time_ms", "weather", "rows_revision", "entity_lights"),
			&LightScene::render_static_frame, DEFVAL(-1), DEFVAL(PackedVector4Array()));
	ClassDB::bind_method(D_METHOD("collect_corona_rows", "camera_pos",
			"camera_forward", "ambient_scale", "time_ms", "frame_index",
			"weather", "models", "owner_entities", "fog"),
			&LightScene::collect_corona_rows);
	ClassDB::bind_method(D_METHOD("get_last_corona_rows"),
			&LightScene::get_last_corona_rows);
	ClassDB::bind_static_method("LightScene", D_METHOD("owner_id_for_wire", "wire_handle"),
			&LightScene::owner_id_for_wire);
	ClassDB::bind_static_method("LightScene", D_METHOD("owner_id_for_static_source", "source_index"),
			&LightScene::owner_id_for_static_source);
	BIND_CONSTANT(WIRE_OWNER_TAG);
	BIND_CONSTANT(STATIC_OWNER_TAG);
	BIND_CONSTANT(WIRE_HANDLE_MASK);
	ClassDB::bind_method(D_METHOD("collect_terrain_light_rows_for_bounds",
			"world_aabbs", "ambient_scale", "time_ms", "weather",
			"recip_packed"),
			&LightScene::collect_terrain_light_rows_for_bounds);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_texture_size"),
			&LightScene::terrain_light_texture_size);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_disc_rgba8"),
			&LightScene::terrain_light_disc_rgba8);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_cube_size", "word"),
			&LightScene::terrain_light_cube_size);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_cube_face_rgba8", "face", "size"),
			&LightScene::terrain_light_cube_face_rgba8);
	ClassDB::bind_method(D_METHOD("get_report"), &LightScene::get_report);
	ClassDB::bind_method(D_METHOD("get_static_light_rows_image"),
			&LightScene::get_static_light_rows_image);
}

} // namespace godot
