#pragma once

// Renderer-neutral object shader pipeline description.
//
// The engine classifies authored material facts and returns this typed
// descriptor. Backend adapters select their own checked-in shader resources
// and bind the capabilities; the engine never emits backend shader source.

#include "renderer/material_classify.h"

#include <cstdint>

namespace renderer {

enum ObjectShaderCapBits : uint32_t {
	OSCAP_BLEND_MASK   = 0x00000003u, // ObjectBlendMode value
	OSCAP_FAMILY_MASK  = 0x0000001cu, // ObjectShaderFamily << 2
	OSCAP_FAMILY_SHIFT = 2u,
	OSCAP_TWO_SIDED    = 0x00000020u,
	OSCAP_ALPHA_TEST   = 0x00000040u,
	OSCAP_ALPHA_INVERT = 0x00000080u,
	OSCAP_EMISSIVE     = 0x00000100u,
	OSCAP_LUMINANCE    = 0x00000200u,
	OSCAP_NORMAL_MAP   = 0x00000400u,
	OSCAP_OBJECT_SPACE = 0x00000800u,
	OSCAP_DETAIL       = 0x00001000u,
	OSCAP_SPECULAR     = 0x00002000u,
	OSCAP_GLASS        = 0x00004000u,
	OSCAP_NORMAL_UV2   = 0x00008000u,
	// vsTracer soft edge: unlit color x |dot(eye, normal)|^2
	// [orig: Tracer.fx vsTracer — D-RMAT-2, ported at REN-4].
	// (The glow-copy capability — is_glow_capable — deliberately is not a key
	// bit: it selects the Q3/bloom duplicate, not the normal-pass look.)
	OSCAP_VIEW_FADE    = 0x00010000u,
};

using ObjectShaderKey = uint32_t;

enum class ObjectDepthPolicy : uint8_t {
	Opaque,
	TransparentNoWrite,
	AlphaPrepass,
};

enum class ObjectCullPolicy : uint8_t {
	Back,
	Disabled,
};

// These names make the two tracked D-RLIT-5 stand-ins explicit. A future
// cubemap/Phong-map port changes this typed contract instead of searching a
// generated source string for an implementation detail.
enum class ObjectEnvironmentSource : uint8_t {
	None,
	HemisphereApproximation,
};

enum class ObjectSpecularSource : uint8_t {
	None,
	AnalyticPow16,
};

// Shader execution topology. These are the combinations reachable from the
// canonical material descriptor table plus the runtime's missing-detail
// downgrade. Backend adapters fail closed on Unsupported instead of growing
// a runtime uber-shader or guessing a fallback.
enum class ObjectShaderTechnique : uint8_t {
	Unsupported,
	Fixed,
	FixedDetail,
	SelfLit,
	SelfLitDetail,
	Tracer,
	Flag,
	FlagSelfLit,
	PhongTangentDiffuse,
	PhongTangentSpecular,
	PhongObjectDiffuse,
	PhongObjectSpecular,
	Dot3Tangent,
	Dot3TangentDetail,
	Dot3Object,
	Dot3ObjectDetail,
	EnvironmentTangent,
	EnvironmentTangentSpecular,
	Glass,
};

struct ObjectShaderPipelineDescriptor {
	ObjectShaderKey key = 0;
	ObjectShaderFamily family = ObjectShaderFamily::Unknown;
	ObjectBlendMode blend = ObjectBlendMode::Opaque;
	ObjectDepthPolicy depth = ObjectDepthPolicy::Opaque;
	ObjectCullPolicy cull = ObjectCullPolicy::Back;
	ObjectEnvironmentSource environment_source = ObjectEnvironmentSource::None;
	ObjectSpecularSource specular_source = ObjectSpecularSource::None;
	ObjectShaderTechnique technique = ObjectShaderTechnique::Unsupported;
	ObjectNormalSpace normal_space = ObjectNormalSpace::None;
	bool writes_alpha = false;
	bool alpha_test = false;
	bool alpha_test_invert = false;
	bool two_sided = false;
	bool emissive = false;
	bool luminance = false;
	bool uses_normal_map = false;
	bool normal_uses_uv2 = false;
	bool uses_detail = false;
	bool uses_specular = false;
	bool glass = false;
	bool view_angle_fade = false;
};

// Pack a classification into the stable 32-bit material/cache key.
ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls);

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key);
ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key);

// Describe the complete normal-pass pipeline selected by a key. Pure data;
// adapters decide how each policy maps to their renderer.
ObjectShaderPipelineDescriptor describe_object_shader_pipeline(ObjectShaderKey key);

} // namespace renderer
