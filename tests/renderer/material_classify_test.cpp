// Unit tests for renderer::classify_object_material + its typed pipeline
// descriptor in engine/runtime/renderer. Verifies the static shader_tag -> family/blend table without
// `.fx` parsing matches the canonical runtime classifications for every
// shader_tag in the original OED's gMaterialInfoTable (relocated as the test
// oracle tests/renderer/material_info_oracle.h).

#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"
#include "renderer/material_descriptor.h"
#include "threedi/threedi_3di3.h"

#include "renderer/material_info_oracle.h"

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

	// Retail's generated gsys_phong bytes, including its truncation rule.
	expect(object_phong_map_texel(0, 0) == ObjectPhongMapTexel{0, 0, 0, 0},
	       "PhongMap black endpoint");
	expect(object_phong_map_texel(37, 64) == ObjectPhongMapTexel{1, 0, 0, 37},
	       "PhongMap quarter N.H row and independent N.L alpha");
	expect(object_phong_map_texel(128, 192) == ObjectPhongMapTexel{81, 2, 0, 128},
	       "PhongMap pow-4/16/64 truncation at row 192");
	expect(object_phong_map_texel(254, 254) == ObjectPhongMapTexel{251, 239, 198, 254},
	       "PhongMap penultimate row preserves the binary32 1/255 source");
	expect(object_phong_map_texel(255, 255) == ObjectPhongMapTexel{255, 255, 255, 255},
	       "PhongMap white endpoint");

	// 0. The renderer descriptor table is exact and complete for the OED dump
	// (tests/renderer/material_info_oracle.h, the relocated gMaterialInfoTable).
	// The shader_flags column matches the OED dump EXCEPT the five rows where
	// the runtime derivation is witnessed to differ (D-RMAT-4,
	// docs/render/render-material-re.md): retail probes every technique at
	// .fx load [orig: HLSLEffect_LoadFromFile @ 0x5ae690], and the descriptor
	// table carries those runtime words.
	{
		// The oracle's bit vocabulary is the dump's own; the descriptor table
		// must agree on every value or the row comparison below means nothing.
		static_assert(kMaterialDescriptorTableCount == material_oracle::kMaterialInfoTableCount,
		              "material descriptor table must mirror the OED material-info dump row for row");
#define OPENNOVA_ORACLE_BIT(bit) \
		static_assert(static_cast<uint32_t>(MATERIAL_FLAG_##bit) == \
		              static_cast<uint32_t>(material_oracle::MATERIAL_FLAG_##bit), \
		              "MATERIAL_FLAG_" #bit " must keep the dump's bit value")
		OPENNOVA_ORACLE_BIT(EMISSIVE); OPENNOVA_ORACLE_BIT(ALPHA); OPENNOVA_ORACLE_BIT(DIFFUSE);
		OPENNOVA_ORACLE_BIT(SECONDARY); OPENNOVA_ORACLE_BIT(NORMAL_A); OPENNOVA_ORACLE_BIT(NORMAL_B);
		OPENNOVA_ORACLE_BIT(BLENDING); OPENNOVA_ORACLE_BIT(GLASS); OPENNOVA_ORACLE_BIT(SKINNED);
		OPENNOVA_ORACLE_BIT(TANGENT); OPENNOVA_ORACLE_BIT(UVGEN); OPENNOVA_ORACLE_BIT(GLOW);
#undef OPENNOVA_ORACLE_BIT

		struct RuntimeFlagFix { const char *tag; uint32_t dump_flags; uint32_t flags; };
		const RuntimeFlagFix runtime_fixes[] = {
			// no VS => no TANGENT; the GLOW technique uses TexCubeRotSpecular
			{ "FFP_GLASS", 0xb000u, MATERIAL_FLAG_GLASS | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_GLOW },
			// tangent-space skinned bump: In.Tangent read, ReflectColor absent
			{ "VS_SKBUMPDIFFT", 0x6014u, MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED |
			                             MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE },
			{ "VS_SKBUMPPHONGT", 0x6014u, MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED |
			                              MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE },
			{ "VS_SKBUMPDIFFT2", 0x601cu, MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_SKINNED |
			                              MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE |
			                              MATERIAL_FLAG_SECONDARY },
			// untextured glass: no TexDiffuse1 reference
			{ "VS_SKGLASS", 0x7004u, MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_BLENDING },
		};
		expect(kMaterialDescriptorTableCount == material_oracle::kMaterialInfoTableCount,
		       "descriptor table count matches OED material table count");
		size_t fixes_seen = 0;
		for (size_t i = 0; i < material_oracle::kMaterialInfoTableCount; ++i) {
			const auto &info = material_oracle::kMaterialInfoTable[i];
			const auto *descriptor = find_material_descriptor(info.name);
			expect(descriptor != nullptr, std::string("descriptor exists for ") + info.name);
			expect(std::string(descriptor->name) == info.name,
			       std::string("descriptor exact-name lookup for ") + info.name);
			uint32_t expected_flags = static_cast<uint32_t>(info.flags);
			for (const RuntimeFlagFix &fix : runtime_fixes) {
				if (std::string(info.name) == fix.tag) {
					expect(expected_flags == fix.dump_flags,
					       std::string("the OED dump word for ") + info.name +
					               " is the one D-RMAT-4 witnessed");
					expected_flags = fix.flags;
					++fixes_seen;
					break;
				}
			}
			expect(static_cast<uint32_t>(descriptor->shader_flags) == expected_flags,
			       std::string("descriptor flags match the runtime derivation for ") + info.name);
			const auto cls = classify_object_material(info.name, 0, 0, 0, 128);
			expect(cls.known_shader, std::string("classifier recognizes descriptor tag ") + info.name);
		}
		expect(fixes_seen == 5, "the dump carries all five D-RMAT-4 rows");
		// The corrected words themselves, as D-RMAT-4 lists them.
		expect(find_material_descriptor("FFP_GLASS")->shader_flags == 0x10003000u,
		       "FFP_GLASS 0xb000 -> 0x10003000");
		expect(find_material_descriptor("VS_SKBUMPDIFFT")->shader_flags == 0xc014u,
		       "VS_SKBUMPDIFFT 0x6014 -> 0xc014");
		expect(find_material_descriptor("VS_SKBUMPPHONGT")->shader_flags == 0xc014u,
		       "VS_SKBUMPPHONGT 0x6014 -> 0xc014");
		expect(find_material_descriptor("VS_SKBUMPDIFFT2")->shader_flags == 0xc01cu,
		       "VS_SKBUMPDIFFT2 0x601c -> 0xc01c");
		expect(find_material_descriptor("VS_SKGLASS")->shader_flags == 0x7000u,
		       "VS_SKGLASS 0x7004 -> 0x7000");
		for (size_t i = 0; i < kMaterialDescriptorTableCount; ++i) {
			const auto &descriptor = kMaterialDescriptorTable[i];
			bool found = false;
			for (size_t j = 0; j < material_oracle::kMaterialInfoTableCount; ++j) {
				if (std::string(descriptor.name) == material_oracle::kMaterialInfoTable[j].name) {
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

	// 8. Skinned tangent-bump shaders are opaque, and — per the runtime
	// capability probe (D-RMAT-4) — NOT glass by tag: their effects read
	// In.Tangent and never reference ReflectColor [orig: probe @ 0x5ae690
	// over SkBDiffT/SkBPhongT/SkBDiffT2 + _vsSkDfT.fx]. The legacy dump's GLASS
	// bit on these rows was table drift. The 3DI per-material glass override
	// still applies.
	{
		for (const char *tag : { "VS_SKBUMPDIFFT", "VS_SKBUMPPHONGT", "VS_SKBUMPDIFFT2" }) {
			const auto cls = classify_object_material(tag, 0, 0, 0, 128);
			expect(cls.blend == ObjectBlendMode::Opaque, "skinned tangent-bump shader stays opaque");
			expect(!cls.is_glass, "skinned tangent-bump shader is not glass by tag (runtime probe)");
			expect(cls.needs_normal_map, "skinned tangent-bump shader has normal map");
			expect(cls.is_skinned, "skinned tangent-bump shader is skinned");
			const auto with_override = classify_object_material(tag, 0, 0, 1, 128);
			expect(with_override.is_glass, "the per-material glass override still applies");
		}
		const auto with_detail = classify_object_material("VS_SKBUMPDIFFT2", 0, 0, 0, 128);
		expect(with_detail.has_detail, "VS_SKBUMPDIFFT2 has detail map");
		const auto detail_pipeline = describe_object_shader_pipeline(
			build_object_shader_key(with_detail));
		expect(detail_pipeline.technique ==
		               ObjectShaderTechnique::Dot3TangentDetailSkinned,
		       "SkBDiffT2 keeps its fixed-function encoded-vector topology");
		const auto no_detail_pipeline = describe_object_shader_pipeline(
			build_object_shader_key(with_detail) & ~OSCAP_DETAIL);
		expect(no_detail_pipeline.technique == ObjectShaderTechnique::Dot3TangentSkinned,
		       "missing SkBDiffT2 detail drops only its second texture stage");
	}

	// 8b. The glow-copy capability and the tracer view fade (REN-4):
	// 0x10000000 = "renders a Q3 glow/bloom duplicate" [orig: Q3 gate
	// @ 0x5d93b5], carried by the FF _LUM rows AND FFP_GLASS at runtime
	// (union probe over its GLOW technique); the self-lum LOOK keys on
	// EMISSIVE. VS_TRACER carries the vsTracer view-angle fade (D-RMAT-2).
	{
		expect(classify_object_material("FF_ST_OP_LUM", 0, 0, 0, 128).is_glow_capable,
		       "FF_ST_OP_LUM is glow-capable");
		expect(classify_object_material("FFP_GLASS", 0, 0, 0, 128).is_glow_capable,
		       "FFP_GLASS is glow-capable (runtime probe over the GLOW technique)");
		expect(!classify_object_material("FFP_GLASS", 0, 0, 0, 128).is_luminance,
		       "FFP_GLASS is not self-lum (glow capability != the LUM look)");
		expect(!classify_object_material("FF_ST_OP", 0, 0, 0, 128).is_glow_capable,
		       "plain FF_ST_OP is not glow-capable");
		const auto tracer = classify_object_material("VS_TRACER", 0, 0, 0, 128);
		expect(tracer.view_angle_fade, "VS_TRACER carries the view-angle fade");
		const auto tracer_key = build_object_shader_key(tracer);
		expect((tracer_key & OSCAP_VIEW_FADE) != 0, "tracer key carries OSCAP_VIEW_FADE");
		const auto tracer_pipeline = describe_object_shader_pipeline(tracer_key);
		expect(tracer_pipeline.view_angle_fade,
		       "tracer pipeline carries the |dot(eye,normal)|^2 fade capability [orig: vsTracer]");
	}

	// 9. Environment mirror effects are reflective but their normal pass is opaque.
	{
		const auto mirror_cls = classify_object_material("VS_BUMPMIRRT", 0, 0, 0, 128);
		expect(mirror_cls.family == ObjectShaderFamily::Environment, "VS_BUMPMIRRT family");
		expect(!mirror_cls.environment_textured,
		       "VS_BUMPMIRRT leaves the reflection untextured");
		const auto mirror = describe_object_shader_pipeline(build_object_shader_key(mirror_cls));
		expect(mirror.technique == ObjectShaderTechnique::EnvironmentMirror,
		       "VS_BUMPMIRRT selects the untextured mirror technique");

		const auto cls = classify_object_material("VS_BMTXMIRRT", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Environment, "VS_BMTXMIRRT family");
		expect(cls.blend == ObjectBlendMode::Opaque, "VS_BMTXMIRRT normal pass is opaque");
		expect(cls.is_glass, "VS_BMTXMIRRT preserves glass/reflect flag");
		expect(cls.uses_environment, "VS_BMTXMIRRT uses environment reflection");
		expect(cls.environment_textured,
		       "VS_BMTXMIRRT post-multiplies the reflection by Diffuse1");
		expect(cls.normal_space == ObjectNormalSpace::Tangent, "VS_BMTXMIRRT tangent space");
		const auto textured = describe_object_shader_pipeline(build_object_shader_key(cls));
		expect(textured.technique == ObjectShaderTechnique::EnvironmentMirrorTextured,
		       "VS_BMTXMIRRT selects the textured mirror technique");
	}

	// 9. VS_FLAG - Flag wind sway family.
	{
		const auto cls = classify_object_material("VS_FLAG", 0, 0, 0, 128);
		expect(cls.family == ObjectShaderFamily::Flag, "VS_FLAG family");
		const auto emissive = classify_object_material("VS_FLAG", 0, 2, 0, 128);
		expect(describe_object_shader_pipeline(build_object_shader_key(emissive)).technique ==
		               ObjectShaderTechnique::Flag,
		       "Flag.fx has no invented self-lit NORMAL technique");
	}

	// 10. VS_LEAVESWIND - not an legacy authoring table shader.
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

	// 13. The renderer-neutral pipeline descriptor carries every behavior the
	// Godot adapter needs to select a checked-in shader and bind capabilities.
	{
		const auto fk = build_object_shader_key(classify_object_material("FF_ST_OP", 0, 0, 0, 128));
		const auto ff = describe_object_shader_pipeline(fk);
		expect(ff.family == ObjectShaderFamily::FixedFunction, "FF pipeline family");
		expect(ff.blend == ObjectBlendMode::Opaque, "FF pipeline blend");
		expect(ff.depth == ObjectDepthPolicy::Opaque, "FF writes opaque depth");
		expect(ff.cull == ObjectCullPolicy::Back, "FF culls back faces");
		expect(ff.technique == ObjectShaderTechnique::Fixed, "FF technique is structural");
		expect(!ff.writes_alpha, "opaque FF does not write fragment alpha");
		expect(!ff.uses_detail && !ff.uses_normal_map, "plain FF has no extra texture stages");

		const auto sk_basic = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("VS_SKBASIC", 0, 0, 0, 128)));
		expect(sk_basic.technique == ObjectShaderTechnique::FixedSkinned,
		       "SkBasic keeps its shader alpha/RGB contract separate from _FFP");
		const auto synthetic_emissive_sk_basic = describe_object_shader_pipeline(
			build_object_shader_key(classify_object_material(
				"VS_SKBASIC", 0, THREEDI_EMISSIVE_FULL, 0, 128)));
		expect(synthetic_emissive_sk_basic.technique ==
		               ObjectShaderTechnique::FixedSkinned,
		       "an emissive byte cannot invent a SkBasic SELFLUM technique");

		const auto mk = build_object_shader_key(classify_object_material("FF_MT_OP", 0, 0, 0, 128));
		const auto mt = describe_object_shader_pipeline(mk);
		expect(mt.uses_detail, "multi-texture pipeline enables the detail stage");
		expect(mt.technique == ObjectShaderTechnique::FixedDetail,
		       "multi-texture pipeline selects the checked-in detail technique");

		const auto gk = build_object_shader_key(classify_object_material("FFP_GLASS", 0, 0, 1, 128));
		const auto glass = describe_object_shader_pipeline(gk);
		expect(glass.family == ObjectShaderFamily::Glass, "glass pipeline family");
		expect(glass.blend == ObjectBlendMode::Additive, "glass pipeline is additive");
		expect(glass.depth == ObjectDepthPolicy::TransparentNoWrite,
		       "additive glass does not write depth");
		expect(glass.environment_source == ObjectEnvironmentSource::HemisphereApproximation,
		       "glass explicitly reports the tracked hemisphere environment stand-in");
		expect(glass.technique == ObjectShaderTechnique::GlassFixed,
		       "glass selects its checked-in technique");
		expect(!glass.writes_alpha, "additive glass does not write AlphaBlend opacity");

		const auto lk = build_object_shader_key(classify_object_material("VS_FLAG", 0, 0, 0, 128));
		const auto flag = describe_object_shader_pipeline(lk);
		expect(flag.family == ObjectShaderFamily::Flag, "flag pipeline selects wind deformation");
		expect(flag.technique == ObjectShaderTechnique::Flag,
		       "flag selects its checked-in technique");

		const auto pk = build_object_shader_key(classify_object_material("VS_PHONGT", 0, 0, 0, 128));
		const auto phong = describe_object_shader_pipeline(pk);
		expect(phong.uses_normal_map, "Phong pipeline samples a normal map");
		expect(phong.normal_space == ObjectNormalSpace::Tangent,
		       "Phong pipeline reports tangent-space normals");
		expect(phong.specular_source == ObjectSpecularSource::AnalyticPow8DiffuseAlpha,
		       "Phong reports the retail pow-8 lobe scaled by Diffuse1 alpha");
		expect(phong.technique == ObjectShaderTechnique::PhongTangentSpecular,
		       "Phong tangent/specular topology is selected at classification time");

		const auto skinned_tangent = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("VS_SKBUMPPHONGT", 0, 0, 0, 128)));
		expect(skinned_tangent.technique ==
		               ObjectShaderTechnique::PhongTangentSpecularSkinned,
		       "skinned tangent Phong selects its distinct retail vertex topology");
		expect(skinned_tangent.specular_source ==
		               ObjectSpecularSource::AnalyticPow8DiffuseAlpha,
		       "skinned tangent Phong keeps the pow-8 Diffuse1-alpha lobe");

		const auto phong_map = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("VS_SKBUMPPHONGOBJ", 0, 0, 0, 128)));
		expect(phong_map.technique ==
		               ObjectShaderTechnique::PhongObjectSpecularPhongMap,
		       "skinned object Phong selects the PhongMap topology");
		expect(phong_map.specular_source ==
		               ObjectSpecularSource::PhongMapLookupDiffuseAlpha,
		       "PhongMap reports Diffuse1 alpha as its pow-4/pow-64 control");

		const auto skinned_glass = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("VS_SKGLASS", 0, 0, 0, 128)));
		expect(skinned_glass.technique == ObjectShaderTechnique::GlassSkinned,
		       "VS_SKGLASS selects its untextured skinned glass technique");

		const auto skinned_key = build_object_shader_key(classify_object_material("VS_SKBUMPDIFFT2", 0, 0, 0, 128));
		const auto skinned = describe_object_shader_pipeline(skinned_key);
		expect(skinned.depth == ObjectDepthPolicy::Opaque,
		       "skinned bump/detail pipeline writes depth as opaque");
		expect(!skinned.writes_alpha,
		       "skinned bump/detail pipeline does not use texture alpha as opacity");
		expect(skinned.technique == ObjectShaderTechnique::Dot3TangentDetailSkinned,
		       "skinned bump/detail selects its authored DOT3 technique");

		ObjectMaterialClassification normal_b_cls;
		normal_b_cls.family = ObjectShaderFamily::Dot3;
		normal_b_cls.needs_normal_map = true;
		normal_b_cls.normal_uses_uv2 = true;
		normal_b_cls.normal_space = ObjectNormalSpace::Tangent;
		const auto normal_b = describe_object_shader_pipeline(build_object_shader_key(normal_b_cls));
		expect(normal_b.normal_uses_uv2,
		       "Normal-B pipeline samples normal maps from secondary UVs");
		expect(normal_b.technique == ObjectShaderTechnique::Unsupported,
		       "unreachable normal-UV2 topology fails closed in backend selection");

		const auto two_sided_key = build_object_shader_key(classify_object_material(
			"FF_ST_OP", THREEDI_MATERIAL_FLAG_TWO_SIDED, 0, 0, 128));
		const auto two_sided = describe_object_shader_pipeline(two_sided_key);
		expect(two_sided.cull == ObjectCullPolicy::Disabled,
		       "two-sided pipeline disables culling");

		const auto cutout_key = build_object_shader_key(classify_object_material(
			"FF_ST_OP", THREEDI_MATERIAL_FLAG_ALPHA_TEST, 0, 0, 128));
		const auto cutout = describe_object_shader_pipeline(cutout_key);
		expect(cutout.alpha_test && cutout.depth == ObjectDepthPolicy::Opaque,
		       "opaque alpha-tested pipeline writes only surviving fragments");

		const auto blended_cutout = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("FF_ST_AB", THREEDI_MATERIAL_FLAG_ALPHA_TEST,
					0, 0, 128)));
		expect(blended_cutout.depth == ObjectDepthPolicy::TransparentNoWrite,
		       "alpha testing does not turn a blended retail technique into a depth writer");

		const auto alpha_key = build_object_shader_key(classify_object_material(
			"FF_ST_AB", 0, 0, 0, 128));
		const auto alpha = describe_object_shader_pipeline(alpha_key);
		expect(alpha.writes_alpha && alpha.depth == ObjectDepthPolicy::TransparentNoWrite,
		       "alpha-blended pipeline owns ALPHA and does not write depth");

		const auto self_lit = describe_object_shader_pipeline(build_object_shader_key(
			classify_object_material("FF_MT_OP_LUM", 0, 2, 0, 128)));
		expect(self_lit.technique == ObjectShaderTechnique::SelfLitDetail,
		       "self-lit detail topology is selected without a runtime cap branch");
	}

	// 14. PROJSHAD state is technique-owned, not inherited from NORMAL. The
	// decoded retail corpus has one material-variant _FFP declaration, fifteen
	// hard-opaque shader declarations, and no pass for tracer/flag/glass.
	// Shared declarations expand to all 24 reachable runtime techniques.
	// [orig: _FFP.fx TBoringFFPProjShad; shipped TECHNIQUE_PROJSHAD declarations]
	{
		struct ProjectedCase {
			ObjectShaderTechnique technique;
			ObjectProjectedShadowPolicy policy;
		};
		const ProjectedCase cases[] = {
			{ObjectShaderTechnique::Unsupported, ObjectProjectedShadowPolicy::NoPass},
			{ObjectShaderTechnique::Fixed, ObjectProjectedShadowPolicy::MaterialBlend},
			{ObjectShaderTechnique::FixedSkinned, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::FixedDetail, ObjectProjectedShadowPolicy::MaterialBlend},
			{ObjectShaderTechnique::SelfLit, ObjectProjectedShadowPolicy::MaterialBlend},
			{ObjectShaderTechnique::SelfLitDetail, ObjectProjectedShadowPolicy::MaterialBlend},
			{ObjectShaderTechnique::Tracer, ObjectProjectedShadowPolicy::NoPass},
			{ObjectShaderTechnique::Flag, ObjectProjectedShadowPolicy::NoPass},
			{ObjectShaderTechnique::PhongTangentDiffuse, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::PhongTangentSpecular, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::PhongTangentSpecularSkinned, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::PhongObjectDiffuse, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::PhongObjectSpecular, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::PhongObjectSpecularPhongMap, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3Tangent, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3TangentDetail, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3TangentSkinned, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3TangentDetailSkinned, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3Object, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::Dot3ObjectDetail, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::EnvironmentMirror, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::EnvironmentMirrorTextured, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::EnvironmentPhong, ObjectProjectedShadowPolicy::Opaque},
			{ObjectShaderTechnique::GlassFixed, ObjectProjectedShadowPolicy::NoPass},
			{ObjectShaderTechnique::GlassSkinned, ObjectProjectedShadowPolicy::NoPass},
		};
		for (const ProjectedCase &test : cases) {
			expect(object_projected_shadow_policy(test.technique) == test.policy,
			       "every object technique preserves its retail PROJSHAD state");
		}
	}

	std::cerr << "renderer_material_classify_test ok\n";
	return 0;
}
