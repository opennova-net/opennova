#include "renderer/object_shader_template.h"

namespace renderer {

namespace {

uint32_t encode_family(ObjectShaderFamily family) {
	return static_cast<uint32_t>(family) << OSCAP_FAMILY_SHIFT;
}

bool has_flag(ObjectShaderKey key, uint32_t bit) {
	return (key & bit) != 0;
}

} // namespace

ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls) {
	ObjectShaderKey key = 0;
	switch (cls.blend) {
		case ObjectBlendMode::Opaque: key |= 0; break;
		case ObjectBlendMode::AlphaBlend: key |= 1; break;
		case ObjectBlendMode::Additive: key |= 2; break;
		case ObjectBlendMode::Multiplicative: key |= 3; break;
	}
	key |= encode_family(cls.family);
	if (cls.is_two_sided) key |= OSCAP_TWO_SIDED;
	if (cls.alpha_test) key |= OSCAP_ALPHA_TEST;
	if (cls.alpha_test_invert) key |= OSCAP_ALPHA_INVERT;
	if (cls.is_emissive) key |= OSCAP_EMISSIVE;
	if (cls.is_luminance) key |= OSCAP_LUMINANCE;
	if (cls.needs_normal_map) key |= OSCAP_NORMAL_MAP;
	if (cls.normal_uses_uv2) key |= OSCAP_NORMAL_UV2;
	if (cls.normal_space == ObjectNormalSpace::Object) key |= OSCAP_OBJECT_SPACE;
	if (cls.has_detail) key |= OSCAP_DETAIL;
	if (cls.uses_specular) key |= OSCAP_SPECULAR;
	if (cls.is_glass) key |= OSCAP_GLASS;
	if (cls.view_angle_fade) key |= OSCAP_VIEW_FADE;
	return key;
}

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key) {
	return static_cast<ObjectShaderFamily>(
		(key & OSCAP_FAMILY_MASK) >> OSCAP_FAMILY_SHIFT);
}

ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key) {
	return static_cast<ObjectBlendMode>(key & OSCAP_BLEND_MASK);
}

// The finite technique choices preserve the witnessed retail material families
// and pass policies; only their Godot resource realization lives in the
// adapter. [orig: HLSLEffect_LoadFromFile @ 0x5af417..0x5af49e; _FFP.fx
// TBoringFFP / TECHNIQUE_NORMAL / SELFLUM variants; alpha-test state at
// CRenderBatchQueue_FlushBatches @ 0x5da3a9..0x5da401; environment-cube
// refresh at update_environment_cubemap @ 0x6106a0].
ObjectShaderPipelineDescriptor describe_object_shader_pipeline(ObjectShaderKey key) {
	ObjectShaderPipelineDescriptor descriptor;
	descriptor.key = key;
	descriptor.family = decode_object_shader_family(key);
	descriptor.blend = decode_object_shader_blend(key);
	descriptor.alpha_test = has_flag(key, OSCAP_ALPHA_TEST);
	descriptor.alpha_test_invert = has_flag(key, OSCAP_ALPHA_INVERT);
	descriptor.two_sided = has_flag(key, OSCAP_TWO_SIDED);
	descriptor.emissive = has_flag(key, OSCAP_EMISSIVE);
	descriptor.luminance = has_flag(key, OSCAP_LUMINANCE);
	descriptor.uses_normal_map = has_flag(key, OSCAP_NORMAL_MAP);
	descriptor.normal_uses_uv2 = has_flag(key, OSCAP_NORMAL_UV2);
	descriptor.uses_detail = has_flag(key, OSCAP_DETAIL);
	descriptor.uses_specular = has_flag(key, OSCAP_SPECULAR);
	descriptor.glass = has_flag(key, OSCAP_GLASS);
	descriptor.view_angle_fade = has_flag(key, OSCAP_VIEW_FADE);

	descriptor.cull = descriptor.two_sided
		? ObjectCullPolicy::Disabled
		: ObjectCullPolicy::Back;
	if (descriptor.alpha_test) {
		descriptor.depth = ObjectDepthPolicy::AlphaPrepass;
	} else if (descriptor.blend == ObjectBlendMode::Opaque) {
		descriptor.depth = ObjectDepthPolicy::Opaque;
	} else {
		descriptor.depth = ObjectDepthPolicy::TransparentNoWrite;
	}
	// Only the AlphaBlend technique writes ALPHA. Touching ALPHA in any other
	// Godot shader moves the surface to a transparent pass.
	descriptor.writes_alpha = descriptor.blend == ObjectBlendMode::AlphaBlend;

	if (descriptor.uses_normal_map) {
		descriptor.normal_space = has_flag(key, OSCAP_OBJECT_SPACE)
			? ObjectNormalSpace::Object
			: ObjectNormalSpace::Tangent;
	}
	if (descriptor.family == ObjectShaderFamily::Glass ||
		descriptor.family == ObjectShaderFamily::Environment) {
		descriptor.environment_source =
			ObjectEnvironmentSource::HemisphereApproximation;
	}
	if (descriptor.uses_specular) {
		descriptor.specular_source = ObjectSpecularSource::AnalyticPow16;
	}

	const bool self_lit = descriptor.emissive || descriptor.luminance;
	if (descriptor.view_angle_fade) {
		descriptor.technique = ObjectShaderTechnique::Tracer;
	} else if (descriptor.family == ObjectShaderFamily::Flag) {
		if (!descriptor.uses_normal_map && !descriptor.uses_detail) {
			descriptor.technique = self_lit
				? ObjectShaderTechnique::FlagSelfLit
				: ObjectShaderTechnique::Flag;
		}
	} else if (self_lit) {
		// Normal/specular/environment stages do not contribute to SELFLUM;
		// the detail texture remains part of its base-color combine.
		descriptor.technique = descriptor.uses_detail
			? ObjectShaderTechnique::SelfLitDetail
			: ObjectShaderTechnique::SelfLit;
	} else if (descriptor.family == ObjectShaderFamily::Unknown ||
			descriptor.family == ObjectShaderFamily::FixedFunction) {
		if (!descriptor.uses_normal_map) {
			descriptor.technique = descriptor.uses_detail
				? ObjectShaderTechnique::FixedDetail
				: ObjectShaderTechnique::Fixed;
		}
	} else if (descriptor.family == ObjectShaderFamily::Phong) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2 &&
				!descriptor.uses_detail) {
			if (descriptor.normal_space == ObjectNormalSpace::Object) {
				descriptor.technique = descriptor.uses_specular
					? ObjectShaderTechnique::PhongObjectSpecular
					: ObjectShaderTechnique::PhongObjectDiffuse;
			} else if (descriptor.normal_space == ObjectNormalSpace::Tangent) {
				descriptor.technique = descriptor.uses_specular
					? ObjectShaderTechnique::PhongTangentSpecular
					: ObjectShaderTechnique::PhongTangentDiffuse;
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Dot3) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2) {
			if (descriptor.normal_space == ObjectNormalSpace::Object) {
				descriptor.technique = descriptor.uses_detail
					? ObjectShaderTechnique::Dot3ObjectDetail
					: ObjectShaderTechnique::Dot3Object;
			} else if (descriptor.normal_space == ObjectNormalSpace::Tangent) {
				descriptor.technique = descriptor.uses_detail
					? ObjectShaderTechnique::Dot3TangentDetail
					: ObjectShaderTechnique::Dot3Tangent;
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Environment) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2 &&
				descriptor.normal_space == ObjectNormalSpace::Tangent &&
				!descriptor.uses_detail) {
			descriptor.technique = descriptor.uses_specular
				? ObjectShaderTechnique::EnvironmentTangentSpecular
				: ObjectShaderTechnique::EnvironmentTangent;
		}
	} else if (descriptor.family == ObjectShaderFamily::Glass) {
		if (!descriptor.uses_normal_map && !descriptor.uses_detail) {
			descriptor.technique = ObjectShaderTechnique::Glass;
		}
	}
	return descriptor;
}

} // namespace renderer
