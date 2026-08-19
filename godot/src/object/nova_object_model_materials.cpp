// ObjectModel: material factory + classification and environment-lighting
// application. Ported verbatim from nova_object_materials.gd (2026-08-09
// de-scripting); the environment pull became the typed EnvLightState
// channel — the model never touches the environment object.

#include "object/nova_object_model.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/math.hpp>

#include "object/nova_object_shader_cache.h"

#include <renderer/material_classify.h>
#include <renderer/object_shader_template.h>
#include <threedi/threedi_3di3.h>

namespace godot {

namespace {

// index_hue.gd: golden-ratio conjugate hue spread.
constexpr double kPhiConjugate = 0.618033988749895;

} // namespace

void ObjectModel::build_material_defs() {
	material_defs_.clear();
	const Array materials = object_data_->get_materials();
	for (int64_t i = 0; i < materials.size(); ++i) {
		const Dictionary material = materials[i];
		const int64_t material_index =
				int64_t(material.get("material_index", material.get("index", 0)));
		material_defs_[material_index] = material;
		const int64_t array_index = int64_t(material.get("index", material_index));
		if (!material_defs_.has(array_index)) {
			material_defs_[array_index] = material;
		}
	}
}

bool ObjectModel::material_supports_projected_shadow_receiver(int p_blend_mode,
		int p_material_flags) {
	// The simple attenuation next-pass has no access to the source material's
	// alpha coverage or two-sided raster state; keep the approximation on
	// coverage-complete one-sided opaque surfaces only.
	return p_blend_mode == static_cast<int32_t>(renderer::ObjectBlendMode::Opaque) &&
			(p_material_flags & (THREEDI_MATERIAL_FLAG_ALPHA_TEST |
										THREEDI_MATERIAL_FLAG_TWO_SIDED)) == 0;
}

Ref<ShaderMaterial> ObjectModel::material_for_index(int p_material_array_index,
		int p_lighting_context) {
	const int64_t cache_key =
			(int64_t(p_material_array_index) << 8) | int64_t(p_lighting_context);
	const Ref<ShaderMaterial> *cached = material_cache_.getptr(cache_key);
	if (cached != nullptr) {
		return *cached;
	}
	const Dictionary *def = material_defs_.getptr(p_material_array_index);
	const Ref<ShaderMaterial> material =
			create_material(p_material_array_index, def != nullptr ? *def : Dictionary());
	material_cache_[cache_key] = material;
	// A newly created context-specific material still carries only the
	// defaults; force the next environment push to stamp it.
	last_env_gen_ = -1;
	last_env_values_.unref();
	last_section_env_values_.unref();
	return material;
}

Ref<ShaderMaterial> ObjectModel::create_material(int p_index,
		const Dictionary &p_material_def) {
	Ref<ShaderMaterial> material;
	material.instantiate();
	const int material_index = int(p_material_def.get("index", p_index));
	Dictionary info;
	if (object_data_.is_valid() && material_index >= 0 &&
			material_index < object_data_->get_material_count()) {
		info = object_data_->get_material_info(material_index);
	}
	String shader_tag =
			String(info.get("shader_tag", p_material_def.get("shader", "FF_ST_OP")));
	if (shader_tag.is_empty()) {
		shader_tag = "FF_ST_OP";
	}
	const int def_flags = int(p_material_def.get("flags", 0));
	int material_flags = 0;
	if (bool(info.get("alpha_test_enabled",
				(def_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0))) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
	}
	if (bool(info.get("alpha_invert",
				(def_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0))) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
	}
	if (bool(info.get("two_sided",
				(def_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0))) {
		material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
	}
	const int emissive_type =
			bool(info.get("emissive", false)) ? 2 : int(p_material_def.get("emissive_type", 0));
	const int is_glass_flag =
			bool(info.get("is_glass", p_material_def.get("is_glass", false))) ? 1 : 0;
	const int alpha_test_byte = int(info.get("alpha_test",
			int(Math::round(float(p_material_def.get("alpha_threshold", 0.0)) * 255.0f))));
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();

	// Textures resolve before the shader key: the detail stage only survives
	// classification when the secondary texture actually resolved.
	Ref<Texture2D> diffuse = load_texture_for_slot(p_material_def, 1);
	Ref<Texture2D> detail = load_texture_for_slot(p_material_def, 2);
	Ref<Texture2D> normal = load_texture_for_slot(p_material_def, 3);
	if (normal.is_null()) {
		normal = load_texture_for_slot(p_material_def, 4);
	}
	if (diffuse.is_null() && detail.is_valid()) {
		diffuse = detail;
		detail.unref();
	}

	int32_t key = shader_cache->classify(shader_tag, material_flags, emissive_type,
			is_glass_flag, alpha_test_byte);
	if (detail.is_null()) {
		// Retail runs the _MT second stage only with its texture bound — an
		// unresolved secondary composes the no-detail shader
		// (render-material-re.md §FF technique tables).
		key &= ~renderer::OSCAP_DETAIL;
	}
	shader_cache->configure_material_for_key(material, key);
	const int32_t blend_mode = shader_cache->blend_for_key(key);
	if (blend_mode != static_cast<int32_t>(renderer::ObjectBlendMode::Opaque)) {
		// Water-side rung applied by refresh_render_order() once placed.
		alpha_materials_.push_back(material);
	}

	if (diffuse.is_valid()) {
		material->set_shader_parameter("u_diffuse", diffuse);
	} else {
		material->set_shader_parameter("u_diffuse",
				solid_colour_texture(hash_color_for_index(p_index)));
	}
	if (detail.is_valid()) {
		material->set_shader_parameter("u_detail", detail);
	}
	if (normal.is_valid()) {
		material->set_shader_parameter("u_normal_map", normal);
	} else {
		material->set_shader_parameter("u_normal_map",
				solid_colour_texture(Color(0.5f, 0.5f, 1.0f, 1.0f)));
	}
	if ((material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0) {
		// The ref byte feeds the compare exactly; the shader keeps a > ref
		// (invert: a <= ref) [orig: CGfxDevice_SetAlphaTestRef @ 0x6770a0].
		material->set_shader_parameter("u_alpha_test_threshold",
				float(alpha_test_byte) / 255.0f);
		material->set_shader_parameter("u_alpha_test_invert",
				(material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0
						? 1.0f
						: 0.0f);
	} else {
		material->set_shader_parameter("u_alpha_test_threshold", 0.0f);
		material->set_shader_parameter("u_alpha_test_invert", 0.0f);
	}
	const Color reflect = info.get("reflect_color", Color(0.7f, 0.8f, 0.9f, 0.35f));
	material->set_shader_parameter("u_reflect_color", reflect);
	// The PANM evaluator supplies the complete two-row affine transform.
	material->set_shader_parameter("u_uv_transform_u", Vector3(1.0f, 0.0f, 0.0f));
	material->set_shader_parameter("u_uv_transform_v", Vector3(0.0f, 1.0f, 0.0f));
	material->set_shader_parameter("u_rgb_mod", Vector3(1, 1, 1));
	material->set_shader_parameter("u_alpha_mod", 1.0f);
	material->set_shader_parameter("u_local_light_count", 0);
	material->set_shader_parameter("u_local_light_position", Vector3());
	material->set_shader_parameter("u_local_light_color", Vector3(1, 1, 1));
	material->set_shader_parameter("u_local_light_intensity", 1.0f);
	material->set_shader_parameter("u_local_light_atten_start", 0.0f);
	material->set_shader_parameter("u_local_light_atten_end", 5.0f);
	if (material_supports_projected_shadow_receiver(blend_mode, material_flags)) {
		material->set_next_pass(get_shadow_receiver_material());
	}
	apply_default_environment_to_material(material);
	return material;
}

Ref<ShaderMaterial> ObjectModel::get_shadow_receiver_material() {
	if (shadow_receiver_material_.is_null()) {
		shadow_receiver_material_.instantiate();
		shadow_receiver_material_->set_shader(
				ResourceLoader::get_singleton()->load(
						"res://shaders/sun_shadow_catcher.gdshader", "Shader"));
	}
	return shadow_receiver_material_;
}

Ref<Texture2D> ObjectModel::load_texture_for_slot(const Dictionary &p_material_def,
		int p_slot) {
	if (object_data_.is_null() || p_material_def.is_empty()) {
		return Ref<Texture2D>();
	}
	const Array textures = p_material_def.get("textures", Array());
	if (textures.is_empty()) {
		return Ref<Texture2D>();
	}
	const int material_index = int(p_material_def.get("index", -1));
	if (material_index < 0) {
		return Ref<Texture2D>();
	}
	for (int64_t i = 0; i < textures.size(); ++i) {
		const Dictionary texture = textures[i];
		if (int(texture.get("slot", 0)) == p_slot) {
			const Ref<Texture2D> loaded = object_data_->load_material_texture(
					material_index, static_cast<int>(i));
			if (loaded.is_valid()) {
				return loaded;
			}
		}
	}
	return Ref<Texture2D>();
}

void ObjectModel::collect_anim_frames(int p_material_index) {
	if (anim_frames_by_mat_.has(p_material_index) || object_data_.is_null()) {
		return;
	}
	const PackedStringArray frame_names =
			object_data_->get_material_anim_frames(p_material_index, 1);
	if (frame_names.size() <= 1) {
		return;
	}
	Array frames;
	for (const String &frame_name : frame_names) {
		frames.append(load_texture_name(frame_name));
	}
	anim_frames_by_mat_[p_material_index] = frames;
}

Ref<Texture2D> ObjectModel::load_texture_name(const String &p_texture_name) {
	if (object_data_.is_null() || p_texture_name.is_empty()) {
		return Ref<Texture2D>();
	}
	return object_data_->load_texture_name(p_texture_name);
}

Color ObjectModel::hash_color_for_index(int p_index) {
	const double h = Math::fposmod(double(p_index) * kPhiConjugate, 1.0);
	return Color::from_hsv(static_cast<float>(h), 0.35f, 0.85f);
}

Ref<ImageTexture> ObjectModel::solid_colour_texture(const Color &p_color) {
	const Ref<Image> image = Image::create(1, 1, false, Image::FORMAT_RGBA8);
	image->set_pixel(0, 0, p_color);
	return ImageTexture::create_from_image(image);
}

void ObjectModel::apply_default_environment_to_material(
		const Ref<ShaderMaterial> &p_material) {
	apply_environment_values(p_material, EnvLightValues::retail_noon_defaults());
}

// A surface material needs per-frame UV/RGB/alpha evaluation only if one of
// its generators animates. Conservative: any non-zero generator style counts.
bool ObjectModel::material_runtime_is_dynamic(int p_material_index) const {
	if (object_data_.is_null()) {
		return true;
	}
	const Dictionary info = object_data_->get_material_info(p_material_index);
	if (info.is_empty()) {
		return true;
	}
	return int(info.get("uv_u_style", 0)) != 0 || int(info.get("uv_v_style", 0)) != 0 ||
			int(info.get("rgb_gen_style", 0)) != 0 ||
			int(info.get("alpha_gen_style", 0)) != 0;
}

// Partition surface materials into runtime-dynamic slots and the static
// remainder; only dynamic slots are visited per frame.
void ObjectModel::classify_materials() {
	material_needs_eval_.clear();
	PackedInt32Array dynamic_slots;
	HashMap<int, bool> kind_cache;
	for (int i = 0; i < surface_materials_.size(); ++i) {
		const int material_index = surface_material_indices_[i];
		bool needs_eval;
		const bool *cached = kind_cache.getptr(material_index);
		if (cached != nullptr) {
			needs_eval = *cached;
		} else {
			needs_eval = material_runtime_is_dynamic(material_index);
			kind_cache[material_index] = needs_eval;
		}
		material_needs_eval_.push_back(needs_eval);
		const Array *frames = anim_frames_by_mat_.getptr(material_index);
		if (needs_eval || (frames != nullptr && frames->size() > 1)) {
			dynamic_slots.append(i);
		}
	}
	dynamic_material_slots_ = dynamic_slots;
}

Ref<EnvLightValues> ObjectModel::entity_lighting_values(
		const Ref<EnvLightValues> &p_world_values, float p_effect_scale,
		bool p_interior_lerp, float p_interior_daylight) {
	if (p_world_values.is_null()) {
		return Ref<EnvLightValues>();
	}
	Ref<EnvLightValues> v;
	v.instantiate();
	const EnvLightValues &w = **p_world_values;
	v->dir = w.dir;
	v->dir_color = w.dir_color * CLAMP(p_effect_scale, 0.0f, 1.0f);
	v->hemi_sky = w.hemi_sky;
	v->hemi_ground = w.hemi_ground;
	v->ceiling = w.ceiling;
	v->floor_color = w.floor_color;
	v->gain = w.gain;
	v->fog_enabled = w.fog_enabled;
	v->fog_color = w.fog_color;
	v->fog_start = w.fog_start;
	v->fog_end = w.fog_end;
	v->fog_type = w.fog_type;
	if (p_interior_lerp) {
		// [orig: the containing building's light_transfer lerp —
		//  setup_entity_lighting_and_shader_constants @0x5D98A0]
		const float transfer = CLAMP(p_interior_daylight, 0.0f, 1.0f);
		v->dir_color = v->dir_color * transfer;
		v->hemi_ground = w.floor_color.lerp(w.hemi_ground, transfer);
		v->hemi_sky = w.ceiling.lerp(w.hemi_sky, transfer);
	}
	return v;
}

void ObjectModel::apply_environment_values(const Ref<ShaderMaterial> &p_material,
		const Ref<EnvLightValues> &p_values) {
	if (p_material.is_null() || p_values.is_null()) {
		return;
	}
	const EnvLightValues &v = **p_values;
	p_material->set_shader_parameter("u_hemi_sky_color", v.hemi_sky);
	p_material->set_shader_parameter("u_dir_light_dir", v.dir);
	p_material->set_shader_parameter("u_dir_light_color", v.dir_color);
	p_material->set_shader_parameter("u_hemi_ground_color", v.hemi_ground);
	p_material->set_shader_parameter("u_color_src_global_gain", v.gain);
	p_material->set_shader_parameter("u_fog_enabled", v.fog_enabled);
	p_material->set_shader_parameter("u_fog_color", v.fog_color);
	p_material->set_shader_parameter("u_fog_start", v.fog_start);
	p_material->set_shader_parameter("u_fog_end", v.fog_end);
	p_material->set_shader_parameter("u_fog_type", v.fog_type);
}

// Stamp the current environment values onto every surface material, skipping
// entirely when the published generation and the derived values are unchanged
// (retained mode — an identical re-push is invisible).
void ObjectModel::apply_environment_to_materials() {
	int64_t gen = -1;
	Ref<EnvLightValues> world_values;
	if (env_state_.is_valid()) {
		gen = env_state_->get_generation();
		const bool have_all_cached = last_env_values_.is_valid() &&
				(!interior_section_lighting_ || last_section_env_values_.is_valid());
		if (gen == last_env_gen_ && have_all_cached) {
			return;
		}
		world_values = env_state_->get_values();
	}
	if (world_values.is_null()) {
		world_values = EnvLightValues::retail_noon_defaults();
	}
	// A portal building is not an ordinary entity submission: its exterior
	// shell always keeps effectScale 1, and only ROBJ 1+ takes its own
	// ItemDef transfer.
	const Ref<EnvLightValues> values = entity_lighting_values(world_values,
			interior_section_lighting_ ? 1.0f : lighting_effect_scale_,
			interior_section_lighting_ ? false : interior_lerp_,
			interior_section_lighting_ ? 0.0f : interior_daylight_);
	Ref<EnvLightValues> section_values;
	if (interior_section_lighting_) {
		section_values = entity_lighting_values(world_values, 1.0f, true,
				interior_section_daylight_);
	}
	const bool entity_unchanged = values->equals(last_env_values_);
	const bool section_unchanged = !interior_section_lighting_ ||
			section_values->equals(last_section_env_values_);
	if (entity_unchanged && section_unchanged) {
		last_env_gen_ = gen;
		return;
	}
	last_env_values_ = values;
	last_section_env_values_ = section_values;
	last_env_gen_ = gen;
	for (int i = 0; i < surface_materials_.size(); ++i) {
		const Ref<ShaderMaterial> material = surface_materials_[i];
		if (material.is_null()) {
			continue;
		}
		const int context = i < surface_lighting_contexts_.size()
				? surface_lighting_contexts_[i]
				: LIGHTING_CONTEXT_ENTITY;
		apply_environment_values(material,
				context == LIGHTING_CONTEXT_INTERIOR_SECTION ? section_values : values);
	}
}

void ObjectModel::apply_lights() {
	// This per-material local-light route is editor preview only. Gameplay
	// evaluates authored LGHT into the shared EffectWorld light pool, whose
	// selected lights arrive through the global object-shader uniforms; it must
	// not also inject the same record through u_local_light_*.
	if (!model_light_preview_enabled_ || !has_lights_) {
		return;
	}
	if (object_data_.is_null()) {
		return;
	}
	const Array lights = object_data_->evaluate_lights(anim_time_ms_, ctrl_values_);
	Dictionary dominant;
	float best_intensity = -1.0f;
	for (int64_t i = 0; i < lights.size(); ++i) {
		const Dictionary info = lights[i];
		const float intensity = float(info.get("intensity", 1.0));
		if (intensity > best_intensity) {
			best_intensity = intensity;
			dominant = info;
		}
	}
	const int count = dominant.is_empty() ? 0 : 1;
	const Vector3 model_position = dominant.get("position", Vector3());
	Vector3 position = get_global_transform().xform(model_position);
	const Color color = dominant.get("color", Color(1, 1, 1, 1));
	const float atten_start = float(dominant.get("atten_start", 0.0));
	const float atten_end = float(dominant.get("atten_end", 5.0));
	const int subobject = int(dominant.get("subobject", -1));
	if (skeleton_ != nullptr && subobject >= 0 &&
			subobject < skeleton_->get_bone_count()) {
		// LGHT positions are authored in model space; move through the same
		// rest-to-live bone transform as user points.
		position = (skeleton_->get_global_transform() *
						   skeleton_->get_bone_global_pose(subobject) *
						   skeleton_->get_bone_global_rest(subobject).affine_inverse())
						   .xform(model_position);
	} else if (subobject >= 0) {
		Node3D *const *node = robj_nodes_.getptr(subobject);
		const Transform3D *rest = robj_rest_transforms_.getptr(subobject);
		if (node != nullptr && *node != nullptr && rest != nullptr) {
			position = (*node)->get_global_transform().xform(
					rest->affine_inverse().xform(model_position));
		}
	}
	// Push only on change (retained mode — the skip is invisible).
	const float intensity = best_intensity > 0.0f ? best_intensity : 1.0f;
	if (last_light_push_valid_ && count == last_light_count_ &&
			position == last_light_position_ && color == last_light_color_ &&
			intensity == last_light_intensity_ &&
			atten_start == last_light_atten_start_ &&
			atten_end == last_light_atten_end_) {
		return;
	}
	last_light_push_valid_ = true;
	last_light_count_ = count;
	last_light_position_ = position;
	last_light_color_ = color;
	last_light_intensity_ = intensity;
	last_light_atten_start_ = atten_start;
	last_light_atten_end_ = atten_end;
	for (const Ref<ShaderMaterial> &material : surface_materials_) {
		if (material.is_null()) {
			continue;
		}
		material->set_shader_parameter("u_local_light_count", count);
		material->set_shader_parameter("u_local_light_position", position);
		material->set_shader_parameter("u_local_light_color",
				Vector3(color.r, color.g, color.b));
		material->set_shader_parameter("u_local_light_intensity", intensity);
		material->set_shader_parameter("u_local_light_atten_start", atten_start);
		material->set_shader_parameter("u_local_light_atten_end", atten_end);
	}
}

} // namespace godot
