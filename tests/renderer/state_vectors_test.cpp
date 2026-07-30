// Render-state parity vectors (REN-1, the T1 instrument — ADR 0023).
//
// Walks the full material input matrix — every shader tag in the canonical
// 45-entry descriptor table (plus unknown-tag probes) x the THREEDI material
// flag byte x emissive x glass x alpha-test byte — through the
// classification/composition chain (classify_object_material ->
// build_object_shader_key -> compose_object_shader_glsl) and compares the
// canonical text against the committed golden
// (tests/renderer/render_state_vectors.golden).
//
// POLICY (mirrors ENG-1's env parity vectors, godot/tests/env_parity_vectors_test.gd):
//
//  - The golden was dumped ONCE from the then-current implementation
//    (pinned-current). During the REN grills its rows converge to the
//    witnessed-expected state decoded from Jointops.exe
//    (docs/render/render-material-re.md when it lands).
//  - A mismatch means the producer chain changed. If the change was NOT a
//    deliberate, witnessed port step: it is a regression — fix the code,
//    never the golden.
//  - A deliberate change re-dumps ONLY because the producer changed, and the
//    commit that re-dumps carries the witness citation
//    ([orig: Name @ 0xADDR] or a D-RMAT/ledger row). Tolerances are never
//    widened; there are none here — comparison is exact text.
//  - Dump mode is LOUD: OPENNOVA_RENDER_VECTORS_DUMP=1 rewrites the golden
//    in the source tree and then FAILS the test, so a dump run can never be
//    mistaken for green.
//
// REN-3 extended the set with section 3: draw-order vectors (sort keys,
// technique-class selection, the water bracket + priority ladder) pinning
// libs/renderer/render_order against docs/render/render-order-re.md
// [orig: RenderBatch_QuickSort @ 0x5d8b40; collect_render_objects_for_batch
// @ 0x5d8f20; Terrain_RenderSceneWithReflection @ 0x5c93a0].
//
// REN-5 added section 5: lighting scalars over libs/renderer/light_runtime
// (docs/render/render-lighting-re.md) — the modulator /64 scale, the world
// lighting block + overrides, the entity uniforms incl. the interior
// daylight lerp, the FF vertex light + MODULATE2X, sun visibility,
// point-light color/attenuation, and the terrain c0/c1 surface light. The
// same slice re-composed the object shaders onto the witnessed uniform
// surface (HemiSky/HemiGround/DirLight/ColorSrcGlobalGain, the x2 combine —
// D-RMAT-5), re-hashing every section-2 row under that citation.
//
// REN-4 extended the classification rows with glow= (the 0x10000000 Q3
// bloom-copy capability [orig: probe @ 0x5ae690; Q3 gate @ 0x5d93b5]) and
// vfade= (the vsTracer view-angle fade, D-RMAT-2), and added section 4:
// uv-anim vectors over libs/renderer/uv_anim
// [orig: compute_uv_transform_matrix @ 0x5b1990; wave_lookup @ 0x5de6b0].

#include "renderer/light_runtime.h"
#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"
#include "renderer/render_order.h"
#include "renderer/uv_anim.h"
#include "oed/material_descriptor.h"
#include "threedi/threedi_3di3.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *blend_name(renderer::ObjectBlendMode blend) {
	switch (blend) {
		case renderer::ObjectBlendMode::Opaque: return "opaque";
		case renderer::ObjectBlendMode::AlphaBlend: return "alpha_blend";
		case renderer::ObjectBlendMode::Additive: return "additive";
		case renderer::ObjectBlendMode::Multiplicative: return "multiplicative";
	}
	return "?";
}

const char *normal_space_name(renderer::ObjectNormalSpace space) {
	switch (space) {
		case renderer::ObjectNormalSpace::None: return "none";
		case renderer::ObjectNormalSpace::Tangent: return "tangent";
		case renderer::ObjectNormalSpace::Object: return "object";
	}
	return "?";
}

uint64_t fnv1a64(const std::string &text) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (unsigned char c : text) {
		hash ^= c;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

std::string generate() {
	using namespace renderer;

	std::ostringstream out;
	out << "# render-state-vectors v1 (renderer_state_vectors_test; REN-1, ADR 0023)\n";

	// Tag list: the full canonical table, in table order, then the
	// unknown-tag probes (a non-OED tag, a case-mismatch probe, and empty —
	// classification is deliberately exact-tag only).
	std::vector<std::string> tags;
	for (size_t i = 0; i < oed::kMaterialInfoTableCount; ++i)
		tags.push_back(oed::kMaterialInfoTable[i].name);
	tags.push_back("VS_LEAVESWIND");
	tags.push_back("ff_st_op");
	tags.push_back("");

	// THREEDI_MATERIAL_FLAG_* byte combos: every subset of
	// ALPHA_TEST(0x01) | ALPHA_INVERT(0x02) | TWO_SIDED(0x04) that is
	// distinct for the classifier, including INVERT-without-TEST (0x02).
	const uint8_t flag_combos[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07 };
	const uint8_t emissive_types[] = { THREEDI_EMISSIVE_NONE, THREEDI_EMISSIVE_FULL };
	const uint8_t glass_flags[] = { 0, 1 };

	std::set<uint32_t> keys;
	char line[512];

	for (const std::string &tag : tags) {
		for (uint8_t flags : flag_combos) {
			// The alpha byte only feeds alpha_test_value; vary it where the
			// ALPHA_TEST flag is set, pin one midpoint sample elsewhere.
			std::vector<uint8_t> alpha_bytes;
			if (flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST)
				alpha_bytes = { 0, 200 };
			else
				alpha_bytes = { 128 };
			for (uint8_t em : emissive_types) {
				for (uint8_t gl : glass_flags) {
					for (uint8_t atb : alpha_bytes) {
						const auto cls = classify_object_material(tag, flags, em, gl, atb);
						const uint32_t key = build_object_shader_key(cls);
						keys.insert(key);
						std::snprintf(line, sizeof(line),
						              "tag=%s flags=%02x em=%u gl=%u atb=%u | "
						              "known=%d fam=%s blend=%s lum=%d emis=%d two=%d "
						              "nmap=%d glass=%d env=%d atest=%d atinv=%d atval=%.6f "
						              "det=%d skin=%d spec=%d nuv2=%d nspace=%s "
						              "glow=%d vfade=%d key=%08x\n",
						              tag.empty() ? "<empty>" : tag.c_str(),
						              flags, em, gl, atb,
						              cls.known_shader ? 1 : 0,
						              object_shader_family_name(cls.family),
						              blend_name(cls.blend),
						              cls.is_luminance ? 1 : 0,
						              cls.is_emissive ? 1 : 0,
						              cls.is_two_sided ? 1 : 0,
						              cls.needs_normal_map ? 1 : 0,
						              cls.is_glass ? 1 : 0,
						              cls.uses_environment ? 1 : 0,
						              cls.alpha_test ? 1 : 0,
						              cls.alpha_test_invert ? 1 : 0,
						              static_cast<double>(cls.alpha_test_value),
						              cls.has_detail ? 1 : 0,
						              cls.is_skinned ? 1 : 0,
						              cls.uses_specular ? 1 : 0,
						              cls.normal_uses_uv2 ? 1 : 0,
						              normal_space_name(cls.normal_space),
						              cls.is_glow_capable ? 1 : 0,
						              cls.view_angle_fade ? 1 : 0,
						              key);
						out << line;
					}
				}
			}
		}
	}

	// Section 2: every unique shader key's composed GLSL, pinned by hash +
	// length. A composer change re-hashes exactly the keys whose source
	// changed; the re-dump carries the citation for the change.
	out << "# composed-glsl (key -> fnv1a64, length)\n";
	for (uint32_t key : keys) {
		const std::string glsl = renderer::compose_object_shader_glsl(key);
		std::snprintf(line, sizeof(line), "key=%08x fnv64=%016llx len=%zu\n",
		              key,
		              static_cast<unsigned long long>(fnv1a64(glsl)),
		              glsl.size());
		out << line;
	}

	// Section 3 (REN-3): draw-order vectors over libs/renderer/render_order —
	// docs/render/render-order-re.md.
	out << "# render-order v1 (REN-3: sort keys, technique classes, water bracket, ladder)\n";

	// Technique-class selection: every stack-default state x a submit-flag
	// sweep including the multi-flag precedence cases
	// [orig: collect_render_objects_for_batch @ 0x5d90d7..0x5d9145].
	const uint32_t stack_states[] = { 0, kStackDefaultClip, kStackDefaultProjShadow,
	                                  kStackDefaultDepthMask };
	const uint32_t submit_states[] = {
		0, kSubmitClipPass, kSubmitProjShadowPass, kSubmitDepthMaskPass,
		kSubmitMatchTerrainPass, kSubmitClipPass | kSubmitMatchTerrainPass,
		kSubmitDepthMaskPass | kSubmitProjShadowPass,
		kSubmitFirstPassOnly | kSubmitAltStream | kSubmitNoGlowCopy,
	};
	for (uint32_t stack : stack_states) {
		for (uint32_t submit : submit_states) {
			std::snprintf(line, sizeof(line), "tclass stack=%x submit=%03x cls=%u\n",
			              stack, submit,
			              static_cast<unsigned>(technique_class_for_submit(stack, submit)));
			out << line;
		}
	}

	// Opaque sort keys: depth grid x effect indexes x alpha-test
	// [orig: @ 0x5d928e..0x5d92c8].
	const float depths[] = { -8.0f, 0.0f, 15.0f, 16.0f, 255.0f, 256.0f,
	                         341.0f, 767.5f, 1023.0f, 4096.0f };
	const uint32_t effects[] = { 0, 1, 42, 63, 127 };
	for (float d : depths) {
		for (uint32_t e : effects) {
			for (int at = 0; at <= 1; ++at) {
				std::snprintf(line, sizeof(line), "okey d=%.1f e=%u at=%d key=%08x\n",
				              static_cast<double>(d), e, at,
				              opaque_sort_key(d, e, at != 0));
				out << line;
			}
		}
	}

	// Transparent sort keys: exact ~float-bits [orig: @ 0x5d931c..0x5d9326].
	const float tdepths[] = { 0.0f, 0.5f, 1.0f, 10.0f, 10.25f, 100.0f,
	                          500.0f, 8192.0f };
	for (float d : tdepths) {
		std::snprintf(line, sizeof(line), "tkey d=%.2f key=%08x\n",
		              static_cast<double>(d), transparent_sort_key(d));
		out << line;
	}

	// Water bracket: queue pick + rung, both camera sides
	// [orig: @ 0x5d934e..0x5d9359; @ 0x5c9596; @ 0x5c967a].
	const float heights[] = { -4.0f, 4.99f, 5.0f, 5.01f, 64.0f };
	for (float h : heights) {
		const TransparentQueue q = transparent_queue_for(h, 5.0f);
		std::snprintf(line, sizeof(line),
		              "side h=%.2f w=5.00 q=%s rungAbove=%d rungBelow=%d\n",
		              static_cast<double>(h),
		              q == TransparentQueue::AboveWater ? "above" : "below",
		              transparent_rung_for(q, true),
		              transparent_rung_for(q, false));
		out << line;
	}

	// The ladder itself.
	std::snprintf(line, sizeof(line),
	              "ladder stars=%d body=%d far=%d water=%d cam=%d fx=%d glow=%d\n",
	              kRungSkyStars, kRungSkyBody, kRungAlphaFarSide, kRungWater,
	              kRungAlphaCameraSide, kRungOverlayFx, kRungSunGlow);
	out << line;

	// Section 4 (REN-4): uv-anim vectors over libs/renderer/uv_anim — the
	// time-driven MatTexCoord1 transform
	// [orig: compute_uv_transform_matrix @ 0x5b1990; wave_lookup @ 0x5de6b0;
	//  bound in apply_shader_parameters @ 0x58db80 with
	//  time = (tick_ms << 8)/1000]. docs/render/render-material-re.md.
	out << "# uv-anim v1 (REN-4: scroll/rotate/waveform/controlled channels)\n";

	// Wave lookup bands: every type over a phase sweep (type 6 uses the
	// caller-supplied rand — pinned constant here).
	const uint8_t wave_types[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xF };
	const uint16_t wave_phases[] = { 0x0000, 0x0180, 0x4000, 0x7FC0, 0x8000,
	                                 0xC132, 0xFF80 };
	for (uint8_t wt : wave_types) {
		for (uint16_t ph : wave_phases) {
			std::snprintf(line, sizeof(line), "wave t=%02x ph=%04x v=%d\n",
			              wt, ph, renderer::uv_anim_wave_lookup(wt, ph, 0x1234));
			out << line;
		}
	}

	// Channel transforms: one channel active per row (the other identity),
	// covering every mode family at two times.
	struct UvCase {
		const char *label;
		renderer::UvAnimChannel ch;
	};
	const UvCase uv_cases[] = {
		{ "scroll+", { 16, 0x20, 3, 0, 0 } },
		{ "scroll-", { 17, 0x00, 5, 0, 0 } },
		{ "rot+", { 32, 0x40, 2, 0, 0 } },
		{ "rot-", { 33, 0x00, 7, 0, 0 } },
		{ "wave-set", { 0x32, 0x10, 4, 64, 320 } },     // mode 0x30, sine band
		{ "wave-scroll", { 0x44, 0x00, 6, -128, 128 } },// mode 0x40, band 4
		{ "wave-shear", { 0x57, 0x08, 2, 0, 256 } },    // mode 0x50, lerped band 7
		{ "wave-scale", { 0x61, 0x00, 3, 128, 512 } },  // mode 0x60, band 1
		{ "ctrl-set", { 'q', 2, 0, 0, 256 } },
		{ "ctrl-scroll", { 'r', 1, 0, -64, 192 } },
		{ "ctrl-shear", { 's', 0, 0, 0, 128 } },
		{ "ctrl-scale", { 't', 3, 0, 128, 384 } },
		{ "ctrl-rot", { 'u', 0, 0, 0, 64 } },
	};
	const uint16_t uv_times[] = { 0x0000, 0x2B67 };
	const renderer::UvAnimChannel identity{};
	for (const UvCase &c : uv_cases) {
		for (uint16_t tm : uv_times) {
			const renderer::UvAnimTransform tu =
				renderer::uv_anim_transform(
						c.ch, identity, tm, 0x8000, 0, 0x0741, 0x0741);
			const renderer::UvAnimTransform tv =
				renderer::uv_anim_transform(
						identity, c.ch, tm, 0, 0x8000, 0x0741, 0x0741);
			std::snprintf(line, sizeof(line),
			              "uvU %-11s t=%04x m=[%.6f %.6f %.6f %.6f %.6f %.6f]\n",
			              c.label, tm,
			              static_cast<double>(tu.m00), static_cast<double>(tu.m01),
			              static_cast<double>(tu.m10), static_cast<double>(tu.m11),
			              static_cast<double>(tu.m20), static_cast<double>(tu.m21));
			out << line;
			std::snprintf(line, sizeof(line),
			              "uvV %-11s t=%04x m=[%.6f %.6f %.6f %.6f %.6f %.6f]\n",
			              c.label, tm,
			              static_cast<double>(tv.m00), static_cast<double>(tv.m01),
			              static_cast<double>(tv.m10), static_cast<double>(tv.m11),
			              static_cast<double>(tv.m20), static_cast<double>(tv.m21));
			out << line;
		}
	}

	// Section 5 (REN-5): lighting scalars over libs/renderer/light_runtime —
	// the witnessed world-lighting chain (docs/render/render-lighting-re.md):
	// the modulator /64 unpack [orig: @ 0x58db30; @ 0x5aaef0], the world block
	// build + store derivations [orig: @ 0x5c8090; @ 0x5d89e0], the per-entity
	// uniforms incl. the interior daylight lerp [orig: @ 0x5d98a0], the FF
	// vertex-light evaluation (hemisphere delta lights [orig: @ 0x5d8cb0]),
	// the sun-visibility factor [orig: @ 0x5c6800], point-light color/
	// attenuation [orig: @ 0x5a9180; @ 0x5aa450], and the terrain c0/c1
	// surface light [orig: @ 0x604420; @ 0x604ee0].
	out << "# lighting v1 (REN-5: modulator scale, world block, entity uniforms, terrain c0/c1)\n";

	const uint32_t mod_packeds[] = { 0x404040u, 0x000000u, 0xFFFFFFu, 0x203040u };
	for (uint32_t m : mod_packeds) {
		const auto s = renderer::unpack_modulator_scale(m);
		std::snprintf(line, sizeof(line), "modscale m=%06x s=[%.6f %.6f %.6f]\n",
		              m, static_cast<double>(s[0]), static_cast<double>(s[1]),
		              static_cast<double>(s[2]));
		out << line;
	}

	const auto print_block = [&](const char *label, const renderer::WorldLightingBlock &b) {
		std::snprintf(line, sizeof(line),
		              "wblock %-6s en=%d dir=[%.6f %.6f %.6f] dc=[%.6f %.6f %.6f] "
		              "sky=[%.6f %.6f %.6f] gnd=[%.6f %.6f %.6f] flr=[%.6f %.6f %.6f] "
		              "ceil=[%.6f %.6f %.6f] iamb=[%.6f %.6f %.6f] oamb=[%.6f %.6f %.6f]\n",
		              label, b.dir_enabled ? 1 : 0,
		              static_cast<double>(b.dir[0]), static_cast<double>(b.dir[1]),
		              static_cast<double>(b.dir[2]),
		              static_cast<double>(b.dir_color[0]), static_cast<double>(b.dir_color[1]),
		              static_cast<double>(b.dir_color[2]),
		              static_cast<double>(b.hemi_sky[0]), static_cast<double>(b.hemi_sky[1]),
		              static_cast<double>(b.hemi_sky[2]),
		              static_cast<double>(b.hemi_ground[0]), static_cast<double>(b.hemi_ground[1]),
		              static_cast<double>(b.hemi_ground[2]),
		              static_cast<double>(b.floor_color[0]), static_cast<double>(b.floor_color[1]),
		              static_cast<double>(b.floor_color[2]),
		              static_cast<double>(b.ceiling_color[0]), static_cast<double>(b.ceiling_color[1]),
		              static_cast<double>(b.ceiling_color[2]),
		              static_cast<double>(b.indoor_ambient[0]), static_cast<double>(b.indoor_ambient[1]),
		              static_cast<double>(b.indoor_ambient[2]),
		              static_cast<double>(b.outdoor_ambient[0]), static_cast<double>(b.outdoor_ambient[1]),
		              static_cast<double>(b.outdoor_ambient[2]));
		out << line;
	};

	renderer::WorldLightingInputs win;
	win.light_packed = 0xFFF0E0u;
	win.sky_packed = 0x8090A0u;
	win.ground_packed = 0x605040u;
	win.ceiling_packed = 0x404850u;
	win.floor_packed = 0x302820u;
	win.light_dir = { 0.3f, -0.8f, 0.5f };
	const renderer::WorldLightingBlock day = renderer::build_world_lighting(win);
	print_block("day", day);

	renderer::WorldLightingInputs win_nvg = win;
	win_nvg.nvg_hemi_rewrite = true;
	win_nvg.nvg_level = 3;
	win_nvg.modulator_packed = 0x404040u;
	print_block("nvg", renderer::build_world_lighting(win_nvg));

	renderer::WorldLightingInputs win_scope = win;
	win_scope.vehicle_scope_grey = true;
	print_block("scope", renderer::build_world_lighting(win_scope));

	renderer::WorldLightingInputs win_dim = win;
	win_dim.nvg_world_dim = true;
	print_block("nvgdim", renderer::build_world_lighting(win_dim));

	const auto print_uniforms = [&](const char *label, const renderer::EntityLightingUniforms &u) {
		std::snprintf(line, sizeof(line),
		              "euni %-10s dc=[%.6f %.6f %.6f] gnd=[%.6f %.6f %.6f] "
		              "sky=[%.6f %.6f %.6f] amb=[%.6f %.6f %.6f]\n",
		              label,
		              static_cast<double>(u.dir_color[0]), static_cast<double>(u.dir_color[1]),
		              static_cast<double>(u.dir_color[2]),
		              static_cast<double>(u.hemi_ground[0]), static_cast<double>(u.hemi_ground[1]),
		              static_cast<double>(u.hemi_ground[2]),
		              static_cast<double>(u.hemi_sky[0]), static_cast<double>(u.hemi_sky[1]),
		              static_cast<double>(u.hemi_sky[2]),
		              static_cast<double>(u.ambient[0]), static_cast<double>(u.ambient[1]),
		              static_cast<double>(u.ambient[2]));
		out << line;
	};
	const renderer::EntityLightingUniforms outdoor_full =
		renderer::compute_entity_lighting(day, 1.0f, false, 0.0f);
	print_uniforms("out-1.0", outdoor_full);
	print_uniforms("out-0.5", renderer::compute_entity_lighting(day, 0.5f, false, 0.0f));
	print_uniforms("int-0.0", renderer::compute_entity_lighting(day, 1.0f, true, 0.0f));
	print_uniforms("int-0.25", renderer::compute_entity_lighting(day, 1.0f, true, 0.25f));
	print_uniforms("int-1.0", renderer::compute_entity_lighting(day, 1.0f, true, 1.0f));

	const std::array<float, 3> normals[] = {
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.70710678f, 0.0f, 0.70710678f },
		{ 0.0f, 0.0f, 1.0f },
	};
	const std::array<float, 3> to_light = { -day.dir[0], -day.dir[1], -day.dir[2] };
	for (const auto &n : normals) {
		const auto lit = renderer::ff_vertex_light(outdoor_full, n, to_light);
		std::snprintf(line, sizeof(line),
		              "fflit n=[%.4f %.4f %.4f] lit=[%.6f %.6f %.6f] x2=[%.6f %.6f %.6f]\n",
		              static_cast<double>(n[0]), static_cast<double>(n[1]),
		              static_cast<double>(n[2]),
		              static_cast<double>(lit[0]), static_cast<double>(lit[1]),
		              static_cast<double>(lit[2]),
		              static_cast<double>(lit[0] * renderer::kFFModulate2x),
		              static_cast<double>(lit[1] * renderer::kFFModulate2x),
		              static_cast<double>(lit[2] * renderer::kFFModulate2x));
		out << line;
	}

	for (int blocked = 0; blocked <= 4; ++blocked) {
		std::snprintf(line, sizeof(line), "sunvis blocked=%d f=%.6f\n",
		              blocked, static_cast<double>(renderer::sun_visibility_factor(blocked)));
		out << line;
	}

	const std::array<float, 3> pl_rgb = { 1.0f, 0.5f, 0.25f };
	const std::array<float, 3> pl_mod = renderer::unpack_modulator_scale(0x203040u);
	for (int d3d = 0; d3d <= 1; ++d3d) {
		const auto c = renderer::point_light_color(pl_rgb, 2.0f, pl_mod, d3d != 0);
		std::snprintf(line, sizeof(line), "plcolor d3d=%d c=[%.6f %.6f %.6f]\n",
		              d3d, static_cast<double>(c[0]), static_cast<double>(c[1]),
		              static_cast<double>(c[2]));
		out << line;
	}
	const int32_t pl_ranges[] = { 0x10000, 0xA0000, 0x400000 };
	for (int32_t r : pl_ranges) {
		const auto a4 = renderer::point_light_attenuation(r);
		std::snprintf(line, sizeof(line), "platten r=%08x a=[%.6f %.6f %.6f %.6f]\n",
		              static_cast<unsigned>(r),
		              static_cast<double>(a4[0]), static_cast<double>(a4[1]),
		              static_cast<double>(a4[2]), static_cast<double>(a4[3]));
		out << line;
	}

	const std::array<float, 3> trn_light = { 0.9f, 0.8f, 0.7f };
	const std::array<float, 3> trn_sky = { 0.2f, 0.25f, 0.3f };
	const float masks[] = { 0.0f, 0.5f, 1.0f };
	for (float m : masks) {
		const auto lit = renderer::terrain_surface_light(m, trn_light, trn_sky);
		std::snprintf(line, sizeof(line), "trnlit mask=%.2f lit=[%.6f %.6f %.6f]\n",
		              static_cast<double>(m),
		              static_cast<double>(lit[0]), static_cast<double>(lit[1]),
		              static_cast<double>(lit[2]));
		out << line;
	}

	return out.str();
}

} // namespace

int main() {
	const std::string generated = generate();
	const char *golden_path = RENDER_STATE_VECTORS_GOLDEN;

	if (std::getenv("OPENNOVA_RENDER_VECTORS_DUMP") != nullptr) {
		std::ofstream out(golden_path, std::ios::binary);
		if (!out) {
			std::cerr << "FAIL: dump mode could not open golden for write: "
			          << golden_path << '\n';
			return 1;
		}
		out << generated;
		out.close();
		std::cerr <<
			"================ RENDER VECTOR DUMP ================\n"
			"Golden rewritten at " << golden_path << " (" <<
			generated.size() << " bytes).\n"
			"THIS RUN IS DELIBERATELY RED. Re-dumping is only legitimate\n"
			"when the producer chain changed on purpose, and the commit\n"
			"that re-dumps must carry the witness citation\n"
			"([orig: Name @ 0xADDR] / D-RMAT row) for the change (ADR 0023).\n"
			"====================================================\n";
		return 1;
	}

	std::ifstream in(golden_path, std::ios::binary);
	if (!in) {
		std::cerr << "FAIL: golden missing: " << golden_path << "\n"
		          << "Initial dump: set OPENNOVA_RENDER_VECTORS_DUMP=1 and rerun.\n";
		return 1;
	}
	std::stringstream buf;
	buf << in.rdbuf();
	const std::string golden = buf.str();

	if (golden == generated) {
		std::cerr << "renderer_state_vectors ok ("
		          << generated.size() << " bytes)\n";
		return 0;
	}

	// Report the first differing lines compactly, then the policy.
	std::istringstream a(golden), b(generated);
	std::string la, lb;
	int line_no = 0, reported = 0;
	while (reported < 8) {
		const bool has_a = static_cast<bool>(std::getline(a, la));
		const bool has_b = static_cast<bool>(std::getline(b, lb));
		if (!has_a && !has_b)
			break;
		++line_no;
		if (!has_a) la.clear();
		if (!has_b) lb.clear();
		if (la != lb) {
			std::cerr << "line " << line_no << ":\n  golden:    "
			          << (la.empty() ? "<eof>" : la) << "\n  generated: "
			          << (lb.empty() ? "<eof>" : lb) << '\n';
			++reported;
		}
	}
	std::cerr <<
		"FAIL: render-state vectors diverged from the committed golden.\n"
		"If this was NOT a deliberate witnessed port step, it is a\n"
		"regression: fix the code, never the golden. A deliberate change\n"
		"re-dumps (OPENNOVA_RENDER_VECTORS_DUMP=1) and cites its witness in\n"
		"the same commit (ADR 0023; docs/render/).\n";
	return 1;
}
