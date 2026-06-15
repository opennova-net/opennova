#include "renderer/object_shader_template.h"

namespace renderer {

namespace {

uint32_t encode_family(ObjectShaderFamily family) {
	return static_cast<uint32_t>(family) << OSCAP_FAMILY_SHIFT;
}

bool has_flag(ObjectShaderKey key, uint32_t bit) {
	return (key & bit) != 0;
}

// shader_type + render_mode. Blend/depth/cull are the only render-state bits
// Godot needs declared up front; the per-family shading lives in the included
// body (godot/shaders/object/object.gdshaderinc). All object materials are
// `unshaded` — the original rolls its own hemisphere + directional + point
// lighting in the effect, it never uses fixed-function D3D *scene* lighting the
// way Godot's lit path would (docs/renderer/renderer-re.md).
std::string compose_render_mode(ObjectShaderKey key) {
	const bool alpha_test = has_flag(key, OSCAP_ALPHA_TEST);
	const uint32_t blend = static_cast<uint32_t>(decode_object_shader_blend(key));

	std::string rm = "shader_type spatial;\nrender_mode unshaded, ";
	switch (blend) {
		case 2: rm += "blend_add"; break;
		case 3: rm += "blend_mul"; break;
		case 1:
		default: rm += "blend_mix"; break;
	}

	if (alpha_test) {
		rm += ", depth_prepass_alpha, depth_draw_opaque";
	} else if (blend != 0) {
		rm += ", depth_draw_never";
	} else {
		rm += ", depth_draw_opaque";
	}
	rm += has_flag(key, OSCAP_TWO_SIDED) ? ", cull_disabled" : ", cull_back";
	rm += ";\n\n";
	return rm;
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
	return key;
}

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key) {
	return static_cast<ObjectShaderFamily>(
		(key & OSCAP_FAMILY_MASK) >> OSCAP_FAMILY_SHIFT);
}

ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key) {
	return static_cast<ObjectBlendMode>(key & OSCAP_BLEND_MASK);
}

// Translate the capability key into the `#define`s that the macro über-shader
// body switches on. This mirrors the original's own structure: the effect set
// is one shared header (`_BaseInc.fx`) plus per-family bodies, multiplied out by
// preprocessor macros (`BLEND_*`, `TEX_*`, `SELFLUM`, `TEX_UVXFORM`). We carry
// the same idea: one `_object_baseinc.gdshaderinc` + one `object.gdshaderinc`
// body specialised per key by these defines (docs/renderer/renderer-re.md).
std::string compose_object_shader_defines(ObjectShaderKey key) {
	std::string d;

	switch (decode_object_shader_family(key)) {
		case ObjectShaderFamily::FixedFunction: d += "#define OBJ_FAMILY_FIXEDFUNCTION\n"; break;
		case ObjectShaderFamily::Phong:         d += "#define OBJ_FAMILY_PHONG\n"; break;
		case ObjectShaderFamily::Flag:          d += "#define OBJ_FAMILY_FLAG\n"; break;
		case ObjectShaderFamily::Dot3:          d += "#define OBJ_FAMILY_DOT3\n"; break;
		case ObjectShaderFamily::Environment:   d += "#define OBJ_FAMILY_ENVIRONMENT\n"; break;
		case ObjectShaderFamily::Glass:         d += "#define OBJ_FAMILY_GLASS\n"; break;
		case ObjectShaderFamily::Unknown:
		default:                                d += "#define OBJ_FAMILY_FIXEDFUNCTION\n"; break;
	}

	switch (decode_object_shader_blend(key)) {
		case ObjectBlendMode::AlphaBlend:     d += "#define OBJ_BLEND_ALPHABLEND\n"; break;
		case ObjectBlendMode::Additive:       d += "#define OBJ_BLEND_ADDITIVE\n"; break;
		case ObjectBlendMode::Multiplicative: d += "#define OBJ_BLEND_MULTIPLICATIVE\n"; break;
		case ObjectBlendMode::Opaque:
		default:                              d += "#define OBJ_BLEND_OPAQUE\n"; break;
	}

	if (has_flag(key, OSCAP_DETAIL))       d += "#define OBJ_DETAIL\n";
	if (has_flag(key, OSCAP_NORMAL_MAP))   d += "#define OBJ_NORMAL_MAP\n";
	if (has_flag(key, OSCAP_NORMAL_UV2))   d += "#define OBJ_NORMAL_UV2\n";
	if (has_flag(key, OSCAP_OBJECT_SPACE)) d += "#define OBJ_OBJECT_SPACE\n";
	if (has_flag(key, OSCAP_ALPHA_TEST))   d += "#define OBJ_ALPHA_TEST\n";
	if (has_flag(key, OSCAP_ALPHA_INVERT)) d += "#define OBJ_ALPHA_INVERT\n";
	if (has_flag(key, OSCAP_SPECULAR))     d += "#define OBJ_SPECULAR\n";
	if (has_flag(key, OSCAP_EMISSIVE))     d += "#define OBJ_EMISSIVE\n";
	if (has_flag(key, OSCAP_LUMINANCE))    d += "#define OBJ_LUMINANCE\n";
	if (has_flag(key, OSCAP_GLASS))        d += "#define OBJ_GLASS\n";
	if (has_flag(key, OSCAP_TWO_SIDED))    d += "#define OBJ_TWO_SIDED\n";

	return d;
}

std::string compose_object_shader_prelude(ObjectShaderKey key) {
	return compose_render_mode(key) + compose_object_shader_defines(key);
}

} // namespace renderer
