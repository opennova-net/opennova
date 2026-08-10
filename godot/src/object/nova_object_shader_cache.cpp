#include "object/nova_object_shader_cache.h"

#include "oed/material_descriptor.h"
#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"
#include "renderer/render_order.h"
#include "threedi/threedi_3di3.h"

#include <godot_cpp/core/class_db.hpp>

namespace godot {

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
	ClassDB::bind_method(D_METHOD("classify", "shader_tag", "material_flags", "emissive_type", "is_glass_flag", "alpha_test_byte"), &ObjectShaderCache::classify);
	ClassDB::bind_method(D_METHOD("family_for_key", "key"), &ObjectShaderCache::family_for_key);
	ClassDB::bind_method(D_METHOD("blend_for_key", "key"), &ObjectShaderCache::blend_for_key);
	ClassDB::bind_method(D_METHOD("get_known_shader_tags"), &ObjectShaderCache::get_known_shader_tags);
	ClassDB::bind_method(D_METHOD("clear"), &ObjectShaderCache::clear);
	ClassDB::bind_method(D_METHOD("set_water_split_height", "height"), &ObjectShaderCache::set_water_split_height);
	ClassDB::bind_method(D_METHOD("clear_water_split_height"), &ObjectShaderCache::clear_water_split_height);
	ClassDB::bind_method(D_METHOD("has_water_split_height"), &ObjectShaderCache::has_water_split_height);
	ClassDB::bind_method(D_METHOD("alpha_rung_for_height", "world_height"), &ObjectShaderCache::alpha_rung_for_height);

	// The per-material 3DI flag byte, single-sourced from engine/formats/threedi so
	// GDScript stops re-declaring the values (maturity REN-2 / ENG-4 leg).
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_ALPHA_TEST", THREEDI_MATERIAL_FLAG_ALPHA_TEST);
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_ALPHA_INVERT", THREEDI_MATERIAL_FLAG_ALPHA_INVERT);
	ClassDB::bind_integer_constant(get_class_static(), "", "MATERIAL_FLAG_TWO_SIDED", THREEDI_MATERIAL_FLAG_TWO_SIDED);

	// The composed-key DETAIL capability bit, single-sourced from
	// engine/runtime/renderer: the reimpl material path masks it off when a material's
	// secondary texture fails to resolve, so the _MT Modulate2x stage is
	// dropped exactly like retail drops a NULL-texture stage instead of
	// running x2 over a placeholder (render-material-re.md §FF technique
	// tables).
	ClassDB::bind_integer_constant(get_class_static(), "", "CAP_DETAIL", renderer::OSCAP_DETAIL);

	// ObjectBlendMode (renderer::ObjectBlendMode) for blend_for_key callers.
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
	Ref<Shader> shader;
	shader.instantiate();
	const std::string code = renderer::compose_object_shader_glsl(ukey);
	shader->set_code(String(code.c_str()));
	cache[ukey] = shader;
	return shader;
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

int32_t ObjectShaderCache::family_for_key(int32_t key) const {
	return static_cast<int32_t>(renderer::decode_object_shader_family(static_cast<uint32_t>(key)));
}

int32_t ObjectShaderCache::blend_for_key(int32_t key) const {
	return static_cast<int32_t>(renderer::decode_object_shader_blend(static_cast<uint32_t>(key)));
}

void ObjectShaderCache::set_water_split_height(float height) {
	water_split_height = height;
	water_split_set = true;
}

void ObjectShaderCache::clear_water_split_height() {
	water_split_set = false;
}

bool ObjectShaderCache::has_water_split_height() const {
	return water_split_set;
}

int32_t ObjectShaderCache::alpha_rung_for_height(float world_height) const {
	// The water-plane transparent bracket [orig: @ 0x5d932e..0x5d9354 vs
	// g_WaterSplitHeightFloat @ 0x8437C4]. No water in the session -> the
	// default camera-side rung. The reimpl applies the camera-above case
	// statically (docs/render/render-order-re.md D-RORD-3 note).
	if (!water_split_set) {
		return renderer::kRungAlphaCameraSide;
	}
	const renderer::TransparentQueue side =
			renderer::transparent_queue_for(world_height, water_split_height);
	return renderer::transparent_rung_for(side, /*camera_above_water=*/true);
}

PackedStringArray ObjectShaderCache::get_known_shader_tags() const {
	PackedStringArray tags;
	tags.resize(static_cast<int64_t>(oed::kMaterialInfoTableCount));
	for (size_t i = 0; i < oed::kMaterialInfoTableCount; ++i) {
		tags[static_cast<int64_t>(i)] = String(oed::kMaterialInfoTable[i].name);
	}
	return tags;
}

} // namespace godot
