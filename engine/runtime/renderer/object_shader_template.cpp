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

// [orig: Render_CreateSystemTextures @0x58aca0 — the 256x256 gsys_phong lookup: N.L in X/alpha,
//  N.H in Y, RGB = trunc(255 * x^4 / x^16 / x^64) with the binary32 1/255 at 0x7D75E8]
ObjectPhongMapTexel object_phong_map_texel(uint8_t ndotl, uint8_t ndoth) {
	// This is the binary32 value at 0x7D75E8. Retail keeps the products on
	// x87 while exponentiating by squaring, then _ftol2_sse truncates each
	// positive channel toward zero.
	constexpr float kInv255 = 0.0039215688593685627f; // 0x3b808081
	const double x = static_cast<double>(static_cast<float>(ndoth) * kInv255);
	const double p2 = x * x;
	const double p4 = p2 * p2;
	const double p8 = p4 * p4;
	const double p16 = p8 * p8;
	const double p32 = p16 * p16;
	const double p64 = p32 * p32;
	return {
		static_cast<uint8_t>(p4 * 255.0),
		static_cast<uint8_t>(p16 * 255.0),
		static_cast<uint8_t>(p64 * 255.0),
		ndotl,
	};
}

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
	if (cls.is_skinned) key |= OSCAP_SKINNED;
	if (cls.environment_textured) key |= OSCAP_ENV_TEXTURED;
	return key;
}

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key) {
	return static_cast<ObjectShaderFamily>(
		(key & OSCAP_FAMILY_MASK) >> OSCAP_FAMILY_SHIFT);
}

ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key) {
	return static_cast<ObjectBlendMode>(key & OSCAP_BLEND_MASK);
}

// Per-technique PROJSHAD pass presence, as the .fx technique-block probe at load finds it
// [orig: HLSLEffect_LoadFromFile @0x5ae690 technique loop; the class-per-batch selection
//  CRenderBatchQueue_FlushBatches @0x5d9ff3; docs/render/render-material-re.md]
ObjectProjectedShadowPolicy object_projected_shadow_policy(
		ObjectShaderTechnique technique) noexcept {
	switch (technique) {
		case ObjectShaderTechnique::Fixed:
		case ObjectShaderTechnique::FixedDetail:
		case ObjectShaderTechnique::SelfLit:
		case ObjectShaderTechnique::SelfLitDetail:
			return ObjectProjectedShadowPolicy::MaterialBlend;

		case ObjectShaderTechnique::FixedSkinned:
		case ObjectShaderTechnique::PhongTangentDiffuse:
		case ObjectShaderTechnique::PhongTangentSpecular:
		case ObjectShaderTechnique::PhongTangentSpecularSkinned:
		case ObjectShaderTechnique::PhongObjectDiffuse:
		case ObjectShaderTechnique::PhongObjectSpecular:
		case ObjectShaderTechnique::PhongObjectSpecularPhongMap:
		case ObjectShaderTechnique::Dot3Tangent:
		case ObjectShaderTechnique::Dot3TangentDetail:
		case ObjectShaderTechnique::Dot3TangentSkinned:
		case ObjectShaderTechnique::Dot3TangentDetailSkinned:
		case ObjectShaderTechnique::Dot3Object:
		case ObjectShaderTechnique::Dot3ObjectDetail:
		case ObjectShaderTechnique::EnvironmentMirror:
		case ObjectShaderTechnique::EnvironmentMirrorTextured:
		case ObjectShaderTechnique::EnvironmentPhong:
			return ObjectProjectedShadowPolicy::Opaque;

		case ObjectShaderTechnique::Unsupported:
		case ObjectShaderTechnique::Tracer:
		case ObjectShaderTechnique::Flag:
		case ObjectShaderTechnique::GlassFixed:
		case ObjectShaderTechnique::GlassSkinned:
			return ObjectProjectedShadowPolicy::NoPass;
	}
	return ObjectProjectedShadowPolicy::NoPass;
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
	descriptor.is_skinned = has_flag(key, OSCAP_SKINNED);
	descriptor.environment_textured = has_flag(key, OSCAP_ENV_TEXTURED);
	descriptor.glass = has_flag(key, OSCAP_GLASS);
	descriptor.view_angle_fade = has_flag(key, OSCAP_VIEW_FADE);

	descriptor.cull = descriptor.two_sided
		? ObjectCullPolicy::Disabled
		: ObjectCullPolicy::Back;
	// Retail alpha testing changes coverage only. Blended NORMAL techniques
	// remain ZMODE_NOWRITE; opaque clipped techniques write the surviving
	// fragments in their ordinary pass. [orig: _FFP.fx technique tables;
	// CGfxDevice_SetAlphaTestRef @ 0x6770a0].
	if (descriptor.blend == ObjectBlendMode::Opaque) {
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
	if (descriptor.family == ObjectShaderFamily::Environment &&
			!descriptor.uses_specular) {
		descriptor.specular_source = ObjectSpecularSource::AnalyticPow16;
	} else if (descriptor.uses_specular && descriptor.is_skinned &&
			descriptor.normal_space == ObjectNormalSpace::Object) {
			descriptor.specular_source =
					ObjectSpecularSource::PhongMapLookupDiffuseAlpha;
	} else if (descriptor.uses_specular) {
		descriptor.specular_source =
				ObjectSpecularSource::AnalyticPow8DiffuseAlpha;
	}

	// The MTRL emissive byte is an invariant witness for *_LUM tags, not an
	// independent way to turn a file effect into a SELFLUM effect. 3DI loading
	// asserts that emissive_type 2 already has the _LUM suffix; file effects
	// such as Flag.fx and SkBasic.fx carry no SELFLUM technique.
	const bool self_lit = descriptor.luminance;
	if (descriptor.view_angle_fade) {
		descriptor.technique = ObjectShaderTechnique::Tracer;
	} else if (descriptor.family == ObjectShaderFamily::Flag) {
		if (!descriptor.uses_normal_map && !descriptor.uses_detail) {
			// Flag.fx's only NORMAL technique always runs vsFlag lighting; it
			// has no SELFLUM topology.
			descriptor.technique = ObjectShaderTechnique::Flag;
		}
	} else if (self_lit && (descriptor.family == ObjectShaderFamily::Unknown ||
			descriptor.family == ObjectShaderFamily::FixedFunction)) {
		// Normal/specular/environment stages do not contribute to SELFLUM;
		// the detail texture remains part of its base-color combine.
		descriptor.technique = descriptor.uses_detail
			? ObjectShaderTechnique::SelfLitDetail
			: ObjectShaderTechnique::SelfLit;
	} else if (descriptor.family == ObjectShaderFamily::Unknown ||
			descriptor.family == ObjectShaderFamily::FixedFunction) {
		if (!descriptor.uses_normal_map) {
			if (descriptor.is_skinned) {
				descriptor.technique = ObjectShaderTechnique::FixedSkinned;
			} else {
				descriptor.technique = descriptor.uses_detail
					? ObjectShaderTechnique::FixedDetail
					: ObjectShaderTechnique::Fixed;
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Phong) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2 &&
				!descriptor.uses_detail) {
			if (descriptor.normal_space == ObjectNormalSpace::Object) {
				if (descriptor.uses_specular) {
					descriptor.technique = descriptor.is_skinned
							? ObjectShaderTechnique::PhongObjectSpecularPhongMap
							: ObjectShaderTechnique::PhongObjectSpecular;
				} else {
					descriptor.technique = ObjectShaderTechnique::PhongObjectDiffuse;
				}
			} else if (descriptor.normal_space == ObjectNormalSpace::Tangent) {
				if (descriptor.uses_specular) {
					descriptor.technique = descriptor.is_skinned
							? ObjectShaderTechnique::PhongTangentSpecularSkinned
							: ObjectShaderTechnique::PhongTangentSpecular;
				} else {
					descriptor.technique = ObjectShaderTechnique::PhongTangentDiffuse;
				}
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Dot3) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2) {
			if (descriptor.normal_space == ObjectNormalSpace::Object) {
				descriptor.technique = descriptor.uses_detail
					? ObjectShaderTechnique::Dot3ObjectDetail
					: ObjectShaderTechnique::Dot3Object;
			} else if (descriptor.normal_space == ObjectNormalSpace::Tangent) {
				if (descriptor.is_skinned) {
					descriptor.technique = descriptor.uses_detail
						? ObjectShaderTechnique::Dot3TangentDetailSkinned
						: ObjectShaderTechnique::Dot3TangentSkinned;
				} else {
					descriptor.technique = descriptor.uses_detail
						? ObjectShaderTechnique::Dot3TangentDetail
						: ObjectShaderTechnique::Dot3Tangent;
				}
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Environment) {
		if (descriptor.uses_normal_map && !descriptor.normal_uses_uv2 &&
				descriptor.normal_space == ObjectNormalSpace::Tangent &&
				!descriptor.uses_detail) {
			if (descriptor.uses_specular) {
				descriptor.technique = ObjectShaderTechnique::EnvironmentPhong;
			} else {
				descriptor.technique = descriptor.environment_textured
						? ObjectShaderTechnique::EnvironmentMirrorTextured
						: ObjectShaderTechnique::EnvironmentMirror;
			}
		}
	} else if (descriptor.family == ObjectShaderFamily::Glass) {
		if (!descriptor.uses_normal_map && !descriptor.uses_detail) {
			descriptor.technique = descriptor.is_skinned
					? ObjectShaderTechnique::GlassSkinned
					: ObjectShaderTechnique::GlassFixed;
		}
	}
	return descriptor;
}

} // namespace renderer
