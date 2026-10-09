#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::renderer {

// The shader tags a shader file (.fx) registers with the renderer's effect registry, as its loader reads
// them: the fixed-function effect's compiles, and another effect's EffectInfo annotations.

// The fixed-function effect, which the renderer opens by this name as it starts and compiles once for each
// of its fixed-function tags [orig: HLSLEffect_InitFixedFunctionShaders @ 0x5AF790, the name @ 0x5AFA3E].
inline constexpr const char *kFixedFunctionShaderFile = "_ffp.fx";

// Those tags, as the renderer names each compile: "FF", then _ST or _MT (one texture or two), _OP, _AB or
// _AD (opaque, alpha-blended, additive), then _LUM for the self-lit [orig: @ 0x5AFAD6, sprintf "FF%s%s%s"],
// in the order the renderer compiles them, texture by self-lit by blending [orig:
// HLSLEffect_InitFixedFunctionShaders @ 0x5AFA54..0x5AFCFC; the suffixes @ 0x5AF8D6..0x5AF91E]: the FF_
// rows of the effect registry's table (material_descriptor.h kMaterialDescriptorTable), their #UV twins
// left out.
const std::vector<std::string> &fixed_function_shader_tags();

// Whether the archive walk that loads every shader passes over a file of this name: one whose name starts
// with '_', an include [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF6E].
bool shader_file_is_include(const std::string &name);

// What a shader's EffectInfo annotations say [orig: HLSLEffect_LoadFromFile @ 0x5AE899..0x5AE9BC]: the tag
// the effect registers under (EffectTag) and where it is written (its offset into the text and length, 0
// for none), and whether the loader registers a TEX_UVXFORM twin of it as the tag and "#UV"
// (EffectAlt_UV) [orig: @ 0x5AEA03; HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF88]. `found` is false for a
// text with no EffectInfo. Read from the text with its comments left out, as the compiler reads it; a
// preprocessor condition around the annotations is not followed.
struct ShaderEffectInfo {
	bool found = false;
	size_t info_offset = 0; // where EffectInfo is written
	std::string tag;
	size_t tag_offset = 0, tag_length = 0;
	bool alt_uv = false;
};
ShaderEffectInfo read_shader_effect_info(const std::string &text);

} // namespace opennova::renderer
