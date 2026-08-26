#include "object/nova_object_shader_cache.h"

#include "object/nova_object_model.h"

#include "renderer/material_classify.h"
#include "renderer/material_descriptor.h"
#include "renderer/object_shader_template.h"
#include "renderer/render_order.h"
#include "threedi/threedi_3di3.h"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

namespace godot {

namespace {

const char *shader_technique_directory(renderer::ObjectShaderTechnique technique) {
	switch (technique) {
		case renderer::ObjectShaderTechnique::Unsupported: return nullptr;
		case renderer::ObjectShaderTechnique::Fixed: return "fixed";
		case renderer::ObjectShaderTechnique::FixedSkinned: return "fixed_skinned";
		case renderer::ObjectShaderTechnique::FixedDetail: return "fixed_detail";
		case renderer::ObjectShaderTechnique::SelfLit: return "self_lit";
		case renderer::ObjectShaderTechnique::SelfLitDetail: return "self_lit_detail";
		case renderer::ObjectShaderTechnique::Tracer: return "tracer";
		case renderer::ObjectShaderTechnique::Flag: return "flag";
		case renderer::ObjectShaderTechnique::PhongTangentDiffuse:
			return "phong_tangent_diffuse";
		case renderer::ObjectShaderTechnique::PhongTangentSpecular:
			return "phong_tangent_specular";
		case renderer::ObjectShaderTechnique::PhongTangentSpecularSkinned:
			return "phong_tangent_specular_skinned";
		case renderer::ObjectShaderTechnique::PhongObjectDiffuse:
			return "phong_object_diffuse";
		case renderer::ObjectShaderTechnique::PhongObjectSpecular:
			return "phong_object_specular";
		case renderer::ObjectShaderTechnique::PhongObjectSpecularPhongMap:
			return "phong_object_specular_phong_map";
		case renderer::ObjectShaderTechnique::Dot3Tangent: return "dot3_tangent";
		case renderer::ObjectShaderTechnique::Dot3TangentDetail:
			return "dot3_tangent_detail";
		case renderer::ObjectShaderTechnique::Dot3TangentSkinned:
			return "dot3_tangent_skinned";
		case renderer::ObjectShaderTechnique::Dot3TangentDetailSkinned:
			return "dot3_tangent_detail_skinned";
		case renderer::ObjectShaderTechnique::Dot3Object: return "dot3_object";
		case renderer::ObjectShaderTechnique::Dot3ObjectDetail:
			return "dot3_object_detail";
		case renderer::ObjectShaderTechnique::EnvironmentMirror:
			return "environment_tangent";
		case renderer::ObjectShaderTechnique::EnvironmentMirrorTextured:
			return "environment_tangent_textured";
		case renderer::ObjectShaderTechnique::EnvironmentPhong:
			return "environment_tangent_specular";
		case renderer::ObjectShaderTechnique::GlassFixed: return "glass";
		case renderer::ObjectShaderTechnique::GlassSkinned: return "glass_skinned";
	}
	return nullptr;
}

const char *shader_policy_name(
		const renderer::ObjectShaderPipelineDescriptor &pipeline) {
	if (pipeline.alpha_test) {
		switch (pipeline.blend) {
			case renderer::ObjectBlendMode::Opaque: return "cutout_mix";
			case renderer::ObjectBlendMode::AlphaBlend: return "cutout_alpha";
			case renderer::ObjectBlendMode::Additive: return "cutout_additive";
			case renderer::ObjectBlendMode::Multiplicative:
				return "cutout_multiplicative";
		}
	}
	switch (pipeline.blend) {
		case renderer::ObjectBlendMode::Opaque: return "opaque";
		case renderer::ObjectBlendMode::AlphaBlend: return "alpha";
		case renderer::ObjectBlendMode::Additive: return "additive";
		case renderer::ObjectBlendMode::Multiplicative: return "multiplicative";
	}
	return nullptr;
}

bool technique_supports_blend(renderer::ObjectShaderTechnique technique,
		renderer::ObjectBlendMode blend) {
	switch (technique) {
		case renderer::ObjectShaderTechnique::Fixed:
		case renderer::ObjectShaderTechnique::FixedDetail:
		case renderer::ObjectShaderTechnique::SelfLit:
		case renderer::ObjectShaderTechnique::SelfLitDetail:
			return blend == renderer::ObjectBlendMode::Opaque ||
					blend == renderer::ObjectBlendMode::AlphaBlend ||
					blend == renderer::ObjectBlendMode::Additive;
		case renderer::ObjectShaderTechnique::Tracer:
		case renderer::ObjectShaderTechnique::GlassFixed:
		case renderer::ObjectShaderTechnique::GlassSkinned:
			return blend == renderer::ObjectBlendMode::Additive;
		case renderer::ObjectShaderTechnique::FixedSkinned:
			return blend == renderer::ObjectBlendMode::Opaque;
		case renderer::ObjectShaderTechnique::Unsupported:
			return false;
		default:
			return blend == renderer::ObjectBlendMode::Opaque;
	}
}

String shader_resource_path(
		const renderer::ObjectShaderPipelineDescriptor &pipeline) {
	const char *technique = shader_technique_directory(pipeline.technique);
	const char *policy = shader_policy_name(pipeline);
	if (technique == nullptr || policy == nullptr ||
			!technique_supports_blend(pipeline.technique, pipeline.blend)) {
		return String();
	}
	const char *cull_suffix = pipeline.cull == renderer::ObjectCullPolicy::Disabled
			? "_double_sided"
			: "";
	return String("res://shaders/object/") + technique + "/" + policy +
			cull_suffix + ".gdshader";
}

// Render_CreateSystemTextures @0x58aca0 builds gsys_phong as a 256x256 RGBA8 lookup
// (docs/render/render-lighting-re.md).
// X is N.L (also copied verbatim to alpha); Y is N.H; RGB are the truncated
// 255*x^(4,16,64) curves. The source multiplier is the exact binary32
// 0x3b808081 value loaded by retail rather than an idealized 1/255.
// renderer/object_shader_template owns the byte-exact cited contract.
Ref<ImageTexture> create_retail_phong_map_texture() {
	constexpr int kSize = 256;
	PackedByteArray pixels;
	pixels.resize(kSize * kSize * 4);
	for (int y = 0; y < kSize; ++y) {
		for (int x_coord = 0; x_coord < kSize; ++x_coord) {
			const renderer::ObjectPhongMapTexel texel =
					renderer::object_phong_map_texel(
							static_cast<uint8_t>(x_coord), static_cast<uint8_t>(y));
			const int offset = (y * kSize + x_coord) * 4;
			pixels.set(offset + 0, texel.red_pow4);
			pixels.set(offset + 1, texel.green_pow16);
			pixels.set(offset + 2, texel.blue_pow64);
			pixels.set(offset + 3, texel.alpha_ndotl);
		}
	}
	const Ref<Image> image = Image::create_from_data(
			kSize, kSize, false, Image::FORMAT_RGBA8, pixels);
	return ImageTexture::create_from_image(image);
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
	phong_map_texture.unref();
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
	ClassDB::bind_integer_constant(get_class_static(), "", "CAP_DETAIL", renderer::OSCAP_DETAIL);

	// ObjectBlendMode (renderer::ObjectBlendMode) constants.
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_OPAQUE", static_cast<int64_t>(renderer::ObjectBlendMode::Opaque));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_ALPHA", static_cast<int64_t>(renderer::ObjectBlendMode::AlphaBlend));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_ADDITIVE", static_cast<int64_t>(renderer::ObjectBlendMode::Additive));
	ClassDB::bind_integer_constant(get_class_static(), "", "BLEND_MULTIPLICATIVE", static_cast<int64_t>(renderer::ObjectBlendMode::Multiplicative));

	// The witnessed transparent ordering ladder, single-sourced from
	// engine/runtime/renderer/render_order (maturity REN-3): sky -> far-water-side
	// alpha -> water -> camera-side alpha -> overlays -> sun glow
	// [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0;
	// docs/render/render-order-re.md].
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SKY_STARS", renderer::kRungSkyStars);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SKY_BODY", renderer::kRungSkyBody);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_ALPHA_FAR_SIDE", renderer::kRungAlphaFarSide);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_WATER", renderer::kRungWater);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_ALPHA_CAMERA_SIDE", renderer::kRungAlphaCameraSide);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_OVERLAY_FX", renderer::kRungOverlayFx);
	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_RUNG_SUN_GLOW", renderer::kRungSunGlow);
}

Ref<Shader> ObjectShaderCache::get_shader_for_key(int32_t key) {
	const uint32_t ukey = static_cast<uint32_t>(key);
	auto it = cache.find(ukey);
	if (it != cache.end()) {
		return it->second;
	}
	const renderer::ObjectShaderPipelineDescriptor pipeline =
			renderer::describe_object_shader_pipeline(ukey);
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
	const renderer::ObjectShaderPipelineDescriptor pipeline =
			renderer::describe_object_shader_pipeline(static_cast<uint32_t>(key));
	if (pipeline.technique ==
			renderer::ObjectShaderTechnique::PhongObjectSpecularPhongMap) {
		if (phong_map_texture.is_null()) {
			phong_map_texture = create_retail_phong_map_texture();
		}
		ERR_FAIL_COND_MSG(phong_map_texture.is_null(),
				"Retail PhongMap texture could not be created");
		material->set_shader_parameter("u_phong_map", phong_map_texture);
	}
}

int32_t ObjectShaderCache::classify(const String &shader_tag,
		int32_t material_flags,
		int32_t emissive_type,
		int32_t is_glass_flag,
		int32_t alpha_test_byte) {
	const std::string tag = shader_tag.utf8().get_data();
	const auto cls = renderer::classify_object_material(
			tag,
			static_cast<uint8_t>(material_flags),
			static_cast<uint8_t>(emissive_type),
			static_cast<uint8_t>(is_glass_flag),
			static_cast<uint8_t>(alpha_test_byte));
	return static_cast<int32_t>(renderer::build_object_shader_key(cls));
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
		return renderer::kRungAlphaCameraSide;
	}
	const renderer::TransparentQueue side =
			renderer::transparent_queue_for(world_height, water_split_height);
	return renderer::transparent_rung_for(side, water_camera_above);
}

PackedStringArray ObjectShaderCache::get_known_shader_tags() const {
	PackedStringArray tags;
	tags.resize(static_cast<int64_t>(renderer::kMaterialDescriptorTableCount));
	for (size_t i = 0; i < renderer::kMaterialDescriptorTableCount; ++i) {
		tags[static_cast<int64_t>(i)] =
				String(renderer::kMaterialDescriptorTable[i].name);
	}
	return tags;
}

} // namespace godot
