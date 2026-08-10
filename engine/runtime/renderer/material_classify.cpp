#include "renderer/material_classify.h"

#include "oed/material_descriptor.h"
#include "threedi/threedi_3di3.h"

namespace renderer {

namespace {

ObjectShaderFamily map_family(::oed::MaterialDescriptorFamily family) {
	switch (family) {
		case ::oed::MaterialDescriptorFamily::Unknown:
			return ObjectShaderFamily::Unknown;
		case ::oed::MaterialDescriptorFamily::FixedFunction:
			return ObjectShaderFamily::FixedFunction;
		case ::oed::MaterialDescriptorFamily::Phong:
			return ObjectShaderFamily::Phong;
		case ::oed::MaterialDescriptorFamily::Flag:
			return ObjectShaderFamily::Flag;
		case ::oed::MaterialDescriptorFamily::Dot3:
			return ObjectShaderFamily::Dot3;
		case ::oed::MaterialDescriptorFamily::Environment:
			return ObjectShaderFamily::Environment;
		case ::oed::MaterialDescriptorFamily::Glass:
			return ObjectShaderFamily::Glass;
	}
	return ObjectShaderFamily::Unknown;
}

ObjectBlendMode map_blend(::oed::MaterialDescriptorBlend blend) {
	switch (blend) {
		case ::oed::MaterialDescriptorBlend::Opaque:
			return ObjectBlendMode::Opaque;
		case ::oed::MaterialDescriptorBlend::AlphaBlend:
			return ObjectBlendMode::AlphaBlend;
		case ::oed::MaterialDescriptorBlend::Additive:
			return ObjectBlendMode::Additive;
		case ::oed::MaterialDescriptorBlend::Multiplicative:
			return ObjectBlendMode::Multiplicative;
	}
	return ObjectBlendMode::Opaque;
}

ObjectNormalSpace map_normal_space(::oed::MaterialDescriptorNormalSpace normal_space) {
	switch (normal_space) {
		case ::oed::MaterialDescriptorNormalSpace::None:
			return ObjectNormalSpace::None;
		case ::oed::MaterialDescriptorNormalSpace::Tangent:
			return ObjectNormalSpace::Tangent;
		case ::oed::MaterialDescriptorNormalSpace::Object:
			return ObjectNormalSpace::Object;
	}
	return ObjectNormalSpace::None;
}

} // namespace

const char *object_shader_family_name(ObjectShaderFamily family) {
	switch (family) {
		case ObjectShaderFamily::Unknown: return "unknown";
		case ObjectShaderFamily::FixedFunction: return "fixed_function";
		case ObjectShaderFamily::Phong: return "phong";
		case ObjectShaderFamily::Flag: return "flag";
		case ObjectShaderFamily::Dot3: return "dot3";
		case ObjectShaderFamily::Environment: return "environment";
		case ObjectShaderFamily::Glass: return "glass";
	}
	return "unknown";
}

ObjectMaterialClassification classify_object_material(const std::string &shader_name,
                                                       uint8_t material_flags,
                                                       uint8_t emissive_type,
                                                       uint8_t is_glass_flag,
                                                       uint8_t alpha_test_value_byte) {
	ObjectMaterialClassification c;

	c.is_two_sided = (material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0;
	c.alpha_test = (material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0;
	c.alpha_test_invert = (material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0;
	c.alpha_test_value = static_cast<float>(alpha_test_value_byte) / 255.0f;
	c.is_emissive = emissive_type == THREEDI_EMISSIVE_FULL;
	c.is_glass = is_glass_flag != 0;

	const ::oed::MaterialDescriptorRecord *descriptor =
		::oed::find_material_descriptor(shader_name);
	if (descriptor == nullptr)
		return c;

	const uint32_t info_flags = static_cast<uint32_t>(descriptor->shader_flags);
	const uint32_t descriptor_flags = descriptor->descriptor_flags;

	c.known_shader = true;
	c.family = map_family(descriptor->family);
	c.blend = map_blend(descriptor->blend);
	// The self-lum LOOK is the EMISSIVE bit (SELFLUM material colors, authored
	// 0x1 on the FF _LUM rows [orig: HLSLEffect_InitFixedFunctionShaders
	// @ 0x5af790; _FFP.fx SELFLUM block]); 0x10000000 is the separate
	// glow-copy CAPABILITY (is_glow_capable below) — the two ride together on
	// LUM rows but FFP_GLASS carries only the capability (D-RMAT-4).
	c.is_luminance = (info_flags & ::oed::MATERIAL_FLAG_EMISSIVE) != 0;
	c.is_glow_capable = (info_flags & ::oed::MATERIAL_FLAG_GLOW) != 0;
	c.view_angle_fade = (descriptor_flags & ::oed::MATERIAL_DESCRIPTOR_VIEW_FADE) != 0;
	c.needs_normal_map = (info_flags & (::oed::MATERIAL_FLAG_NORMAL_A |
	                                     ::oed::MATERIAL_FLAG_NORMAL_B)) != 0;
	c.is_glass = c.is_glass || (info_flags & ::oed::MATERIAL_FLAG_GLASS) != 0;
	c.uses_environment = (descriptor_flags & ::oed::MATERIAL_DESCRIPTOR_ENVIRONMENT) != 0;
	c.has_detail = (info_flags & ::oed::MATERIAL_FLAG_SECONDARY) != 0;
	c.is_skinned = (descriptor_flags & ::oed::MATERIAL_DESCRIPTOR_SKINNED) != 0;
	c.uses_specular = (descriptor_flags & ::oed::MATERIAL_DESCRIPTOR_SPECULAR) != 0;
	c.normal_uses_uv2 = (info_flags & ::oed::MATERIAL_FLAG_NORMAL_B) != 0;
	c.normal_space = c.needs_normal_map ? map_normal_space(descriptor->normal_space)
	                                    : ObjectNormalSpace::None;

	return c;
}

} // namespace renderer
