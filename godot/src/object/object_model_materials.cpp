// ObjectModel: material factory + classification and environment-lighting
// application. Ported verbatim from object_materials.gd (2026-08-09
// de-scripting); the environment pull became the typed EnvLightState
// channel — the model never touches the environment object.

#include "object/object_model.h"
#include "object/material_info.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/math.hpp>

#include "object/object_shader_cache.h"
#include "render/frame_fx.h"
#include "util/texture_path_resolver.h"

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/render_order.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/model_geometry.h>
#include <base/io/fixed.h>

using namespace opennova::threedi;

namespace godot {

// A model decides its own mirror CLIP arming unless an owning entity does
// (a linked part), it draws in no arming pass, or it is a BySide person.
bool ObjectModel::tracks_water_mirror_clip() const {
	return !water_mirror_clip_inherited_ && !thermal_entity_wave_ &&
			!entity_light_owner_.is_valid() &&
			water_mirror_clip_wave_ != opennova::env::MirrorClipWave::kNone;
}

void ObjectModel::set_water_mirror_clip_wave_id(int p_wave) {
	set_water_mirror_clip_wave(
			p_wave == static_cast<int>(opennova::env::MirrorClipWave::kSectorModel) ?
					opennova::env::MirrorClipWave::kSectorModel :
			p_wave == static_cast<int>(opennova::env::MirrorClipWave::kEntity) ?
					opennova::env::MirrorClipWave::kEntity :
					opennova::env::MirrorClipWave::kNone);
}

void ObjectModel::set_water_mirror_clip_wave(opennova::env::MirrorClipWave p_wave) {
	if (water_mirror_clip_wave_ == p_wave) {
		return;
	}
	water_mirror_clip_wave_ = p_wave;
	if (tracks_water_mirror_clip()) {
		water_mirror_clip_models_.insert(this);
		set_notify_transform(true);
	} else {
		water_mirror_clip_models_.erase(this);
	}
	refresh_water_mirror_clip();
}

// The per-draw test against the live water plane (the building pass reads
// the graphic's bound-block floor, the first entity wave the entity+0 bound
// radius; runtime/environment/water_mirror.h carries the witnesses). No plane
// means no mirror pass, so nothing is armed.
void ObjectModel::refresh_water_mirror_clip() {
	if (water_mirror_clip_inherited_) {
		return;
	}
	if (const ObjectModel *owner = get_entity_light_owner()) {
		apply_water_mirror_clip_armed(owner->water_mirror_clip_armed_);
		return;
	}
	bool armed = false;
	const ObjectShaderCache *cache = ObjectShaderCache::get_singleton();
	if (tracks_water_mirror_clip() && cache != nullptr && cache->has_water_plane() &&
			is_inside_tree()) {
		if (water_mirror_clip_wave_ == opennova::env::MirrorClipWave::kSectorModel &&
				object_data_.is_valid() && object_data_->has_document()) {
			water_mirror_clip_floor_q16_ = opennova::world::model_bound_floor_q16(
					object_data_->native_model());
		}
		const int32_t extent =
				water_mirror_clip_wave_ == opennova::env::MirrorClipWave::kSectorModel ?
				water_mirror_clip_floor_q16_ : entity_bound_radius_q16_;
		armed = opennova::env::water_mirror_clip_armed(water_mirror_clip_wave_,
				opennova::io::float_to_fp16_16_round_sat(
						static_cast<float>(get_global_position().y)),
				extent,
				opennova::io::float_to_fp16_16_round_sat(cache->get_water_plane_height()));
	}
	apply_water_mirror_clip_armed(armed);
}

void ObjectModel::apply_water_mirror_clip_armed(bool p_armed) {
	if (water_mirror_clip_armed_ == p_armed) {
		return;
	}
	water_mirror_clip_armed_ = p_armed;
	stamp_entity_lighting_instances();
	for (ObjectModel *linked : live_presentation_links()) {
		linked->apply_water_mirror_clip_armed(p_armed);
	}
	for (const ObjectID &id : water_mirror_clip_attached_) {
		ObjectModel *attached = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id));
		if (attached != nullptr && attached->get_entity_light_owner() == this) {
			attached->apply_water_mirror_clip_armed(p_armed);
		}
	}
}

void ObjectModel::refresh_water_mirror_clip_all() {
	for (ObjectModel *model : water_mirror_clip_models_) {
		model->refresh_water_mirror_clip();
	}
}


void ObjectModel::set_material_and_auxiliary_parameter(
		const Ref<ShaderMaterial> &p_material,
		const Ref<ShaderMaterial> &p_auxiliary, const StringName &p_name,
		const Variant &p_value) {
	if (p_material.is_null()) {
		return;
	}
	p_material->set_shader_parameter(p_name, p_value);
	if (p_auxiliary.is_valid()) {
		p_auxiliary->set_shader_parameter(p_name, p_value);
	}
}

Ref<ShaderMaterial> ObjectModel::material_for_index(int p_material_array_index) {
	const int64_t cache_key = int64_t(p_material_array_index);
	const Ref<ShaderMaterial> *cached = material_cache_.getptr(cache_key);
	if (cached != nullptr) {
		return *cached;
	}
	// The surface's material index addresses the MTRL row by its authored
	// index first, else by array position (ObjectData::find_material_array_index).
	const int array_index = object_data_.is_valid()
			? object_data_->find_material_array_index(p_material_array_index)
			: -1;
	Ref<ShaderMaterial> postmultiply;
	const Ref<ShaderMaterial> material =
			create_material(array_index, postmultiply);
	if (material.is_valid() && render_rung_override_ != kRenderRungFromWaterSide) {
		material->set_render_priority(render_rung_override_);
	}
	material_cache_[cache_key] = material;
	if (postmultiply.is_valid()) {
		postmultiply_cache_[cache_key] = postmultiply;
	}
	return material;
}

Ref<ShaderMaterial> ObjectModel::postmultiply_material_for_index(
		int p_material_array_index) const {
	const Ref<ShaderMaterial> *cached =
			postmultiply_cache_.getptr(int64_t(p_material_array_index));
	return cached != nullptr ? *cached : Ref<ShaderMaterial>();
}

Ref<ShaderMaterial> ObjectModel::create_material(int p_array_index,
		Ref<ShaderMaterial> &r_postmultiply) {
	r_postmultiply = Ref<ShaderMaterial>();
	Ref<ShaderMaterial> material;
	material.instantiate();
	// The authored MTRL row; a surface with no row builds the FF_ST_OP defaults.
	MaterialInfo info;
	const bool has_info = object_data_.is_valid() &&
			object_data_->get_material_info(p_array_index, info);
	String shader_tag = has_info ? info.shader_tag : String("FF_ST_OP");
	if (shader_tag.is_empty()) {
		shader_tag = "FF_ST_OP";
	}
	int material_flags = 0;
	if (has_info && info.alpha_test_enabled) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
	}
	if (has_info && info.alpha_invert) {
		material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
	}
	if (has_info && info.two_sided) {
		material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
	}
	const int emissive_type = (has_info && info.emissive) ? 2 : 0;
	const int is_glass_flag = (has_info && info.is_glass) ? 1 : 0;
	const int alpha_test_byte = has_info ? info.alpha_test : 0;
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();

	// Textures resolve before the shader key: the detail stage only survives
	// classification when the secondary texture actually resolved.
	Ref<Texture2D> diffuse;
	Ref<Texture2D> detail;
	Ref<Texture2D> normal;
	if (object_data_.is_valid() && p_array_index >= 0) {
		diffuse = object_data_->load_material_slot_texture(p_array_index, 1);
		detail = object_data_->load_material_slot_texture(p_array_index, 2);
		normal = object_data_->load_material_slot_texture(p_array_index, 3);
		if (normal.is_null()) {
			normal = object_data_->load_material_slot_texture(p_array_index, 4);
		}
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
		r_postmultiply = proxy_material;
	}
	// Each stage also carries its texture's last retail mip level
	// (material_texture_max_lod): pixel-built textures stop at 4x4.
	const Ref<Texture> bound_diffuse = diffuse.is_valid()
			? Ref<Texture>(diffuse)
			: opennova::prepare_material_texture({}, {}, 0);
	set_material_and_auxiliary_parameter(material, r_postmultiply, "u_diffuse", bound_diffuse);
	set_material_and_auxiliary_parameter(material, r_postmultiply, "u_diffuse_max_lod",
			opennova::material_texture_max_lod(bound_diffuse));
	if (detail.is_valid()) {
		material->set_shader_parameter("u_detail", detail);
		material->set_shader_parameter("u_detail_max_lod",
				opennova::material_texture_max_lod(detail));
	}
	const Ref<Texture> bound_normal = normal.is_valid()
			? Ref<Texture>(normal)
			: Ref<Texture>(solid_colour_texture(Color(0.5f, 0.5f, 1.0f, 1.0f)));
	material->set_shader_parameter("u_normal_map", bound_normal);
	material->set_shader_parameter("u_normal_max_lod",
			opennova::material_texture_max_lod(bound_normal));
	if ((material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0) {
		// The ref byte feeds the compare exactly; the shader keeps a > ref
		// (invert: a <= ref) [orig: CGfxDevice_SetAlphaTestRef @ 0x6770a0].
		set_material_and_auxiliary_parameter(material, r_postmultiply, "u_alpha_test_threshold",
				float(alpha_test_byte) / 255.0f);
		set_material_and_auxiliary_parameter(material, r_postmultiply, "u_alpha_test_invert",
				(material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0
						? 1.0f
						: 0.0f);
	} else {
		set_material_and_auxiliary_parameter(material, r_postmultiply, "u_alpha_test_threshold", 0.0f);
		set_material_and_auxiliary_parameter(material, r_postmultiply, "u_alpha_test_invert", 0.0f);
	}
	// The draw-invariant effect parameters: routed static colours (ReflectColor
	// W = 1) and constant generators over the effect defaults. Dynamic
	// generators overwrite theirs every runtime frame.
	opennova::renderer::MaterialRuntime initial;
	if (object_data_.is_valid()) {
		object_data_->material_static_runtime_native(p_array_index, initial);
	}
	set_material_and_auxiliary_parameter(material, r_postmultiply, "u_reflect_color",
			Vector4(initial.reflect[0], initial.reflect[1], initial.reflect[2],
					initial.reflect[3]));
	// The PANM evaluator supplies the complete two-row affine transform.
	material->set_shader_parameter("u_uv_transform_u",
			Vector3(initial.uv.m00, initial.uv.m10, initial.uv.m20));
	material->set_shader_parameter("u_uv_transform_v",
			Vector3(initial.uv.m01, initial.uv.m11, initial.uv.m21));
	material->set_shader_parameter("u_rgb_mod",
			Vector3(initial.rgb_r, initial.rgb_g, initial.rgb_b));
	material->set_shader_parameter("u_alpha_mod", initial.alpha);
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

void ObjectModel::collect_anim_frames(int p_material_index) {
	if (anim_frames_by_mat_.has(p_material_index) || object_data_.is_null()) {
		return;
	}
	const PackedStringArray frame_names =
			object_data_->get_material_anim_frames(p_material_index, 1);
	if (frame_names.size() <= 1) {
		return;
	}
	// Each frame is its own texture row, dispatched by that row's type like
	// any other stage texture.
	Array frames;
	for (int frame = 0; frame < frame_names.size(); ++frame) {
		frames.append(object_data_->load_material_anim_frame(p_material_index, 1, frame));
	}
	anim_frames_by_mat_[p_material_index] = frames;
}

Ref<ImageTexture> ObjectModel::solid_colour_texture(const Color &p_color) {
	const Ref<Image> image = Image::create(1, 1, false, Image::FORMAT_RGBA8);
	image->set_pixel(0, 0, p_color);
	return ImageTexture::create_from_image(image);
}

// A surface material needs per-frame evaluation only if a parameter its
// effect reads can change per draw (renderer::material_runtime_is_dynamic).
bool ObjectModel::material_runtime_is_dynamic(int p_material_index) const {
	if (object_data_.is_null()) {
		return false;
	}
	return object_data_->material_runtime_dynamic_native(p_material_index);
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
