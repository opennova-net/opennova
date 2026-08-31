#include "object/object_shader_cache.h"

#include "object/object_model.h"

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/object_shader_template.h>
#include <runtime/renderer/render_order.h>
#include <formats/threedi/threedi_3di3.h>

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

namespace godot {

namespace {

const char *shader_technique_directory(opennova::renderer::ObjectShaderTechnique technique) {
	switch (technique) {
		case opennova::renderer::ObjectShaderTechnique::Unsupported: return nullptr;
		case opennova::renderer::ObjectShaderTechnique::Fixed: return "fixed";
		case opennova::renderer::ObjectShaderTechnique::FixedSkinned: return "fixed_skinned";
		case opennova::renderer::ObjectShaderTechnique::FixedDetail: return "fixed_detail";
		case opennova::renderer::ObjectShaderTechnique::SelfLit: return "self_lit";
		case opennova::renderer::ObjectShaderTechnique::SelfLitDetail: return "self_lit_detail";
		case opennova::renderer::ObjectShaderTechnique::Tracer: return "tracer";
		case opennova::renderer::ObjectShaderTechnique::Flag: return "flag";
		case opennova::renderer::ObjectShaderTechnique::PhongTangentDiffuse:
			return "phong_tangent_diffuse";
		case opennova::renderer::ObjectShaderTechnique::PhongTangentSpecular:
			return "phong_tangent_specular";
		case opennova::renderer::ObjectShaderTechnique::PhongTangentSpecularSkinned:
			return "phong_tangent_specular_skinned";
		case opennova::renderer::ObjectShaderTechnique::PhongObjectDiffuse:
			return "phong_object_diffuse";
		case opennova::renderer::ObjectShaderTechnique::PhongObjectSpecular:
			return "phong_object_specular";
		case opennova::renderer::ObjectShaderTechnique::PhongObjectSpecularPhongMap:
			return "phong_object_specular_phong_map";
		case opennova::renderer::ObjectShaderTechnique::Dot3Tangent: return "dot3_tangent";
		case opennova::renderer::ObjectShaderTechnique::Dot3TangentDetail:
			return "dot3_tangent_detail";
		case opennova::renderer::ObjectShaderTechnique::Dot3TangentSkinned:
			return "dot3_tangent_skinned";
		case opennova::renderer::ObjectShaderTechnique::Dot3TangentDetailSkinned:
			return "dot3_tangent_detail_skinned";
		case opennova::renderer::ObjectShaderTechnique::Dot3Object: return "dot3_object";
		case opennova::renderer::ObjectShaderTechnique::Dot3ObjectDetail:
			return "dot3_object_detail";
		case opennova::renderer::ObjectShaderTechnique::EnvironmentMirror:
			return "environment_tangent";
		case opennova::renderer::ObjectShaderTechnique::EnvironmentMirrorTextured:
			return "environment_tangent_textured";
		case opennova::renderer::ObjectShaderTechnique::EnvironmentPhong:
			return "environment_tangent_specular";
		case opennova::renderer::ObjectShaderTechnique::GlassFixed: return "glass";
		case opennova::renderer::ObjectShaderTechnique::GlassSkinned: return "glass_skinned";
	}
	return nullptr;
}

const char *shader_policy_name(
		const opennova::renderer::ObjectShaderPipelineDescriptor &pipeline) {
	if (pipeline.alpha_test) {
		switch (pipeline.blend) {
			case opennova::renderer::ObjectBlendMode::Opaque: return "cutout_mix";
			case opennova::renderer::ObjectBlendMode::AlphaBlend: return "cutout_alpha";
			case opennova::renderer::ObjectBlendMode::Additive: return "cutout_additive";
			case opennova::renderer::ObjectBlendMode::Multiplicative:
				return "cutout_multiplicative";
		}
	}
	switch (pipeline.blend) {
		case opennova::renderer::ObjectBlendMode::Opaque: return "opaque";
		case opennova::renderer::ObjectBlendMode::AlphaBlend: return "alpha";
		case opennova::renderer::ObjectBlendMode::Additive: return "additive";
		case opennova::renderer::ObjectBlendMode::Multiplicative: return "multiplicative";
	}
	return nullptr;
}

bool technique_supports_blend(opennova::renderer::ObjectShaderTechnique technique,
		opennova::renderer::ObjectBlendMode blend) {
	switch (technique) {
		case opennova::renderer::ObjectShaderTechnique::Fixed:
		case opennova::renderer::ObjectShaderTechnique::FixedDetail:
		case opennova::renderer::ObjectShaderTechnique::SelfLit:
		case opennova::renderer::ObjectShaderTechnique::SelfLitDetail:
			return blend == opennova::renderer::ObjectBlendMode::Opaque ||
					blend == opennova::renderer::ObjectBlendMode::AlphaBlend ||
					blend == opennova::renderer::ObjectBlendMode::Additive;
		case opennova::renderer::ObjectShaderTechnique::Tracer:
		case opennova::renderer::ObjectShaderTechnique::GlassFixed:
		case opennova::renderer::ObjectShaderTechnique::GlassSkinned:
			return blend == opennova::renderer::ObjectBlendMode::Additive;
		case opennova::renderer::ObjectShaderTechnique::FixedSkinned:
			return blend == opennova::renderer::ObjectBlendMode::Opaque;
		case opennova::renderer::ObjectShaderTechnique::Unsupported:
			return false;
		default:
			return blend == opennova::renderer::ObjectBlendMode::Opaque;
	}
}

String shader_resource_path(
		const opennova::renderer::ObjectShaderPipelineDescriptor &pipeline) {
	const char *technique = shader_technique_directory(pipeline.technique);
	const char *policy = shader_policy_name(pipeline);
	if (technique == nullptr || policy == nullptr ||
			!technique_supports_blend(pipeline.technique, pipeline.blend)) {
		return String();
	}
	const char *cull_suffix = pipeline.cull == opennova::renderer::ObjectCullPolicy::Disabled
			? "_double_sided"
			: "";
	return String("res://shaders/object/") + technique + "/" + policy +
			cull_suffix + ".gdshader";
}

} // namespace

ObjectShaderCache *ObjectShaderCache::singleton = nullptr;

ObjectShaderCache *ObjectShaderCache::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(ObjectShaderCache);
	}
	return singleton;
}

void ObjectShaderCache::destroy_singleton() {
	if (singleton != nullptr) {
		memdelete(singleton); // ~ObjectShaderCache nulls the static.
	}
}

ObjectShaderCache::ObjectShaderCache() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

ObjectShaderCache::~ObjectShaderCache() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void ObjectShaderCache::clear() {
	cache.clear();
}

void ObjectShaderCache::_bind_methods() {
	ClassDB::bind_static_method("ObjectShaderCache", D_METHOD("get_singleton"), &ObjectShaderCache::get_singleton);
	ClassDB::bind_method(D_METHOD("get_shader_for_key", "key"), &ObjectShaderCache::get_shader_for_key);
	ClassDB::bind_method(D_METHOD("configure_material_for_key", "material", "key"), &ObjectShaderCache::configure_material_for_key);
	ClassDB::bind_method(D_METHOD("classify", "shader_tag", "material_flags", "emissive_type", "is_glass_flag", "alpha_test_byte"), &ObjectShaderCache::classify);
	ClassDB::bind_method(D_METHOD("get_known_shader_tags"), &ObjectShaderCache::get_known_shader_tags);
	ClassDB::bind_method(D_METHOD("clear"), &ObjectShaderCache::clear);
	ClassDB::bind_method(D_METHOD("set_water_plane", "height", "camera_above"), &ObjectShaderCache::set_water_plane);
	ClassDB::bind_method(D_METHOD("clear_water_plane"), &ObjectShaderCache::clear_water_plane);
	ClassDB::bind_method(D_METHOD("has_water_plane"), &ObjectShaderCache::has_water_plane);
	ClassDB::bind_method(D_METHOD("alpha_rung_for_height", "world_height"), &ObjectShaderCache::alpha_rung_for_height);

	// The per-material 3DI flag byte, single-sourced from engine/formats/threedi so
	// GDScript stops re-declaring the values (maturity REN-2 / ENG-4 leg).
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_ALPHA_TEST", THREEDI_MATERIAL_FLAG_ALPHA_TEST);
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_ALPHA_INVERT", THREEDI_MATERIAL_FLAG_ALPHA_INVERT);
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_TWO_SIDED", THREEDI_MATERIAL_FLAG_TWO_SIDED);

	// The packed-key DETAIL capability bit, single-sourced from
	// engine/runtime/renderer: the reimpl material path masks it off when a material's
	// secondary texture fails to resolve, so the _MT Modulate2x stage is
	// dropped exactly like retail drops a NULL-texture stage instead of
	// running x2 over a placeholder (render-material-re.md §FF technique
	// tables).
	ClassDB::bind_integer_constant(get_class_static(), "", "CAP_DETAIL", opennova::renderer::OSCAP_DETAIL);

	// ObjectBlendMode (opennova::renderer::ObjectBlendMode) constants.
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_OPAQUE", static_cast<int64_t>(opennova::renderer::ObjectBlendMode::Opaque));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_ALPHA", static_cast<int64_t>(opennova::renderer::ObjectBlendMode::AlphaBlend));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_ADDITIVE", static_cast<int64_t>(opennova::renderer::ObjectBlendMode::Additive));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_MULTIPLICATIVE", static_cast<int64_t>(opennova::renderer::ObjectBlendMode::Multiplicative));

	// The witnessed transparent ordering ladder, single-sourced from
	// engine/runtime/renderer/render_order (maturity REN-3): sky -> far-water-side
	// alpha -> water -> camera-side alpha -> overlays -> sun glow
	// [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0;
	// docs/render/render-order-re.md].
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SKY_STARS", opennova::renderer::kRungSkyStars);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SKY_BODY", opennova::renderer::kRungSkyBody);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_ALPHA_FAR_SIDE", opennova::renderer::kRungAlphaFarSide);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_WATER", opennova::renderer::kRungWater);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_ALPHA_CAMERA_SIDE", opennova::renderer::kRungAlphaCameraSide);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_OVERLAY_FX", opennova::renderer::kRungOverlayFx);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SUN_GLOW", opennova::renderer::kRungSunGlow);
}

Ref<Shader> ObjectShaderCache::get_shader_for_key(int32_t key) {
	const uint32_t ukey = static_cast<uint32_t>(key);
	auto it = cache.find(ukey);
	if (it != cache.end()) {
		return it->second;
	}
	const opennova::renderer::ObjectShaderPipelineDescriptor pipeline =
			opennova::renderer::describe_object_shader_pipeline(ukey);
	const String path = shader_resource_path(pipeline);
	ERR_FAIL_COND_V_MSG(path.is_empty(), Ref<Shader>(),
			"Object shader key selects an unsupported family/blend pipeline");
	const Ref<Shader> shader = ResourceLoader::get_singleton()->load(path, "Shader");
	ERR_FAIL_COND_V_MSG(shader.is_null(), Ref<Shader>(),
			String("Object shader resource failed to load: ") + path);
	cache[ukey] = shader;
	return shader;
}

void ObjectShaderCache::configure_material_for_key(
		const Ref<ShaderMaterial> &material, int32_t key) {
	ERR_FAIL_COND_MSG(material.is_null(), "Cannot configure a null ShaderMaterial");
	const Ref<Shader> shader = get_shader_for_key(key);
	ERR_FAIL_COND_MSG(shader.is_null(), "Object shader resource is unavailable");
	material->set_shader(shader);
	// ADR 0043: the PhongMap technique's lobe blend maps onto roughness in
	// the lit pipeline; the gsys_phong lookup texture is no longer bound
	// (the byte-exact texel contract stays engine-side as the intent
	// witness, pinned by the T1 vectors).
}

int32_t ObjectShaderCache::classify(const String &shader_tag,
		int32_t material_flags,
		int32_t emissive_type,
		int32_t is_glass_flag,
		int32_t alpha_test_byte) {
	const std::string tag = shader_tag.utf8().get_data();
	const auto cls = opennova::renderer::classify_object_material(
			tag,
			static_cast<uint8_t>(material_flags),
			static_cast<uint8_t>(emissive_type),
			static_cast<uint8_t>(is_glass_flag),
			static_cast<uint8_t>(alpha_test_byte));
	return static_cast<int32_t>(opennova::renderer::build_object_shader_key(cls));
}

void ObjectShaderCache::set_water_plane(float height, bool camera_above) {
	if (water_split_set && water_split_height == height &&
			water_camera_above == camera_above) {
		return;
	}
	water_split_height = height;
	water_camera_above = camera_above;
	water_split_set = true;
	++water_plane_generation;
	ObjectModel::mark_render_order_dirty_all();
}

void ObjectShaderCache::clear_water_plane() {
	if (!water_split_set) {
		return;
	}
	water_split_set = false;
	++water_plane_generation;
	ObjectModel::mark_render_order_dirty_all();
}

bool ObjectShaderCache::has_water_plane() const {
	return water_split_set;
}

uint64_t ObjectShaderCache::get_water_plane_generation() const {
	return water_plane_generation;
}

int32_t ObjectShaderCache::alpha_rung_for_height(float world_height) const {
	// The water-plane transparent bracket [orig: @ 0x5d932e..0x5d9354 vs
	// g_WaterSplitHeightFloat @ 0x8437C4]. No water in the session -> the
	// default camera-side rung. The frame owner publishes the effective render
	// eye side after camera placement, so the ladder mirrors underwater.
	if (!water_split_set) {
		return opennova::renderer::kRungAlphaCameraSide;
	}
	const opennova::renderer::TransparentQueue side =
			opennova::renderer::transparent_queue_for(world_height, water_split_height);
	return opennova::renderer::transparent_rung_for(side, water_camera_above);
}

PackedStringArray ObjectShaderCache::get_known_shader_tags() const {
	PackedStringArray tags;
	tags.resize(static_cast<int64_t>(opennova::renderer::kMaterialDescriptorTableCount));
	for (size_t i = 0; i < opennova::renderer::kMaterialDescriptorTableCount; ++i) {
		tags[static_cast<int64_t>(i)] =
				String(opennova::renderer::kMaterialDescriptorTable[i].name);
	}
	return tags;
}

} // namespace godot
