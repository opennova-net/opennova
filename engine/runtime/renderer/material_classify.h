#pragma once

// Material shader classification - derives rendering family + lighting/blend
// caps from a `.3di` MTRL shader_tag and per-material flag bits.
//
// Pure C++, no Godot deps. Used by renderer bindings to select a typed
// pipeline and by the runtime/server when they need to know whether a
// surface is glass / two-sided / additively-blended.
//
// The runtime uses an exact static descriptor table derived from the retail
// shader registry and the original one-.fx-per-shader effects. Unknown
// tags stay unknown; classification does not guess from string fragments.

#include <cstdint>
#include <string>

namespace opennova::renderer {

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
	// BmTxMirrT's NORMAL pass post-multiplies environment/specular by
	// Diffuse1; BumpMirrT is reflection-only.
	bool environment_textured = false;
	bool normal_uses_uv2 = false;
	// The 0x10000000 capability: this material renders a duplicate into the
	// glow/bloom queue (Q3), flushed by the bloom pass — the FF _LUM rows and
	// FFP_GLASS carry it at runtime [orig: Q3 copy gate on effect caps
	// @ 0x5d93b5; FrameFX_RenderGlowSource flush mode 4 @ 0x582a54].
	bool is_glow_capable = false;
	// vsTracer soft edge: color x |dot(eye, normal)|^2, unlit
	// [orig: Tracer.fx vsTracer — D-RMAT-2].
	bool view_angle_fade = false;
	ObjectNormalSpace normal_space = ObjectNormalSpace::None;
};

// Classify a material from its shader tag string + binary 3DI flag fields.
// `material_flags` is the MTRL flags byte (THREEDI_MATERIAL_FLAG_*);
// `emissive_type` is 2 for *_LUM variants; `is_glass_flag` mirrors the
// per-MTRL glass override; `alpha_test_value_byte` is converted to 0..1.
ObjectMaterialClassification classify_object_material(const std::string &shader_name,
                                                       uint8_t material_flags,
                                                       uint8_t emissive_type,
                                                       uint8_t is_glass_flag,
                                                       uint8_t alpha_test_value_byte);

}  // namespace opennova::renderer