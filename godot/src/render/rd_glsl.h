#pragma once

// GLSL text the RenderingDevice render legs share: engine constants spliced
// into a shader template's @TOKEN@ slots, so the GLSL never restates them,
// the shared GLSL snippets, and the one vertex + fragment SPIR-V compile.

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rendering_device.hpp>

#include "util/string_convert.h"

#include <array>
#include <cstdio>
#include <string>

namespace godot {

// A GLSL float literal for an engine constant: %.9g round-trips every float,
// and a trailing ".0" keeps integral values typed as floats.
inline std::string glsl_float(float p_value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(p_value));
	std::string text(buffer);
	if (text.find_first_of(".eE") == std::string::npos)
		text += ".0";
	return text;
}

inline std::string glsl_vec3(const std::array<float, 3> &p_value) {
	return "vec3(" + glsl_float(p_value[0]) + ", " + glsl_float(p_value[1]) + ", " +
			glsl_float(p_value[2]) + ")";
}

inline void splice_token(std::string &p_text, const char *p_token, const std::string &p_value) {
	const std::string token(p_token);
	for (std::size_t at = p_text.find(token); at != std::string::npos;
			at = p_text.find(token, at + p_value.size()))
		p_text.replace(at, token.size(), p_value);
}

// The object stages' sampLinearWrap2D at retail's highest filter tier: the 2x
// anisotropic footprint of the beauty wrappers (shared.gdshaderinc
// obj_sample_aniso2), clamped at the stage texture's last retail mip level.
// A fragment template splices it into its @SAMPLE_ANISO2@ slot and calls
// rd_sample_aniso2(texture, uv, max_lod).
inline const char *const kGlslSampleAniso2 = R"GLSL(vec4 rd_sample_aniso2(sampler2D tex, vec2 tex_uv, float max_lod) {
	vec2 size = vec2(textureSize(tex, 0));
	vec2 du = dFdx(tex_uv);
	vec2 dv = dFdy(tex_uv);
	float length_x = length(du * size);
	float length_y = length(dv * size);
	float major = max(length_x, length_y);
	float minor = min(length_x, length_y);
	float ratio = clamp(major / max(minor, 1.0e-8), 1.0, 2.0);
	float lod = min(log2(max(major / ratio, 1.0e-8)), max_lod);
	vec2 offset = (length_x >= length_y ? du : dv) * (0.5 - 0.5 / ratio);
	return 0.5 * (textureLod(tex, tex_uv - offset, lod) +
			textureLod(tex, tex_uv + offset, lod));
})GLSL";

// Compiles one vertex + fragment GLSL pair to SPIR-V on p_rd. Returns an
// empty string when both stages produced bytecode (r_spirv holds it);
// otherwise the failure text: "no SPIR-V" when the compiler returned nothing,
// else "vertex=<error>; fragment=<error>". The caller creates the shader from
// r_spirv (shader_create_from_spirv) and reports a rejection itself.
inline std::string compile_rd_spirv(RenderingDevice *p_rd, const std::string &p_vertex_glsl,
		const std::string &p_fragment_glsl, Ref<RDShaderSPIRV> &r_spirv) {
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX,
			opennova::to_gd(p_vertex_glsl));
	source->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT,
			opennova::to_gd(p_fragment_glsl));
	r_spirv = p_rd->shader_compile_spirv_from_source(source);
	if (r_spirv.is_null())
		return "no SPIR-V";
	const String vertex_error =
			r_spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX);
	const String fragment_error =
			r_spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (vertex_error.is_empty() && fragment_error.is_empty() &&
			!r_spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX).is_empty() &&
			!r_spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty())
		return std::string();
	return "vertex=" + opennova::to_std(vertex_error) + "; fragment=" +
			opennova::to_std(fragment_error);
}

} // namespace godot
