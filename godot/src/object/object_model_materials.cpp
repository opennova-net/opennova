// ObjectModel: material factory + classification and environment-lighting
// application. Ported verbatim from object_materials.gd (2026-08-09
// de-scripting); the environment pull became the typed EnvLightState
// channel — the model never touches the environment object.

#include "object/object_model.h"
#include "object/model_inspection_records.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/math.hpp>

#include "object/object_shader_cache.h"
#include "render/frame_fx.h"

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/render_order.h>
#include <formats/threedi/threedi_3di3.h>

namespace godot {

namespace {

// index_hue.gd: golden-ratio conjugate hue spread.
constexpr double kPhiConjugate = 0.618033988749895;

const StringName &postmultiply_material_meta() {
	static const StringName name("_opennova_postmultiply_material");
	return name;
}

Ref<ShaderMaterial> auxiliary_for(const Ref<ShaderMaterial> &p_material,
		const StringName &p_meta) {
	if (p_material.is_null() ||
			!p_material->has_meta(p_meta)) {
		return Ref<ShaderMaterial>();
	}
	return p_material->get_meta(p_meta, Variant());
}

} // namespace

void ObjectModel::set_material_and_auxiliary_parameter(
		const Ref<ShaderMaterial> &p_material, const StringName &p_name,
		const Variant &p_value) {
	if (p_material.is_null()) {
		return;
	}
	p_material->set_shader_parameter(p_name, p_value);
	const Ref<ShaderMaterial> postmultiply = auxiliary_for(
			p_material, postmultiply_material_meta());
	if (postmultiply.is_valid()) {
		postmultiply->set_shader_parameter(p_name, p_value);
	}
}

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

Ref<ShaderMaterial> ObjectModel::material_for_index(int p_material_array_index) {
	const int64_t cache_key = int64_t(p_material_array_index);
	const Ref<ShaderMaterial> *cached = material_cache_.getptr(cache_key);
	if (cached != nullptr) {
		return *cached;
	}
	const Dictionary *def = material_defs_.getptr(p_material_array_index);
	const Ref<ShaderMaterial> material =
			create_material(p_material_array_index, def != nullptr ? *def : Dictionary());
	material_cache_[cache_key] = material;
	return material;
}

Ref<ShaderMaterial> ObjectModel::create_material(int p_index,
		const Dictionary &p_material_def) {
	Ref<ShaderMaterial> material;
	material.instantiate();
	const int material_index = int(p_material_def.get("index", p_index));
	Ref<MaterialInfo> info;
	if (object_data_.is_valid() && material_index >= 0 &&
			material_index < object_data_->get_material_count()) {
		info = object_data_->get_material_info(material_index);
	}
	// The authored MTRL row wins; the def's material block is the fallback.
	String shader_tag = info.is_valid() ? info->get_shader_tag()
									   : String(p_material_def.get("shader", "FF_ST_OP"));
	if (shader_tag.is_empty()) {
		shader_tag = "FF_ST_OP";
	}
	const int def_flags = int(p_material_def.get("flags", 0));
	int material_flags = 0;
	if (info.is_valid() ? info->get_alpha_test_enabled()
						: (def_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
	}
	if (info.is_valid() ? info->get_alpha_invert()
						: (def_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
	}
	if (info.is_valid() ? info->get_two_sided()
						: (def_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0) {
		material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
	}
	const int emissive_type = (info.is_valid() && info->get_emissive())
			? 2
			: int(p_material_def.get("emissive_type", 0));
	const int is_glass_flag =
			(info.is_valid() ? info->get_is_glass() : bool(p_material_def.get("is_glass", false))) ? 1 : 0;
	const int alpha_test_byte = info.is_valid()
			? info->get_alpha_test()
			: int(Math::round(float(p_material_def.get("alpha_threshold", 0.0)) * 255.0f));
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
	const CharString shader_tag_utf8 = shader_tag.utf8();
	const opennova::renderer::ObjectMaterialClassification q3_classification =
			opennova::renderer::classify_object_material(
					std::string(shader_tag_utf8.get_data()),
					static_cast<uint8_t>(material_flags),
					static_cast<uint8_t>(emissive_type),
					static_cast<uint8_t>(is_glass_flag),
					static_cast<uint8_t>(alpha_test_byte));
	if (detail.is_null()) {
		// Retail runs the _MT second stage only with its texture bound — an
		// unresolved secondary composes the no-detail shader
		// (render-material-re.md §FF technique tables).
		key &= ~opennova::renderer::OSCAP_DETAIL;
	}
	shader_cache->configure_material_for_key(material, key);
	const opennova::renderer::ObjectShaderPipelineDescriptor pipeline =
			opennova::renderer::describe_object_shader_pipeline(static_cast<uint32_t>(key));
	// BmTxMirrT.fx P3 is an independent raw-UV, regular-fogged draw with
	// DESTCOLOR/SRCCOLOR (2*source*framebuffer). Keep it paired with the P0/P1
	// material so animated Diffuse1 and environment state update atomically.
	if (pipeline.technique ==
			opennova::renderer::ObjectShaderTechnique::EnvironmentMirrorTextured) {
		String proxy_path = "res://shaders/object/postmultiply/environment_textured";
		if (pipeline.alpha_test) {
			proxy_path += "_cutout";
		}
		if (pipeline.two_sided) {
			proxy_path += "_double_sided";
		}
		proxy_path += ".gdshader";
		const Ref<Shader> proxy_shader = ResourceLoader::get_singleton()->load(
				proxy_path, "Shader");
		ERR_FAIL_COND_V_MSG(proxy_shader.is_null(), Ref<ShaderMaterial>(),
				String("Environment postmultiply shader failed to load: ") + proxy_path);
		Ref<ShaderMaterial> proxy_material;
		proxy_material.instantiate();
		proxy_material->set_shader(proxy_shader);
		proxy_material->set_render_priority(opennova::renderer::kRungObjectPostMultiply);
		material->set_meta(postmultiply_material_meta(), proxy_material);
	}
	if (diffuse.is_valid()) {
		set_material_and_auxiliary_parameter(material, "u_diffuse", diffuse);
	} else {
		set_material_and_auxiliary_parameter(material, "u_diffuse",
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
		set_material_and_auxiliary_parameter(material, "u_alpha_test_threshold",
				float(alpha_test_byte) / 255.0f);
		set_material_and_auxiliary_parameter(material, "u_alpha_test_invert",
				(material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0
						? 1.0f
						: 0.0f);
	} else {
		set_material_and_auxiliary_parameter(material, "u_alpha_test_threshold", 0.0f);
		set_material_and_auxiliary_parameter(material, "u_alpha_test_invert", 0.0f);
	}
	const Color reflect = info.is_valid() ? info->get_reflect_color() : Color(0.7f, 0.8f, 0.9f, 0.35f);
	set_material_and_auxiliary_parameter(material, "u_reflect_color", reflect);
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
	FrameFx::register_q3_object_material(material, q3_classification);
	// No shadow-receiver next pass on world models: retail's render-slot
	// entity ground shadows drape TERRAIN-FOLLOWING patches only — a live
	// silhouette never lands on another model [orig:
	// RenderSlot_DrawAllDrapes @0x5d6e20 draws the slot's terrain patch via
	// render_sector_model @0x5d5ca0; see docs/render/render-lighting-re.md].
	// The terrain material carries the drape pass (SlotShadow).
	return material;
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

// A surface material needs per-frame UV/RGB/alpha evaluation only if one of
// its generators animates. Conservative: any non-zero generator style counts.
bool ObjectModel::material_runtime_is_dynamic(int p_material_index) const {
	if (object_data_.is_null()) {
		return true;
	}
	const Ref<MaterialInfo> info = object_data_->get_material_info(p_material_index);
	if (info.is_null()) {
		return true;
	}
	return info->get_uv_u_style() != 0 || info->get_uv_v_style() != 0 ||
			info->get_rgb_gen_style() != 0 || info->get_alpha_gen_style() != 0;
}

// Partition surface materials into runtime-dynamic slots and the static
// remainder; only dynamic slots are visited per frame.
void ObjectModel::classify_materials() {
	material_needs_eval_.clear();
	PackedInt32Array dynamic_slots;
	HashMap<int, bool> kind_cache;
	HashSet<ObjectID> admitted_materials;
	material_runtime_stamps_.assign(
			static_cast<size_t>(surface_materials_.size()), MaterialRuntimeStamp());
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
			const Ref<ShaderMaterial> material = surface_materials_[i];
			if (material.is_valid()) {
				const ObjectID material_id(material->get_instance_id());
				if (!admitted_materials.has(material_id)) {
					admitted_materials.insert(material_id);
					dynamic_slots.append(i);
				}
			}
		}
	}
	dynamic_material_slots_ = dynamic_slots;
}

} // namespace godot
