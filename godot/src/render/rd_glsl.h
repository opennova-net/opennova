#pragma once

// GLSL text the RenderingDevice render legs share: engine constants spliced
// into a shader template's @TOKEN@ slots, so the GLSL never restates them.

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

} // namespace godot
