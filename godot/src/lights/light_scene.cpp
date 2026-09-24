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
#include <vector>

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include "env/weather.h"
#include "object/object_model.h"

namespace godot {

namespace {

// The objects-target select inputs both light legs (the point-light select
// and the per-model draw select) build: the flicker ring from the weather
// oscillator, the ambient scale, and the objects-target options.
struct ObjectSelectInputs {
	opennova::renderer::LightFlickerInputs flicker;
	std::array<float, 3> ambient{};
	opennova::renderer::LightSelectionOptions options;
};

ObjectSelectInputs object_select_inputs(int p_time_ms, const Weather *weather,
		const Vector3 &p_ambient_scale) {
	ObjectSelectInputs sel;
	sel.flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	if (weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				weather->runtime().core().oscillator;
		sel.flicker.amp_ring = oscillator.amp_ring;
		sel.flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		sel.flicker.ring_index = oscillator.ring_index;
	}
	sel.ambient = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	sel.options.target = opennova::renderer::LightSelectionTarget::Objects;
	sel.options.admit_owned_unscoped = false;
	return sel;
}

// godot (x, y, z) <-> mission (x, -z, y): the same conversion the mission
// placer and the render fixtures use.
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
		opennova::io::float_to_fp16_16_round_sat(static_cast<double>(world.x)),
		opennova::io::float_to_fp16_16_round_sat(-static_cast<double>(world.z)),
		opennova::io::float_to_fp16_16_round_sat(static_cast<double>(world.y)),
	};
}

opennova::renderer::LightDrawContext entity_draw_context(
		const Vector3 &position, int32_t bound_radius_q16) {
	return opennova::renderer::entity_light_draw_context({
			mission_fixed_from_godot(position), bound_radius_q16});
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

void fill_flicker(opennova::renderer::LightFlickerInputs &flicker, int p_time_ms,
		const Weather *p_weather) {
	flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	if (p_weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				p_weather->runtime().core().oscillator;
		flicker.amp_ring = oscillator.amp_ring;
		flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		flicker.ring_index = oscillator.ring_index;
	}
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

// Every object delivery path publishes the Light_GetPointLightParams colour:
// retail's shader passes receive exactly that through PointLightColor /
// PointLightColorArray (retail CRenderBatchQueue_FlushBatches @0x5da6a8 /
// @0x5da8ae), and the D3D fill's 1.5 (retail Light_FillD3DPointLight
// @0x5aa4b2) reaches only the fixed-function D3D lights, which the FF shader
// helper applies itself. The render-slot pick reads the params directly too.
constexpr bool kObjectLightD3DFill = false;

} // namespace

int64_t LightScene::spawn_model_light(const Ref<ModelLightSpawn> &p_config) {
	if (p_config.is_null()) return 0;
	opennova::renderer::LightSpawnParams params;
	params.position_fixed = mission_fixed_from_godot(p_config->get_position());
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
	params.position_fixed = mission_fixed_from_godot(p_config->get_position());
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
	// radius/2 below the light [orig: AmmoDef_ProcessImpactEffect @0x40a2b3
	// sets it; EffectWorld_RenderLightCoronas @0x5ab037 reads it, see
	// docs/render/render-lighting-re.md].
	params.corona_lower_half_radius = p_config->get_corona_lower_half_radius();
	return encode_handle(scene_.spawn(params));
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
	selected_.fill(opennova::renderer::SelectedLight{});
	selected_count_ = 0;
	selection_mode_ = "none";
	owner_isolation_ = "none";
	last_models_ = 0;
	last_lit_models_ = 0;
	static_row_count_ = 0;
	last_static_draws_ = 0;
	last_lit_static_draws_ = 0;
	if (static_light_rows_image_.is_valid() &&
			static_light_rows_texture_.is_valid()) {
		static_light_rows_bytes_.fill(0);
		static_light_rows_image_->set_data(
				static_light_rows_image_->get_width(),
				static_light_rows_image_->get_height(), false,
				Image::FORMAT_RGBAF, static_light_rows_bytes_);
		static_light_rows_texture_->update(static_light_rows_image_);
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
			mission_fixed_from_godot(p_camera_world);
	const double radius = std::isfinite(static_cast<double>(p_query_radius)) ?
			MAX(static_cast<double>(p_query_radius), 0.0) :
			static_cast<double>(std::numeric_limits<int32_t>::max()) / opennova::io::kFp16OneD;
	const int64_t half = static_cast<int64_t>(opennova::io::float_to_fp16_16_round_sat(radius));
	std::array<int32_t, 3> qmin{};
	std::array<int32_t, 3> qmax{};
	for (int axis = 0; axis < 3; ++axis) {
		qmin[axis] = clamp_int64(static_cast<int64_t>(center[axis]) - half);
		qmax[axis] = clamp_int64(static_cast<int64_t>(center[axis]) + half);
	}
	std::array<opennova::renderer::LightHandle, opennova::renderer::LightScene::kQueryLimit> handles{};
	const size_t found = scene_.query_camera_global(qmin, qmax, handles);

	opennova::renderer::LightFlickerInputs flicker;
	flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	const Weather *weather = p_weather;
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
	opennova::renderer::LightSelectionOptions options;
	options.target = opennova::renderer::LightSelectionTarget::Objects;
	options.admit_owned_unscoped = true;
	selected_count_ = scene_.select(handles.data(), found,
			opennova::renderer::LightActiveGroups{}, options, ambient, flicker,
			kObjectLightD3DFill, selected_);
	return static_cast<int>(selected_count_);
}

int64_t LightScene::owner_id_for_wire(int64_t p_wire_handle) {
	return p_wire_handle >= 0 ? (WIRE_OWNER_TAG | (p_wire_handle & WIRE_HANDLE_MASK)) : 0;
}

int64_t LightScene::owner_id_for_static_source(int64_t p_source_index) {
	return p_source_index >= 0 ? (STATIC_OWNER_TAG | p_source_index) : 0;
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
		float p_radius, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather, std::vector<opennova::renderer::SlotPointLight> &r_out) {
	// The render-slot dominant-light query: the witnessed per-entity collect
	// over entity position +- bound radius, group-gated params, no D3D-fill
	// boost [orig: RenderSlot_UpdateEntityLight @0x5d6a30 collects via
	// collect_nearby_zones_by_aabb @0x5aa250 and reads
	// Light_GetPointLightParams @0x5a9180 directly — the pick itself lives
	// portable in opennova::renderer::pick_dominant_light, see
	// docs/render/render-lighting-re.md].
	r_out.clear();
	const std::array<int32_t, 3> center = mission_fixed_from_godot(p_world_pos);
	const int64_t half =
			static_cast<int64_t>(opennova::io::float_to_fp16_16_round_sat(MAX(p_radius, 0.0f)));
	std::array<int32_t, 3> qmin{};
	std::array<int32_t, 3> qmax{};
	for (int axis = 0; axis < 3; ++axis) {
		qmin[axis] = clamp_int64(static_cast<int64_t>(center[axis]) - half);
		qmax[axis] = clamp_int64(static_cast<int64_t>(center[axis]) + half);
	}
	std::array<opennova::renderer::LightHandle, opennova::renderer::LightScene::kQueryLimit>
			handles{};
	const size_t found = scene_.query(qmin, qmax, handles);
	// The shared objects-target select inputs (object_select_inputs).
	const ObjectSelectInputs sel = object_select_inputs(p_time_ms, p_weather, p_ambient_scale);
	const opennova::renderer::LightFlickerInputs &flicker = sel.flicker;
	const std::array<float, 3> &ambient = sel.ambient;
	const opennova::renderer::LightSelectionOptions &options = sel.options;
	std::array<opennova::renderer::SelectedLight, opennova::renderer::LightScene::kSelectLimit>
			selected{};
	const size_t count = scene_.select(handles.data(), found,
			opennova::renderer::LightActiveGroups{}, options, ambient, flicker,
			kObjectLightD3DFill, selected);
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
	// The shared objects-target select inputs (object_select_inputs).
	const ObjectSelectInputs sel = object_select_inputs(p_time_ms, p_weather, p_ambient_scale);
	const opennova::renderer::LightFlickerInputs &flicker = sel.flicker;
	const std::array<float, 3> &ambient = sel.ambient;
	const opennova::renderer::LightSelectionOptions &options = sel.options;

	struct DrawTarget {
		ObjectModel *model = nullptr;
		int32_t robj_index = 0;
		bool robj_scoped = false;
	};
	const int64_t model_count = p_models.size();
	std::vector<DrawTarget> targets;
	std::vector<opennova::renderer::LightDrawContext> draws;
	targets.reserve(static_cast<size_t>(model_count));
	draws.reserve(static_cast<size_t>(model_count));
	int filtered_models = 0;
	bool any_robj_scoped = false;
	for (int64_t i = 0; i < model_count; ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(
				static_cast<Object *>(p_models[i]));
		if (model == nullptr) {
			continue;
		}
		// Owners are parallel to the INPUT array, not the filtered draw list.
		const uint64_t owner_entity = i < p_owner_entities.size()
				? static_cast<uint64_t>(
						static_cast<int64_t>(p_owner_entities[i]))
				: 0;
		const uint64_t interior_owner = i < p_interior_owners.size()
				? static_cast<uint64_t>(
						static_cast<int64_t>(p_interior_owners[i]))
				: 0;
		const int32_t interior_section = i < p_interior_sections.size()
				? static_cast<int32_t>(p_interior_sections[i])
				: 0;
		ObjectModel *entity_model = model->get_entity_light_owner();
		if (entity_model == nullptr) entity_model = model;
		const bool explicit_query = i < p_entity_positions.size() &&
				i < p_entity_bound_radii_q16.size();
		const auto entity_draw = entity_draw_context(
				explicit_query ? p_entity_positions[i] : entity_model->get_global_position(),
				explicit_query ? p_entity_bound_radii_q16[i] : entity_model->get_entity_bound_radius_q16());
		const bool robj_scoped = i < p_robj_scoped.size() &&
				p_robj_scoped[i] != 0;
		std::vector<ObjectModel *> entity_models;
		collect_entity_light_models(model, entity_models);
		for (ObjectModel *part_model : entity_models) {
			++filtered_models;
			if (robj_scoped) {
				any_robj_scoped = true;
				std::vector<ObjectModel::PointLightDrawPart> parts;
				part_model->collect_point_light_draw_parts(parts);
				// A rigid building has one row per visible ROBJ. Retain a section-0
				// fallback for malformed/skinned building data so it never falls back
				// to the broader entity-owner admission rule.
				if (parts.empty()) {
					parts.push_back(ObjectModel::PointLightDrawPart{
							0, part_model->get_world_bounds()});
				}
				for (const ObjectModel::PointLightDrawPart &part : parts) {
					auto draw = entity_draw;
					// A building draw declares itself as interior section zero, then
					// the model collector re-scopes the OWNER section per ROBJ. The
					// group gate falls back from interior section zero to this value.
					// [orig: Terrain_RenderSectorModels @0x5c5e07;
					// collect_render_objects_for_batch @0x5d8ff7, see
					// docs/render/render-lighting-re.md]
					const opennova::renderer::SubmitOwnerGroup owner =
							opennova::renderer::submit_owner_group(owner_entity, false, false,
									part.robj_index);
					draw.groups.owner_group_entity = owner.entity;
					draw.groups.owner_group_section = owner.section;
					draw.groups.interior_group_entity = owner_entity;
					draw.groups.interior_group_section = 0;
					draws.push_back(draw);
					targets.push_back(DrawTarget{
							part_model, part.robj_index, true});
				}
				continue;
			}

			// Every other entity submit: the interior group is the building this
			// model currently stands inside plus that blink volume's section
			// (zero = outdoors); the owner group is what the submit declares
			// (renderer::submit_owner_group) -- the drawn entity only for a
			// person's skinned draws, else entity 0 with the rigid collector's
			// per-ROBJ section.
			auto draw = entity_draw;
			draw.groups.interior_group_entity = interior_owner;
			draw.groups.interior_group_section = interior_section;
			const bool person_wave = part_model->is_slot_shadow_person();
			const bool skinned = part_model->is_active_level_skinned();
			// A rigid draw's owner section is read only when an owned light's
			// owner is the interior entity AND the interior section is zero (the
			// gate falls back to the owner section); only then do its ROBJs
			// select apart.
			if (!skinned && interior_owner != 0 && interior_section == 0) {
				std::vector<ObjectModel::PointLightDrawPart> parts;
				part_model->collect_point_light_draw_parts(parts);
				if (!parts.empty()) {
					for (const ObjectModel::PointLightDrawPart &part : parts) {
						auto part_draw = draw;
						const opennova::renderer::SubmitOwnerGroup owner =
								opennova::renderer::submit_owner_group(owner_entity,
										person_wave, false, part.robj_index);
						part_draw.groups.owner_group_entity = owner.entity;
						part_draw.groups.owner_group_section = owner.section;
						draws.push_back(part_draw);
						targets.push_back(DrawTarget{
								part_model, part.robj_index, true});
					}
					continue;
				}
			}
			const opennova::renderer::SubmitOwnerGroup owner =
					opennova::renderer::submit_owner_group(owner_entity, person_wave,
							skinned, 0);
			draw.groups.owner_group_entity = owner.entity;
			draw.groups.owner_group_section = owner.section;
			draws.push_back(draw);
			targets.push_back(DrawTarget{part_model, 0, false});
		}
	}
	std::vector<opennova::renderer::LightDrawSelection> selections(draws.size());
	scene_.select_for_draws(draws.data(), draws.size(), options, ambient,
			flicker, kObjectLightD3DFill, selections.data());
	std::unordered_set<ObjectModel *> lit_model_set;
	for (size_t i = 0; i < targets.size(); ++i) {
		const opennova::renderer::LightDrawSelection &selection = selections[i];
		Vector4 posr[opennova::renderer::LightScene::kSelectLimit]{};
		Vector4 color[opennova::renderer::LightScene::kSelectLimit]{};
		for (size_t light = 0; light < selection.count; ++light) {
			const opennova::renderer::SelectedLight &selected = selection.lights[light];
			const Vector3 world = mission_to_godot(selected.position);
			posr[light] = Vector4(world.x, world.y, world.z,
					selected.attenuation[2]);
			color[light] = Vector4(selected.color[0], selected.color[1],
					selected.color[2], selected.range);
		}
		if (targets[i].robj_scoped) {
			targets[i].model->apply_point_light_selection_to_robj(
					targets[i].robj_index,
					static_cast<int>(selection.count), posr, color);
		} else {
			targets[i].model->apply_point_light_selection(
					static_cast<int>(selection.count), posr, color);
		}
		if (selection.count > 0) {
			lit_model_set.insert(targets[i].model);
		}
	}
	selection_mode_ = "per_model_objects";
	owner_isolation_ = any_robj_scoped ? "per_robj_buildings" : "per_model";
	last_models_ = filtered_models;
	const int lit_models = static_cast<int>(lit_model_set.size());
	last_lit_models_ = lit_models;
	return lit_models;
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
		int64_t p_rows_revision) {
	// Rows are immutable identities stamped into MultiMesh INSTANCE_CUSTOM.x.
	// Their expensive AABB/light overlap and owner-group selection changes only
	// when either the placer row revision or the pool's selection topology does.
	// Cache those selected handles; live fade/RGB-gen/weather/ambient color is
	// still reevaluated below every frame for the handful of lit rows. A carved
	// entity remains a zero row rather than shifting later atlas identities.
	const int64_t row_count = p_entity_positions.size();
	static_row_count_ = static_cast<int>(row_count);
	opennova::renderer::LightFlickerInputs flicker;
	fill_flicker(flicker, p_time_ms, p_weather);
	const std::array<float, 3> ambient = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	opennova::renderer::LightSelectionOptions options;
	options.target = opennova::renderer::LightSelectionTarget::Objects;
	options.admit_owned_unscoped = false;

	std::vector<int> atlas_rows;
	std::vector<opennova::renderer::LightDrawSelection> selections;
	const uint64_t scene_revision = scene_.selection_revision();
	const bool cacheable = p_rows_revision >= 0;
	const bool rebuild_selection = !cacheable ||
			static_cached_scene_revision_ != scene_revision ||
			static_cached_rows_revision_ != p_rows_revision ||
			static_cached_row_count_ != row_count;
	if (rebuild_selection) {
		std::vector<opennova::renderer::LightDrawContext> draws;
		atlas_rows.reserve(static_cast<size_t>(row_count));
		draws.reserve(static_cast<size_t>(row_count));
		for (int64_t row = 0; row < row_count; ++row) {
			if (row >= p_active.size() || p_active[row] == 0) {
				continue;
			}
			auto draw = entity_draw_context(p_entity_positions[row],
					row < p_entity_bound_radii_q16.size()
							? p_entity_bound_radii_q16[row] : 0);
			draw.groups.owner_group_entity = row < p_owner_entities.size()
					? static_cast<uint64_t>(
							static_cast<int64_t>(p_owner_entities[row]))
					: 0;
			draw.groups.owner_group_section = row < p_owner_sections.size()
					? static_cast<int32_t>(p_owner_sections[row])
					: 0;
			draw.groups.interior_group_entity = row < p_interior_owners.size()
					? static_cast<uint64_t>(
							static_cast<int64_t>(p_interior_owners[row]))
					: 0;
			draw.groups.interior_group_section =
					row < p_interior_sections.size()
					? static_cast<int32_t>(p_interior_sections[row])
					: 0;
			atlas_rows.push_back(static_cast<int>(row));
			draws.push_back(draw);
		}
		selections.resize(draws.size());
		scene_.select_for_draws(draws.data(), draws.size(), options, ambient,
				flicker, kObjectLightD3DFill, selections.data());
		last_static_draws_ = static_cast<int>(draws.size());
		if (cacheable) {
			static_cached_selections_.clear();
			static_cached_selections_.reserve(selections.size());
			for (size_t i = 0; i < selections.size(); ++i) {
				const opennova::renderer::LightDrawSelection &selection = selections[i];
				if (selection.count == 0) {
					continue;
				}
				StaticCachedSelection cached;
				cached.atlas_row = atlas_rows[i];
				cached.groups = draws[i].groups;
				cached.count = selection.count;
				cached.last_count = selection.count;
				for (size_t light = 0; light < selection.count; ++light) {
					cached.handles[light] = selection.lights[light].handle;
					cached.animated = cached.animated ||
							scene_.slot_gen_active(selection.lights[light].handle);
				}
				static_cached_selections_.push_back(cached);
			}
			static_cached_scene_revision_ = scene_revision;
			static_cached_color_revision_ = scene_.color_revision();
			static_cached_ambient_ = ambient;
			static_cached_rows_revision_ = p_rows_revision;
			static_cached_row_count_ = static_cast<int>(row_count);
			static_cached_active_draws_ = last_static_draws_;
		}
	} else {
		// Broadphase and group gating are unchanged. Steady frames maintain
		// the resident payload row by row: a cached row re-selects only when
		// it is gen-animated or a global color input moved (any blend write
		// or fade ramp bumps the pool's color revision; the ambient gain is
		// compared directly). A skipped row's select() inputs are all
		// provably unchanged, so its resident 144 bytes already equal what a
		// recompute would produce; a recomputed row byte-compares before it
		// marks the texture for upload.
		last_static_draws_ = static_cached_active_draws_;
		const uint64_t color_revision = scene_.color_revision();
		const bool recompute_all =
				static_cached_color_revision_ != color_revision ||
				static_cached_ambient_ != ambient;
		const int64_t resident_size =
				static_cast<int64_t>(STATIC_LIGHT_ROW_TEXELS) *
				MAX(static_cast<int>(row_count), 1) * 16;
		if (static_bytes_resident_ &&
				static_light_rows_bytes_.size() == resident_size &&
				static_light_rows_image_.is_valid() &&
				static_light_rows_texture_.is_valid()) {
			float *texels = reinterpret_cast<float *>(
					static_light_rows_bytes_.ptrw());
			bool changed = false;
			int lit_draws = 0;
			opennova::renderer::LightDrawSelection selection;
			std::array<float, static_cast<size_t>(STATIC_LIGHT_ROW_TEXELS) * 4>
					row_texels;
			for (StaticCachedSelection &cached : static_cached_selections_) {
				if (!recompute_all && !cached.animated) {
					if (cached.last_count > 0) {
						++lit_draws;
					}
					continue;
				}
				selection.count = scene_.select(cached.handles.data(),
						cached.count, cached.groups, options, ambient, flicker,
						kObjectLightD3DFill, selection.lights);
				cached.last_count = selection.count;
				row_texels.fill(0.0f);
				row_texels[0] = static_cast<float>(selection.count);
				if (selection.count > 0) {
					++lit_draws;
				}
				for (size_t light = 0; light < selection.count; ++light) {
					const opennova::renderer::SelectedLight &selected =
							selection.lights[light];
					const Vector3 world =
							mission_to_godot(selected.position);
					const size_t base = (1 + light * 2) * 4;
					row_texels[base + 0] = world.x;
					row_texels[base + 1] = world.y;
					row_texels[base + 2] = world.z;
					row_texels[base + 3] = selected.attenuation[2];
					row_texels[base + 4] = selected.color[0];
					row_texels[base + 5] = selected.color[1];
					row_texels[base + 6] = selected.color[2];
					row_texels[base + 7] = selected.range;
				}
				float *atlas = texels + static_cast<size_t>(cached.atlas_row) *
						STATIC_LIGHT_ROW_TEXELS * 4;
				if (std::memcmp(atlas, row_texels.data(),
						sizeof(row_texels)) != 0) {
					std::memcpy(atlas, row_texels.data(), sizeof(row_texels));
					changed = true;
				}
			}
			if (changed) {
				static_light_rows_image_->set_data(STATIC_LIGHT_ROW_TEXELS,
						MAX(static_cast<int>(row_count), 1), false,
						Image::FORMAT_RGBAF, static_light_rows_bytes_);
				static_light_rows_texture_->update(static_light_rows_image_);
			}
			if (recompute_all) {
				static_cached_color_revision_ = color_revision;
				static_cached_ambient_ = ambient;
			}
			last_lit_static_draws_ = lit_draws;
			return lit_draws;
		}
		// No resident payload to maintain (first cacheable frame after a
		// non-cacheable call, or a texture that was never created): fall
		// through to the full re-select + whole-buffer path below.
		atlas_rows.reserve(static_cached_selections_.size());
		selections.resize(static_cached_selections_.size());
		for (size_t i = 0; i < static_cached_selections_.size(); ++i) {
			StaticCachedSelection &cached = static_cached_selections_[i];
			atlas_rows.push_back(cached.atlas_row);
			selections[i].count = scene_.select(cached.handles.data(), cached.count,
					cached.groups, options, ambient, flicker,
					kObjectLightD3DFill, selections[i].lights);
			cached.last_count = selections[i].count;
		}
	}

	const int texture_rows = MAX(static_cast<int>(row_count), 1);
	// Build the frame's rows in the scratch buffer and upload only when the
	// payload differs from the resident texture: with no flickering or moving
	// light in range, the selection resolves to the same bytes every frame.
	PackedByteArray &previous = static_light_rows_bytes_;
	static_light_rows_scratch_.resize(
			static_cast<int64_t>(STATIC_LIGHT_ROW_TEXELS) * texture_rows * 16);
	static_light_rows_scratch_.fill(0);
	float *texels = reinterpret_cast<float *>(static_light_rows_scratch_.ptrw());
	int lit_draws = 0;
	for (size_t i = 0; i < selections.size(); ++i) {
		const opennova::renderer::LightDrawSelection &selection = selections[i];
		float *atlas = texels + static_cast<size_t>(atlas_rows[i]) *
				STATIC_LIGHT_ROW_TEXELS * 4;
		atlas[0] = static_cast<float>(selection.count);
		if (selection.count > 0) {
			++lit_draws;
		}
		for (size_t light = 0; light < selection.count; ++light) {
			const opennova::renderer::SelectedLight &selected = selection.lights[light];
			const Vector3 world = mission_to_godot(selected.position);
			float *posr = atlas + (1 + light * 2) * 4;
			float *color = posr + 4;
			posr[0] = world.x;
			posr[1] = world.y;
			posr[2] = world.z;
			posr[3] = selected.attenuation[2];
			color[0] = selected.color[0];
			color[1] = selected.color[1];
			color[2] = selected.color[2];
			color[3] = selected.range;
		}
	}

	const bool recreate = static_light_rows_image_.is_null() ||
			static_light_rows_image_->get_width() != STATIC_LIGHT_ROW_TEXELS ||
			static_light_rows_image_->get_height() != texture_rows ||
			static_light_rows_texture_.is_null();
	const bool changed = recreate ||
			previous.size() != static_light_rows_scratch_.size() ||
			memcmp(previous.ptr(), static_light_rows_scratch_.ptr(),
					static_cast<size_t>(previous.size())) != 0;
	if (changed) {
		previous = static_light_rows_scratch_;
	}
	if (recreate) {
		static_light_rows_image_ = Image::create_from_data(
				STATIC_LIGHT_ROW_TEXELS, texture_rows, false,
				Image::FORMAT_RGBAF, static_light_rows_bytes_);
		static_light_rows_texture_ =
				ImageTexture::create_from_image(static_light_rows_image_);
	} else if (changed) {
		static_light_rows_image_->set_data(STATIC_LIGHT_ROW_TEXELS,
				texture_rows, false, Image::FORMAT_RGBAF,
				static_light_rows_bytes_);
		static_light_rows_texture_->update(static_light_rows_image_);
	}
	if (recreate) {
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			rs->global_shader_parameter_set("opennova_static_point_light_rows",
					static_light_rows_texture_);
		}
	}
	// The payload now mirrors the cached row set (when one exists), so steady
	// frames may maintain it in place.
	static_bytes_resident_ = cacheable;
	last_lit_static_draws_ = lit_draws;
	return lit_draws;
}

void LightScene::build_corona_inputs(const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog,
		std::vector<opennova::renderer::LightCoronaOwnerMask> &r_owner_masks,
		opennova::renderer::LightCoronaFrameInputs &r_inputs) const {
	opennova::renderer::LightCoronaFrameInputs &inputs = r_inputs;
	std::vector<opennova::renderer::LightCoronaOwnerMask> &owner_masks =
			r_owner_masks;
	owner_masks.clear();
	// Owner visible-section masks from the same model/owner walk the
	// per-model light pass runs: a model with an occlusion verdict (mask
	// != -1) contributes its owner row; everything else passes the gate
	// like retail's non-pool-2 owners [orig: Terrain_IsBuildingSectionBitSet
	// @0x5c6960 returns TRUE outside the mask array, see
	// docs/render/render-lighting-re.md].
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
		opennova::renderer::LightCoronaOwnerMask row;
		row.owner_entity = static_cast<uint64_t>(
				static_cast<int64_t>(p_owner_entities[i]));
		row.section_mask = static_cast<uint32_t>(mask & 0xFFFFFFFF);
		owner_masks.push_back(row);
	}
	inputs.owner_masks = owner_masks.data();
	inputs.owner_mask_count = owner_masks.size();
	// The fog-to-black fold [orig: CD3DDevice_SetFogAndBlendMode(dev, 2)
	// @0x5aafb6, see docs/render/render-lighting-re.md]: primary fog params,
	// color forced black engine-side.
	inputs.fog_enabled = p_fog.is_valid() && p_fog->fog_enabled;
	inputs.fog_type = p_fog.is_valid() ? static_cast<int32_t>(p_fog->fog_type) : 0;
	inputs.fog_start = p_fog.is_valid() ? p_fog->fog_start : 0.0f;
	inputs.fog_end = p_fog.is_valid() ? p_fog->fog_end : 0.0f;
	inputs.camera_fixed = mission_fixed_from_godot(p_camera_pos);
	// The camera depth plane in mission space: depth grows in front of the
	// camera, zero at the camera origin (the batch-sort plane retail feeds
	// the fade [orig: @0x5ab2f8..0x5ab33c]; the engine also tests the light
	// centre against the viewport near depth on this axis).
	const Vector3 forward = p_camera_forward.normalized();
	const std::array<float, 3> normal_mission = {
		static_cast<float>(forward.x),
		static_cast<float>(-forward.z),
		static_cast<float>(forward.y),
	};
	const std::array<int32_t, 3> &cam = inputs.camera_fixed;
	inputs.depth_plane_normal = normal_mission;
	inputs.depth_plane_w =
			-(normal_mission[0] * static_cast<float>(cam[0]) / opennova::io::kFp16One +
					normal_mission[1] * static_cast<float>(cam[1]) / opennova::io::kFp16One +
					normal_mission[2] * static_cast<float>(cam[2]) / opennova::io::kFp16One);
	inputs.ambient_scale = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	inputs.frame_index = static_cast<uint32_t>(p_frame_index);
	inputs.flicker.time_ms = static_cast<uint32_t>(p_time_ms);
	const Weather *weather = p_weather;
	if (weather != nullptr) {
		const opennova::env::WeatherOscillator &oscillator =
				weather->runtime().core().oscillator;
		inputs.flicker.amp_ring = oscillator.amp_ring;
		inputs.flicker.amp_ring_size =
				sizeof(oscillator.amp_ring) / sizeof(oscillator.amp_ring[0]);
		inputs.flicker.ring_index = oscillator.ring_index;
	}
}

int LightScene::fill_corona_multimesh(const Vector3 &p_camera_pos,
		const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
		int p_time_ms, int p_frame_index, Weather *p_weather,
		const TypedArray<Node3D> &p_models,
		const PackedInt64Array &p_owner_entities, const Ref<EnvLightValues> &p_fog,
		const Ref<MultiMesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return 0;
	}
	opennova::renderer::LightCoronaFrameInputs inputs;
	build_corona_inputs(p_camera_pos, p_camera_forward, p_ambient_scale,
			p_time_ms, p_frame_index, p_weather, p_models, p_owner_entities,
			p_fog, corona_masks_scratch_, inputs);
	corona_quads_scratch_.clear();
	scene_.collect_corona_quads(inputs, corona_quads_scratch_);
	const int64_t rows = static_cast<int64_t>(corona_quads_scratch_.size());
	// Grow to the high-water only: instance_count reallocation is the cost the
	// per-row Dictionary path paid on every size change.
	if (p_mesh->get_instance_count() < rows) {
		p_mesh->set_instance_count(static_cast<int32_t>(rows));
	}
	const int64_t capacity = p_mesh->get_instance_count();
	if (capacity <= 0) {
		p_mesh->set_visible_instance_count(0);
		return 0;
	}
	// The RenderingServer instance layout: three 4-float TRANSFORM_3D rows
	// (basis row, origin component), then RGBA when the mesh carries colors,
	// then custom data when it does. The mesh is configured by the shell, so
	// the stride is read off it rather than assumed.
	if (p_mesh->get_transform_format() != MultiMesh::TRANSFORM_3D) {
		ERR_FAIL_V_MSG(0, "corona MultiMesh must use TRANSFORM_3D");
	}
	const int64_t kFloatsPerInstance = 12 + (p_mesh->is_using_colors() ? 4 : 0) +
			(p_mesh->is_using_custom_data() ? 4 : 0);
	ERR_FAIL_COND_V_MSG(!p_mesh->is_using_colors(), 0,
			"corona MultiMesh must carry per-instance colors");
	corona_buffer_.resize(capacity * kFloatsPerInstance);
	float *w = corona_buffer_.ptrw();
	std::memset(w + rows * kFloatsPerInstance, 0,
			static_cast<size_t>((capacity - rows) * kFloatsPerInstance) *
					sizeof(float));
	for (int64_t i = 0; i < rows; ++i) {
		const opennova::renderer::LightCoronaQuad &quad =
				corona_quads_scratch_[static_cast<size_t>(i)];
		const Vector3 center = mission_to_godot(quad.center);
		const float half = quad.half_size;
		float *out = w + i * kFloatsPerInstance;
		out[0] = half;
		out[1] = 0.0f;
		out[2] = 0.0f;
		out[3] = static_cast<float>(center.x);
		out[4] = 0.0f;
		out[5] = half;
		out[6] = 0.0f;
		out[7] = static_cast<float>(center.y);
		out[8] = 0.0f;
		out[9] = 0.0f;
		out[10] = half;
		out[11] = static_cast<float>(center.z);
		out[12] = quad.rgb[0];
		out[13] = quad.rgb[1];
		out[14] = quad.rgb[2];
		out[15] = 1.0f;
	}
	p_mesh->set_buffer(corona_buffer_);
	p_mesh->set_visible_instance_count(static_cast<int32_t>(rows));
	return static_cast<int>(rows);
}

size_t LightScene::collect_terrain_light_rows(
		const opennova::renderer::TerrainLightPatchBounds *p_patches,
		size_t p_patch_count, const Vector3 &p_ambient_scale, int p_time_ms,
		Weather *p_weather, uint32_t p_recip_packed,
		opennova::renderer::TerrainLightPatchRows *r_rows) const {
	opennova::renderer::TerrainLightPassInputs inputs;
	// EffectWorld_AmbientScale = the env light-state gain the object pass
	// already feeds; flt_2732DA{C,8,4} = the loaded recip unpacked once
	// [orig: EffectWorld_TickInstancesAndLightScale @0x5aa1ef..0x5aa23f].
	inputs.ambient_scale = {
		static_cast<float>(p_ambient_scale.x),
		static_cast<float>(p_ambient_scale.y),
		static_cast<float>(p_ambient_scale.z),
	};
	inputs.terrain_factor =
			opennova::renderer::terrain_per_channel_factor(p_recip_packed);
	fill_flicker(inputs.flicker, p_time_ms, p_weather);
	// The normal pass: the pixel-shader terrain path is the one we render, the
	// per-light loop is never skipped, and the 0.4/r alternate projection rides
	// render-mode bit 0x100, which this shell never sets [orig: @0x6095e4,
	// @0x60983f, @0x609890].
	inputs.alt_pass = false;
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

int LightScene::terrain_light_strip_rows() {
	return opennova::renderer::kFalloffSpot1DRows;
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

PackedByteArray LightScene::terrain_light_strip_rgba8() {
	return argb_words_to_rgba8(opennova::renderer::kFalloffTextureSize,
			opennova::renderer::kFalloffSpot1DRows,
			&opennova::renderer::falloff_texture_spot1d_argb);
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

float LightScene::muzzle_glow_radius() {
	return static_cast<float>(opennova::renderer::LightScene::kMuzzleGlowRadiusFixed) / opennova::io::kFp16One;
}
Color LightScene::muzzle_glow_color() {
	return opennova::color_from_rgb24(opennova::renderer::LightScene::kMuzzleGlowColorRgb);
}
int LightScene::muzzle_glow_fade_mode() { return opennova::renderer::LightScene::kMuzzleGlowFadeMode; }
int LightScene::muzzle_glow_fade_ticks() { return opennova::renderer::LightScene::kMuzzleGlowFadeTicks; }
Color LightScene::death_flash_color() {
	return opennova::color_from_rgb24(opennova::renderer::LightScene::kDeathFlashColorRgb);
}
int LightScene::death_flash_fade_mode() { return opennova::renderer::LightScene::kDeathFlashFadeMode; }
int LightScene::death_flash_fade_ticks() { return opennova::renderer::LightScene::kDeathFlashFadeTicks; }

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
	ClassDB::bind_method(D_METHOD("render_static_frame",
			"entity_positions", "entity_bound_radii_q16", "owner_entities", "owner_sections",
			"interior_owners", "interior_sections", "active",
			"ambient_scale", "time_ms", "weather", "rows_revision"),
			&LightScene::render_static_frame, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("fill_corona_multimesh", "camera_pos",
			"camera_forward", "ambient_scale", "time_ms", "frame_index",
			"weather", "models", "owner_entities", "fog", "mesh"),
			&LightScene::fill_corona_multimesh);
	ClassDB::bind_method(D_METHOD("get_last_corona_buffer"),
			&LightScene::get_last_corona_buffer);
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
			D_METHOD("terrain_light_strip_rows"),
			&LightScene::terrain_light_strip_rows);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_disc_rgba8"),
			&LightScene::terrain_light_disc_rgba8);
	ClassDB::bind_static_method("LightScene",
			D_METHOD("terrain_light_strip_rgba8"),
			&LightScene::terrain_light_strip_rgba8);
	ClassDB::bind_method(D_METHOD("get_report"), &LightScene::get_report);
	ClassDB::bind_method(D_METHOD("get_static_light_rows_image"),
			&LightScene::get_static_light_rows_image);
}

} // namespace godot
