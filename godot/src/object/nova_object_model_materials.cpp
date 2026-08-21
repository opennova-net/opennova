// ObjectModel material factory, classification, and authored animation.

#include "object/nova_object_model.h"

#include <godot_cpp/classes/image.hpp>
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

Ref<ShaderMaterial> ObjectModel::material_for_index(int p_material_array_index) {
	const int64_t cache_key = p_material_array_index;
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

} // namespace godot
