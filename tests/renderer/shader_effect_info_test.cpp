// The tags a shader file registers (runtime/renderer/shader_effect_info.h): an effect's EffectInfo
// annotations read with its comments left out (EffectTag, where it is written, EffectAlt_UV) [orig:
// HLSLEffect_LoadFromFile @ 0x5AE899..0x5AE9BC]; none for a text with no EffectInfo; the fixed-function
// effect's compiles, the effect registry's FF_ rows, which are the renderer's own names for them
// ("FF%s%s%s" over the texture, blend and self-lit suffixes in its compile order [orig:
// HLSLEffect_InitFixedFunctionShaders @ 0x5AFA54..0x5AFCFC, sprintf @ 0x5AFAD6]), each with its #UV twin
// in the table; an include's name.
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/shader_effect_info.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::renderer;

namespace {

int test_effect_info() {
	const std::string source =
			"// string EffectTag = \"COMMENTED\";\r\n/* string EffectInfo < string EffectTag = \"BLOCK\"; > */\r\n"
			"string EffectInfo <\r\n\tstring EffectName = \"x > y\";\r\n\tstring EffectTag = \"VS_TEST\";\r\n"
			"\tbool EffectAlt_UV = true;\r\n>;\r\n";
	const ShaderEffectInfo info = read_shader_effect_info(source);
	TEST_EXPECT(info.found && info.tag == "VS_TEST" && info.alt_uv &&
	            source.substr(info.tag_offset, info.tag_length) == "VS_TEST" &&
	            source.compare(info.info_offset, 10, "EffectInfo") == 0 && info.info_offset == source.rfind("EffectInfo <"));
	const ShaderEffectInfo one =
			read_shader_effect_info("string EffectInfo < string EffectTag = \"VS_ONE\"; bool EffectAlt_UV = false; >;");
	TEST_EXPECT(one.found && one.tag == "VS_ONE" && !one.alt_uv);
	TEST_EXPECT(read_shader_effect_info("string EffectInfo < bool EffectAlt_UV = 1; >;").alt_uv);
	TEST_EXPECT(!read_shader_effect_info("float4 main() : COLOR { return 0; }").found);
	std::printf("effect info: the tag past the comments, its place, the #UV twin asked or not\n");
	return 0;
}

int test_fixed_function_tags() {
	// The renderer's own names, built as it builds them, are the table's FF_ rows in its order.
	std::vector<std::string> built;
	for (const char *texture : {"_ST", "_MT"})
		for (const char *lum : {"", "_LUM"})
			for (const char *blend : {"_OP", "_AB", "_AD"})
				built.push_back(std::string("FF") + texture + blend + lum);
	TEST_EXPECT(fixed_function_shader_tags() == built && built.size() == 12);
	for (const std::string &tag : built) {
		const MaterialDescriptorRecord *row = find_material_descriptor(tag);
		const MaterialDescriptorRecord *twin = find_material_descriptor(tag + "#UV");
		TEST_EXPECT(row && row->family == MaterialDescriptorFamily::FixedFunction && twin &&
		            (twin->descriptor_flags & MATERIAL_DESCRIPTOR_UV_TRANSFORM) != 0);
	}
	TEST_EXPECT(std::string(kFixedFunctionShaderFile) == "_ffp.fx");
	TEST_EXPECT(shader_file_is_include("_vsinc.fx") && shader_file_is_include(kFixedFunctionShaderFile) &&
	            !shader_file_is_include("glass.fx") && !shader_file_is_include(""));
	std::printf("fixed function: the twelve compiles, each with its #UV twin; an include's '_'\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_effect_info();
	failures += test_fixed_function_tags();
	if (failures == 0) std::printf("renderer_shader_effect_info: all passed\n");
	return failures == 0 ? 0 : 1;
}
