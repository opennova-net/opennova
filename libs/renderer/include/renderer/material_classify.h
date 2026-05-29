#pragma once

// Material shader classification - derives rendering family + lighting/blend
// caps from a `.3di` MTRL shader_tag and per-material flag bits.
//
// Pure C++, no Godot deps.  Used by the editor (godot/engine/object) to pick
// shader code and by the runtime/server when they need to know whether a
// surface is glass / two-sided / additively-blended.
//
// ONED uses an exact static descriptor table derived from OED's
// gMaterialInfoTable plus the original one-.fx-per-shader effects. Unknown
// tags stay unknown; classification does not guess from string fragments.

#include <cstdint>
#include <string>

namespace renderer {

enum class ObjectBlendMode : uint8_t {
	Opaque,
	AlphaBlend,
	Additive,
	Multiplicative,
};

enum class ObjectShaderFamily : uint8_t {
	Unknown,
	FixedFunction,
	Phong,
	Flag,
	Dot3,
	Environment,
	Glass,
};

enum class ObjectNormalSpace : uint8_t {
	None,
	Tangent,
	Object,
};

struct ObjectMaterialClassification {
	bool known_shader = false;
	ObjectShaderFamily family = ObjectShaderFamily::Unknown;
	ObjectBlendMode blend = ObjectBlendMode::Opaque;
	bool is_emissive = false;
	bool is_luminance = false;
	bool is_two_sided = false;
	bool needs_normal_map = false;
	bool is_glass = false;
	bool uses_environment = false;
	bool alpha_test = false;
	bool alpha_test_invert = false;
	float alpha_test_value = 0.0f;
	bool has_detail = false;
	bool is_skinned = false;
	bool uses_specular = false;
	bool normal_uses_uv2 = false;
	ObjectNormalSpace normal_space = ObjectNormalSpace::None;
};

const char *object_shader_family_name(ObjectShaderFamily family);

// Classify a material from its shader tag string + binary 3DI flag fields.
// `material_flags` is the MTRL flags byte (THREEDI_MATERIAL_FLAG_*);
// `emissive_type` is 2 for *_LUM variants; `is_glass_flag` mirrors the
// per-MTRL glass override; `alpha_test_value_byte` is converted to 0..1.
ObjectMaterialClassification classify_object_material(const std::string &shader_name,
                                                       uint8_t material_flags,
                                                       uint8_t emissive_type,
                                                       uint8_t is_glass_flag,
                                                       uint8_t alpha_test_value_byte);

} // namespace renderer
