#pragma once

// Renderer-neutral object shader pipeline description.
//
// The engine classifies authored material facts and returns this typed
// descriptor. Backend bindings select their own checked-in shader resources
// and bind the capabilities; the engine never emits backend shader source.

#include <runtime/renderer/material_classify.h>

#include <cstdint>

namespace opennova::renderer {

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
	OSCAP_SKINNED      = 0x00020000u,
	OSCAP_ENV_TEXTURED = 0x00040000u,
};

using ObjectShaderKey = uint32_t;

enum class ObjectDepthPolicy : uint8_t {
	Opaque,
	TransparentNoWrite,
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
	AnalyticPow8DiffuseAlpha,
	PhongMapLookupDiffuseAlpha,
	AnalyticPow16,
};

struct ObjectPhongMapTexel {
	uint8_t red_pow4 = 0;
	uint8_t green_pow16 = 0;
	uint8_t blue_pow64 = 0;
	uint8_t alpha_ndotl = 0;

	bool operator==(const ObjectPhongMapTexel &other) const {
		return red_pow4 == other.red_pow4 &&
				green_pow16 == other.green_pow16 &&
				blue_pow64 == other.blue_pow64 &&
				alpha_ndotl == other.alpha_ndotl;
	}
};

// One byte-exact gsys_phong texel. X is N.L and Y is N.H.
// [orig: Render_CreateSystemTextures @ 0x58ad69..0x58aeb6].
ObjectPhongMapTexel object_phong_map_texel(uint8_t ndotl, uint8_t ndoth);

// Shader execution topology. These are the combinations reachable from the
// canonical material descriptor table plus the runtime's missing-detail
// downgrade. Backend bindings fail closed on Unsupported instead of growing
// a runtime uber-shader or guessing a fallback.
enum class ObjectShaderTechnique : uint8_t {
	Unsupported,
	Fixed,
	FixedSkinned,
	FixedDetail,
	SelfLit,
	SelfLitDetail,
	Tracer,
	Flag,
	PhongTangentDiffuse,
	PhongTangentSpecular,
	PhongTangentSpecularSkinned,
	PhongObjectDiffuse,
	PhongObjectSpecular,
	PhongObjectSpecularPhongMap,
	Dot3Tangent,
	Dot3TangentDetail,
	Dot3TangentSkinned,
	Dot3TangentDetailSkinned,
	Dot3Object,
	Dot3ObjectDetail,
	EnvironmentMirror,
	EnvironmentMirrorTextured,
	EnvironmentPhong,
	GlassFixed,
	GlassSkinned,
};

// The PROJSHAD pass is not a copy of NORMAL's blend policy. The four _FFP
// techniques compile material blend variants, the live file-effect passes
// force opaque ONE/ZERO state, and effects without a PROJSHAD declaration do
// not submit a fallback pass.
// [orig: _FFP.fx TBoringFFPProjShad; the 15 shipped shader PROJSHAD
// declarations; HLSLEffect_LoadFromFile @ 0x5AE690]
enum class ObjectProjectedShadowPolicy : uint8_t {
	NoPass,
	MaterialBlend,
	Opaque,
};

ObjectProjectedShadowPolicy object_projected_shadow_policy(
		ObjectShaderTechnique technique) noexcept;

// The PROJSHAD coverage source per technique: the alpha the black pass tests
// (alpha-test materials) or blends (MaterialBlend). _FFP's TBoringFFPProjShad
// keeps Diffuse1.a times the AlphaGenValue register (the FFP families'
// u_alpha_mod), its _MT variants also multiply Diffuse2.a over UV2; every file
// effect's PROJSHAD pass takes Diffuse1.a alone (vscPostBlackT1 /
// vscSkinPostBlackT1 write the black diffuse, the texture stage keeps the
// texture alpha); tracer/flag/glass declare no pass.
// [orig: _FFP.fx TBoringFFPProjShad (Diffuse1 x AlphaGenValue, _MT Diffuse2);
// _vsPost.fx vscPostBlackT1; _vsSkPost.fx vscSkinPostBlackT1; the 15 shipped
// PROJSHAD declarations]
enum class ObjectProjectedShadowCoverage : uint8_t {
	NoPass,
	DiffuseAlpha,
	DiffuseAlphaFfp,
	DiffuseDetailAlphaFfp,
};

ObjectProjectedShadowCoverage object_projected_shadow_coverage(
		ObjectShaderTechnique technique) noexcept;
// The manifest token of a coverage source ("no_pass", "diffuse_alpha",
// "diffuse_alpha_ffp", "diffuse_detail_alpha_ffp"): the object pipeline
// manifest's projected_shadow_contracts names the same table per technique.
const char *object_projected_shadow_coverage_name(
		ObjectProjectedShadowCoverage coverage) noexcept;

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
	bool is_skinned = false;
	bool environment_textured = false;
	bool glass = false;
	bool view_angle_fade = false;
};

// Pack a classification into the stable 32-bit material/cache key.
ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls);

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key);
ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key);

// Describe the complete normal-pass pipeline selected by a key. Pure data;
// bindings decide how each policy maps to their renderer.
ObjectShaderPipelineDescriptor describe_object_shader_pipeline(ObjectShaderKey key);

}  // namespace opennova::renderer