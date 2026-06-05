// Unit tests for renderer::classify_object_material + the GLSL composer in
// libs/renderer.  Verifies the static shader_tag -> family/blend table without
// `.fx` parsing matches the canonical's runtime classifications for every
// shader_tag in the original OED's gMaterialInfoTable.

#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"
#include "oed/material_descriptor.h"
#include "threedi/threedi_3di3.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(1);
	}
}

bool contains(const std::string &haystack, const char *needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
	using namespace renderer;

	// 0. The renderer descriptor table is exact and complete for OED's table.
	{
		expect(oed::kMaterialDescriptorTableCount == oed::kMaterialInfoTableCount,
		       "descriptor table count matches OED material table count");
		for (size_t i = 0; i < oed::kMaterialInfoTableCount; ++i) {
			const auto &info = oed::kMaterialInfoTable[i];
			const auto *descriptor = oed::find_material_descriptor(info.name);
			expect(descriptor != nullptr, std::string("descriptor exists for ") + info.name);
			expect(std::string(descriptor->name) == info.name,
			       std::string("descriptor exact-name lookup for ") + info.name);
			expect(static_cast<uint32_t>(descriptor->shader_flags) == static_cast<uint32_t>(info.flags),
			       std::string("descriptor flags match gMaterialInfoTable for ") + info.name);
			const auto cls = classify_object_material(info.name, 0, 0, 0, 128);
			expect(cls.known_shader, std::string("classifier recognizes descriptor tag ") + info.name);
		}
		for (size_t i = 0; i < oed::kMaterialDescriptorTableCount; ++i) {
			const auto &descriptor = oed::kMaterialDescriptorTable[i];
			bool found = false;
			for (size_t j = 0; j < oed::kMaterialInfoTableCount; ++j) {
				if (std::string(descriptor.name) == oed::kMaterialInfoTable[j].name) {
					found = true;
					break;
				}
			}
			expect(found, std::string("descriptor references known OED tag ") + descriptor.name);
		}
	}

	// 1. Plain FF_ST_OP - opaque fixed-function diffuse.
	{
		const auto cls = classify_object_material("FF_ST_OP", 0, 0, 0, 128);
		expect(cls.known_shader, "FF_ST_OP should be known");
		expect(cls.family == ObjectShaderFamily::FixedFunction, "FF_ST_OP family");
		expect(cls.blend == ObjectBlendMode::Opaque, "FF_ST_OP blend opaque");
		expect(!cls.is_glass, "FF_ST_OP not glass");
		expect(!cls.is_two_sided, "FF_ST_OP not two-sided when flags=0");
	}

	// 2. FF_ST_AB - alpha-blended FF.
	{
		const auto cls = classify_object_material("FF_ST_AB", 0, 0, 0, 128);
		expect(cls.blend == ObjectBlendMode::AlphaBlend, "FF_ST_AB blend AlphaBlend");
		expect(cls.family == ObjectShaderFamily::FixedFunction, "FF_ST_AB family");
	}

	// 3. FF_ST_AD - additive FF.
	{
		const auto cls = classify_object_material("FF_ST_AD", 0, 0, 0, 128);
		expect(cls.blend == ObjectBlendMode::Additive, "FF_ST_AD additive");
	}

	// 4. FF_ST_OP_LUM - luminance variant should bypass lighting.
	{
		const auto cls = classify_object_material("FF_ST_OP_LUM", 0, 2, 0, 128);
		expect(cls.is_luminance, "FF_ST_OP_LUM luminance");
		expect(cls.is_emissive, "FF_ST_OP_LUM emissive when emissive_type=2");
		expect(cls.family == ObjectShaderFamily::FixedFunction, "FF_ST_OP_LUM family");
	}

	// 5. FFP_GLASS - Glass.fx normal technique is additive/no-depth-write.
	{
		const auto cls = classify_object_material("FFP_GLASS", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Glass, "FFP_GLASS family");
		expect(cls.is_glass, "FFP_GLASS glass flag");
		expect(cls.uses_environment, "FFP_GLASS samples the environment cube");
		expect(cls.blend == ObjectBlendMode::Additive, "FFP_GLASS uses additive blending from Glass.fx");
	}

	// 6. VS_PHONGT - Phong family with normal map + specular.
	{
		const auto cls = classify_object_material("VS_PHONGT", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Phong, "VS_PHONGT family");
		expect(cls.needs_normal_map, "VS_PHONGT has normal map");
		expect(cls.uses_specular, "VS_PHONGT specular");
		expect(cls.normal_space == ObjectNormalSpace::Tangent, "VS_PHONGT tangent space");
	}

	// 7. VS_DOT3DIFFOBJ - Dot3DiffO.fx uses the Phong-bump execution path
	// without specular, and its normal map is object-space.
	{
		const auto cls = classify_object_material("VS_DOT3DIFFOBJ", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Phong, "VS_DOT3DIFFOBJ family");
		expect(cls.needs_normal_map, "VS_DOT3DIFFOBJ has normal map");
		expect(!cls.uses_specular, "VS_DOT3DIFFOBJ has no specular term");
		expect(cls.normal_space == ObjectNormalSpace::Object, "VS_DOT3DIFFOBJ object space");
	}

	// 8. VS_DOT3DIFF2 - the two-texture BDiffT2 effect stays in the Dot3 path.
	{
		const auto cls = classify_object_material("VS_DOT3DIFF2", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Dot3, "VS_DOT3DIFF2 family");
		expect(cls.has_detail, "VS_DOT3DIFF2 has detail map");
		expect(cls.needs_normal_map, "VS_DOT3DIFF2 has normal map");
		expect(cls.normal_space == ObjectNormalSpace::Tangent, "VS_DOT3DIFF2 tangent space");
	}

	// 8. Skinned tangent-bump shaders are opaque despite the legacy GLASS table flag.
	{
		const uint8_t glass_flags[] = { 0, 1 };
		for (const char *tag : { "VS_SKBUMPDIFFT", "VS_SKBUMPPHONGT", "VS_SKBUMPDIFFT2" }) {
			for (uint8_t is_glass_flag : glass_flags) {
				const auto cls = classify_object_material(tag, 0, 0, is_glass_flag, 128);
				expect(cls.blend == ObjectBlendMode::Opaque, "skinned tangent-bump shader stays opaque");
				expect(cls.is_glass, "skinned tangent-bump shader preserves the OED glass capability bit");
				expect(cls.needs_normal_map, "skinned tangent-bump shader has normal map");
				expect(cls.is_skinned, "skinned tangent-bump shader is skinned");
			}
		}
		const auto with_detail = classify_object_material("VS_SKBUMPDIFFT2", 0, 0, 0, 128);
		expect(with_detail.has_detail, "VS_SKBUMPDIFFT2 has detail map");
	}

	// 9. Environment mirror effects are reflective but their normal pass is opaque.
	{
		const auto cls = classify_object_material("VS_BMTXMIRRT", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Environment, "VS_BMTXMIRRT family");
		expect(cls.blend == ObjectBlendMode::Opaque, "VS_BMTXMIRRT normal pass is opaque");
		expect(cls.is_glass, "VS_BMTXMIRRT preserves glass/reflect flag");
		expect(cls.uses_environment, "VS_BMTXMIRRT uses environment reflection");
		expect(cls.normal_space == ObjectNormalSpace::Tangent, "VS_BMTXMIRRT tangent space");
	}

	// 9. VS_FLAG - Flag wind sway family.
	{
		const auto cls = classify_object_material("VS_FLAG", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Flag, "VS_FLAG family");
	}

	// 10. VS_LEAVESWIND - not an original OED gMaterialInfoTable shader.
	{
		const auto with_at = classify_object_material(
			"VS_LEAVESWIND", THREEDI_MATERIAL_FLAG_ALPHA_TEST, 0, 0, 128);
		expect(!with_at.known_shader, "VS_LEAVESWIND with alpha_test remains unknown");
		expect(with_at.family == ObjectShaderFamily::Unknown,
		       "VS_LEAVESWIND with alpha_test has unknown family");
		expect(with_at.blend == ObjectBlendMode::Opaque,
		       "VS_LEAVESWIND with alpha_test does not receive an FF blend alias");
		const auto without_at = classify_object_material("VS_LEAVESWIND", 0, 0, 0, 128);
		expect(!without_at.known_shader, "VS_LEAVESWIND without alpha_test remains unknown");
		expect(without_at.family == ObjectShaderFamily::Unknown,
		       "VS_LEAVESWIND without alpha_test has unknown family");
		expect(without_at.blend == ObjectBlendMode::Opaque,
		       "VS_LEAVESWIND without alpha_test stays opaque fallback");
	}

	// 11. Two-sided + alpha-test bits propagate from the material_flags byte.
	{
		const uint8_t flags = THREEDI_MATERIAL_FLAG_TWO_SIDED |
		                      THREEDI_MATERIAL_FLAG_ALPHA_TEST |
		                      THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
		const auto cls = classify_object_material("FF_ST_OP", flags, 0, 0, 200);
		expect(cls.is_two_sided, "two-sided propagates");
		expect(cls.alpha_test, "alpha_test propagates");
		expect(cls.alpha_test_invert, "alpha_test_invert propagates");
		expect(cls.alpha_test_value > 0.78f && cls.alpha_test_value < 0.79f,
		       "alpha_test_value 200/255 ~= 0.784");
	}

	// 12. Shader key encoding round-trips family + blend.
	{
		const auto cls = classify_object_material("VS_PHONGT", 0, 0, 0, 128);
		const auto key = build_object_shader_key(cls);
		expect(decode_object_shader_family(key) == ObjectShaderFamily::Phong,
		       "key decodes Phong family");
		expect(decode_object_shader_blend(key) == ObjectBlendMode::Opaque,
		       "key decodes Opaque blend");
	}

	// 13. GLSL composer emits sane shader source for each family.
	{
		const auto fk = build_object_shader_key(classify_object_material("FF_ST_OP", 0, 0, 0, 128));
		const std::string ff_glsl = compose_object_shader_glsl(fk);
		expect(contains(ff_glsl, "shader_type spatial"), "FF GLSL is spatial");
		expect(contains(ff_glsl, "obj_ff_lighting"), "FF GLSL uses ff lighting helper");
		expect(contains(ff_glsl, "cull_back"), "FF cull_back default");

		const auto mk = build_object_shader_key(classify_object_material("FF_MT_OP", 0, 0, 0, 128));
		const std::string mt_glsl = compose_object_shader_glsl(mk);
		expect(contains(mt_glsl, "texture(u_detail, v_uv2)"),
		       "Multi-texture GLSL samples detail maps from secondary UVs");
		expect(!contains(mt_glsl, "v_uv * 4.0"),
		       "Multi-texture GLSL should not re-scale baked detail UVs");

		const auto gk = build_object_shader_key(classify_object_material("FFP_GLASS", 0, 0, 1, 128));
		const std::string glass_glsl = compose_object_shader_glsl(gk);
		expect(contains(glass_glsl, "blend_add"), "Glass emits additive render mode");
		expect(contains(glass_glsl, "u_reflect_color"), "Glass exposes reflect color");
		expect(contains(glass_glsl, "fresnel"), "Glass fragment computes fresnel");
		expect(!contains(glass_glsl, "ALPHA ="), "Additive glass does not write AlphaBlend opacity");

		const auto lk = build_object_shader_key(classify_object_material("VS_FLAG", 0, 0, 0, 128));
		const std::string flag_glsl = compose_object_shader_glsl(lk);
		expect(contains(flag_glsl, "u_wind_amount"), "Flag exposes wind amount");
		expect(contains(flag_glsl, "TIME"), "Flag uses TIME for sway");

		const auto pk = build_object_shader_key(classify_object_material("VS_PHONGT", 0, 0, 0, 128));
		const std::string phong_glsl = compose_object_shader_glsl(pk);
		expect(contains(phong_glsl, "u_normal_map"), "Phong samples normal map");
		expect(contains(phong_glsl, "spec"), "Phong has specular term");

		const auto skinned_key = build_object_shader_key(classify_object_material("VS_SKBUMPDIFFT2", 0, 0, 0, 128));
		const std::string skinned_glsl = compose_object_shader_glsl(skinned_key);
		expect(contains(skinned_glsl, "depth_draw_opaque"),
		       "Skinned bump/detail GLSL writes depth as opaque");
		expect(!contains(skinned_glsl, "ALPHA ="),
		       "Skinned bump/detail GLSL does not use texture alpha as opacity");

		ObjectMaterialClassification normal_b_cls;
		normal_b_cls.family = ObjectShaderFamily::Dot3;
		normal_b_cls.needs_normal_map = true;
		normal_b_cls.normal_uses_uv2 = true;
		normal_b_cls.normal_space = ObjectNormalSpace::Tangent;
		const std::string normal_b_glsl = compose_object_shader_glsl(build_object_shader_key(normal_b_cls));
		expect(contains(normal_b_glsl, "texture(u_normal_map, v_uv2)"),
		       "Normal-B GLSL samples normal maps from secondary UVs");

		const auto two_sided_key = build_object_shader_key(classify_object_material(
			"FF_ST_OP", THREEDI_MATERIAL_FLAG_TWO_SIDED, 0, 0, 128));
		const std::string two_sided_glsl = compose_object_shader_glsl(two_sided_key);
		expect(contains(two_sided_glsl, "cull_disabled"),
		       "two-sided emits cull_disabled");
	}

	std::cerr << "renderer_material_classify_test ok\n";
	return 0;
}
