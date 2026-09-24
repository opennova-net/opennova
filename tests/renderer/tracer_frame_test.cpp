// Pins the witnessed tracer style tables and the ribbon compile
// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0; style build
// CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3A0; the beam build
// Render_DrawTrailOrBeamSegments @ 0x5DCB80].

#include <runtime/renderer/tracer_frame.h>
#include <runtime/renderer/render_order.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
namespace r = opennova::renderer;

bool check(bool ok, const char *message) {
	if (!ok) {
		std::fputs(message, stderr);
		std::fputc(10, stderr);
	}
	return ok;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

bool vertex_at(const r::TracerVertex &v, float x, float y, float z,
		float epsilon = 0.0001f) {
	return near(v.x, x, epsilon) && near(v.y, y, epsilon) && near(v.z, z, epsilon);
}

// The camera on +Z looking straight down, a 90-degree horizontal fov.
r::TracerView top_down_view(float height = 10.0f) {
	r::TracerView view;
	view.camera = {0.0f, 0.0f, height};
	view.forward = {0.0f, 0.0f, -1.0f};
	view.projection_x_scale = 1.0f;
	view.tick_ms = 1000;
	return view;
}

bool style_table_contract() {
	// stdred is the id-1 block AND the fallback for id 0 / out-of-range ids
	// [orig: the CEffectChannel_Init default case @ 0x5db1c8].
	const auto &stdred = r::tracer_style(1);
	if (!check(r::tracer_style(0).colors == stdred.colors &&
			r::tracer_style(13).colors == stdred.colors &&
			r::tracer_style(99).colors == stdred.colors &&
			r::tracer_style(-1).colors == stdred.colors,
			"unknown tracer_type ids take the stdred block")) return false;
	if (!check(stdred.color_count == 12 && stdred.colors[1] == 0xE08080 &&
			stdred.colors[6] == 0x200000 && stdred.fog_black &&
			!stdred.cross_section && !stdred.distortion &&
			stdred.shader == r::TracerShader::Stock &&
			stdred.base_argb == 0 && stdred.size_count == 1 &&
			near(stdred.sizes[0], 0.02f),
			"stdred: 12-entry ramp, width 0.02, stock additive, fog to black"))
		return false;
	if (!check(r::tracer_style(2).colors[1] == 0x80A080,
			"stdgreen swaps the ramp to the green channel")) return false;

	// Rocket/AT4 smoke: quadratic alpha fade a = ((255-2i)^2) >> 8 over 112
	// grays; linear width growth [orig: @ 0x5db4f8-0x5db5f6]; the smoke
	// material, the cross-section, the wave words and the distortion flag
	// [orig: g_TracerStyle_Rocket @ 0x2BF4A40, g_TracerStyle_AT4 @ 0x2BF4210].
	const auto &rocket = r::tracer_style(3);
	const auto &at4 = r::tracer_style(4);
	if (!check(rocket.color_count == 112 && !rocket.fog_black &&
			rocket.base_argb == 0xC0C0C0 &&
			(rocket.colors[0] >> 24) == 254 && (rocket.colors[0] & 0xFFFFFF) == 0xC0C0C0 &&
			(rocket.colors[111] >> 24) == 4,
			"rocket smoke: 112-entry quadratic fade over gray")) return false;
	if (!check(rocket.size_count == 32 && near(rocket.sizes[16], 1.0f) &&
			at4.size_count == 32 && near(at4.sizes[16], 0.5f) &&
			at4.colors == rocket.colors,
			"rocket widths grow at 0.0625/entry, at4 at 0.03125, shared ramp")) return false;
	if (!check(rocket.cross_section && rocket.distortion &&
			rocket.shader == r::TracerShader::Smoke &&
			near(rocket.wave_u, 1.0f) && near(rocket.wave_v, 1.0f) &&
			near(rocket.wave_t, 1.0f) && at4.cross_section && at4.distortion &&
			near(at4.wave_u, 2.0f) && near(at4.wave_v, 1.5f) && near(at4.wave_t, 1.0f),
			"rocket/at4: smoke cross-section with the witnessed wave words, distortion on"))
		return false;

	// Grenade: cubic alpha fade a = (192 * (255-3i)^3) >> 24 over 64 grays
	// [orig: @ 0x5db648]; no distortion pass [orig: @ 0x2BF4208 zero].
	const auto &grenade = r::tracer_style(5);
	if (!check(grenade.color_count == 64 && !grenade.fog_black &&
			(grenade.colors[0] >> 24) == 189 && (grenade.colors[63] >> 24) == 3 &&
			grenade.size_count == 32 && near(grenade.sizes[8], 0.0625f) &&
			grenade.cross_section && !grenade.distortion &&
			grenade.shader == r::TracerShader::Smoke && near(grenade.wave_u, 8.0f) &&
			near(grenade.wave_v, 4.0f),
			"grenade smoke: 64-entry cubic fade, width 0.0078125/entry, wave 8/4/1"))
		return false;

	// Rapid: the short 6-entry ramps; NVG (id 8): 0xFF2020, alpha UP 6/entry
	// [orig: @ 0x5db6f7-0x5db711], its own material, fog to black.
	const auto &nvg = r::tracer_style(8);
	if (!check(r::tracer_style(6).color_count == 6 &&
			r::tracer_style(7).colors[1] == 0x80A080 &&
			nvg.color_count == 32 && (nvg.colors[31] >> 24) == 186 &&
			(nvg.colors[31] & 0xFFFFFF) == 0xFF2020 && nvg.base_argb == 0xC04040 &&
			nvg.fog_black && nvg.cross_section && !nvg.distortion &&
			nvg.shader == r::TracerShader::NvgLaser && near(nvg.wave_t, 0.1f),
			"rapid ramps are 6 entries; the NVG laser ramps up over red")) return false;

	// Sniper: width 0.04 -> 0.1 over 10 [orig: .data @ 0x8458C8]; the static
	// block's +4 and +0x828 words are set, the material is the stock slot
	// [orig: g_TracerStyle_SniperRed @ 0x8458B0]. DF1: 32-entry fades at
	// constant width 0.006 [orig: @ 0x5db3d9-0x5db4d6].
	const auto &sniper = r::tracer_style(9);
	if (!check(sniper.color_count == 20 && sniper.colors[0] == 0xFF180000 &&
			sniper.size_count == 10 && near(sniper.sizes[0], 0.04f) &&
			near(sniper.sizes[9], 0.1f) &&
			r::tracer_style(10).colors[4] == 0xF0000700 && sniper.cross_section &&
			sniper.distortion && sniper.shader == r::TracerShader::Stock &&
			near(sniper.wave_u, 0.0f),
			"sniper ramps, the 0.04->0.1 width curve, the distortion flag")) return false;
	const auto &df1_red = r::tracer_style(11);
	return check(df1_red.color_count == 32 && df1_red.colors[0] == 0 &&
			df1_red.colors[1] == 0x7C3E3E &&
			r::tracer_style(12).colors[1] == 0x3E7C3E &&
			near(df1_red.sizes[0], 0.006f) && !df1_red.cross_section,
			"df1 fades and the thin constant width");
}

bool strip_geometry_contract() {
	// Three points along +X under a top-down camera: the side vector is
	// normalize(segment x eye ray) in this frame (retail's eye ray x segment in
	// its Y-negated frame), here +Y.
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channel{1, 0, 3, points};
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(&channel, 1, top_down_view(), r::TracerPass::Main, frame);
	if (!check(frame.channels == 1 && frame.vertices.size() == 4 &&
			frame.draws.size() == 1 && frame.indices.size() == 6 &&
			frame.draws[0].shader == r::TracerShader::Stock &&
			frame.draws[0].fog_black,
			"points [0..count-2] emit one +/- pair each; the strip as two triangles"))
		return false;
	// Pair 0 at the oldest point: half-width max(0.02, 10 x 0.0012) = 0.02;
	// the oldest pair takes the style base colour (stdred base 0).
	if (!check(vertex_at(frame.vertices[0], 0.0f, 0.02f, 0.0f) &&
			vertex_at(frame.vertices[1], 0.0f, -0.02f, 0.0f),
			"the +rs vertex leads, on the witnessed side")) return false;
	if (!check(frame.vertices[0].argb == 0 && frame.vertices[2].argb == 0xE08080,
			"the oldest pair takes the base colour; ramp index (count - i) + age - 1"))
		return false;
	if (!check(frame.vertices[0].u0 == 0.0f && frame.vertices[0].v1 == 0.0f,
			"the strip writes no texture coordinates")) return false;
	const std::uint32_t expected[6] = {0, 1, 2, 1, 2, 3};
	for (int k = 0; k < 6; ++k)
		if (!check(frame.indices[static_cast<std::size_t>(k)] == expected[k],
				"the strip expands to (k, k+1, k+2)")) return false;

	// Age shifts the same ramp (the post-death fade): age 5 -> index 6 -> 0x200000.
	r::TracerChannelInput aged = channel;
	aged.age = 5;
	r::compile_tracer_ribbons(&aged, 1, top_down_view(), r::TracerPass::Main, frame);
	if (!check(frame.vertices[2].argb == 0x200000,
			"age advances the ramp index (the fade half)")) return false;

	// A two-point channel builds one pair, which the strip draw (four vertices
	// minimum) never draws [orig: @ 0x5DC8CB..0x5DC8D8].
	r::TracerChannelInput two{1, 0, 2, points};
	r::compile_tracer_ribbons(&two, 1, top_down_view(), r::TracerPass::Main, frame);
	return check(frame.channels == 1 && frame.vertices.empty() && frame.draws.empty(),
			"a two-point channel counts but draws nothing");
}

bool minimum_width_contract() {
	// The minimum half-width divides by the projection's _11: at 100 u under a
	// 90-degree fov (_11 = 1) it is 100 x 65536 x 1.831e-8 = 0.12; through a 4x
	// scope (_11 = 4) a quarter of that [orig: @ 0x5DC0D6..0x5DC0F7].
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channel{1, 0, 3, points};
	r::TracerRibbonFrame frame;
	r::TracerView view = top_down_view(100.0f);
	r::compile_tracer_ribbons(&channel, 1, view, r::TracerPass::Main, frame);
	if (!check(near(frame.vertices[0].y, 0.12f, 0.0005f),
			"far points take the distance-proportional min width")) return false;
	view.projection_x_scale = 4.0f;
	r::compile_tracer_ribbons(&channel, 1, view, r::TracerPass::Main, frame);
	if (!check(near(frame.vertices[0].y, 0.03f, 0.0005f),
			"a narrower fov shrinks the min width by 1/_11")) return false;
	view.projection_x_scale = 10.0f;
	r::compile_tracer_ribbons(&channel, 1, view, r::TracerPass::Main, frame);
	return check(near(frame.vertices[0].y, 0.02f),
			"the style width wins once the min width falls under it");
}

bool screen_parallel_plane_contract() {
	// The side vector loses its component along the view direction: segment
	// x eye ray = +Y here, the tilted view (0, 0.6, -0.8) takes 0.6 of it out,
	// leaving (0, 0.64, 0.48) -> (0, 0.8, 0.6) [orig: @ 0x5DC34F..0x5DC3CE].
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channel{1, 0, 3, points};
	r::TracerRibbonFrame frame;
	r::TracerView view = top_down_view();
	view.forward = {0.0f, 0.6f, -0.8f};
	r::compile_tracer_ribbons(&channel, 1, view, r::TracerPass::Main, frame);
	if (!check(vertex_at(frame.vertices[0], 0.0f, 0.016f, 0.012f) &&
			vertex_at(frame.vertices[1], 0.0f, -0.016f, -0.012f),
			"the side vector is projected into the screen plane")) return false;
	// A repeated point has no segment: the side vector is zero and the pair
	// collapses onto the point [orig: the zero branch @ 0x5DC2DB].
	const float repeated[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput stalled{1, 0, 3, repeated};
	r::compile_tracer_ribbons(&stalled, 1, top_down_view(), r::TracerPass::Main, frame);
	return check(vertex_at(frame.vertices[0], 0.0f, 0.0f, 0.0f) &&
			vertex_at(frame.vertices[1], 0.0f, 0.0f, 0.0f),
			"a zero-length segment draws a zero-width pair");
}

bool cross_section_contract() {
	// A rocket channel: four vertices per point, the base colour outside, the
	// ramp inside, the scrolling texture layers [orig: @ 0x5DC4F6..0x5DC796].
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channel{3, 0, 3, points};
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(&channel, 1, top_down_view(), r::TracerPass::Main, frame);
	if (!check(frame.vertices.size() == 8 && frame.indices.size() == 18 &&
			frame.draws.size() == 1 && frame.draws[0].shader == r::TracerShader::Smoke &&
			!frame.draws[0].fog_black,
			"two cross-sections, one six-triangle bridge, the smoke material"))
		return false;
	// Point 0: size[2] = 0.125 beats the 0.012 min width; the inner pair sits
	// at 0.4 of it.
	const auto &v = frame.vertices;
	if (!check(vertex_at(v[0], 0.0f, 0.125f, 0.0f) && vertex_at(v[1], 0.0f, 0.05f, 0.0f) &&
			vertex_at(v[2], 0.0f, -0.05f, 0.0f) && vertex_at(v[3], 0.0f, -0.125f, 0.0f),
			"the outer pair at +-rs, the inner pair at +-0.4 rs")) return false;
	// The oldest point is all base colour; point 1's inner pair rides ramp
	// index (3 - 1) + 0 - 1 = 1 (253^2 >> 8 = 250), its outer pair the base.
	if (!check(v[0].argb == 0x00C0C0C0u && v[3].argb == 0x00C0C0C0u &&
			v[1].argb == 0x00C0C0C0u && v[2].argb == 0x00C0C0C0u &&
			v[4].argb == 0x00C0C0C0u && v[7].argb == 0x00C0C0C0u &&
			v[5].argb == 0xFAC0C0C0u && v[6].argb == 0xFAC0C0C0u,
			"outer = the base colour, inner = the ramp")) return false;
	// T = 1000 ms, wave 1/1/1: phase 0.4, u offset 0.1 w + phase = 0.5,
	// A = 0.2 x 0.125, B = 0.2 x w; the segment is perpendicular to the eye
	// ray so the wobble is zero.
	if (!check(near(v[0].u0, 0.5f) && near(v[0].v0, -0.025f) && near(v[0].u1, 0.4f) &&
			near(v[0].v1, -0.2f) && near(v[1].u0, 0.5f) && near(v[1].v0, -0.01f) &&
			near(v[1].v1, -0.08f) && near(v[2].v0, 0.01f) && near(v[2].v1, 0.08f) &&
			near(v[3].v0, 0.025f) && near(v[3].v1, 0.2f),
			"the cross-section texture coordinates")) return false;
	// Point 1 steps u by 0.3 x wave_u and u1 by 0.11 x wave_u; its eye ray
	// leans along the segment (cos 0.0995) so the inner u carries
	// -0.1 x cos^4.
	const float cosine = 1.0f / std::sqrt(101.0f);
	const float wobble = -0.1f * cosine * cosine * cosine * cosine;
	if (!check(near(v[4].u0, 0.8f) && near(v[4].u1, 0.51f) &&
			near(v[5].u0, 0.8f + wobble, 0.000001f),
			"per-point u steps and the cos^4 wobble")) return false;
	const std::uint32_t expected[18] = {0, 1, 4, 1, 5, 4, 1, 2, 5, 2, 6, 5, 2, 3, 6, 3, 7, 6};
	for (int k = 0; k < 18; ++k)
		if (!check(frame.indices[static_cast<std::size_t>(k)] == expected[k],
				"the witnessed bridge index order")) return false;
	return true;
}

bool distortion_pass_contract() {
	// The distortion pass draws only the +0x828 styles: 1.2x wider, the inner
	// alpha 255 - ((255 - a)^3 >> 16), the outer pair the raw base, scene fog
	// [orig: @ 0x5DB8D4..0x5DC0B6; CEffectEmitterPool_RenderDistortionPass
	// @ 0x5DCB58].
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	const r::TracerChannelInput channels[2] = {{1, 0, 3, points}, {3, 0, 3, points}};
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(channels, 2, top_down_view(), r::TracerPass::Distortion,
			frame);
	if (!check(frame.channels == 1 && frame.draws.size() == 1 &&
			frame.draws[0].shader == r::TracerShader::Distortion &&
			!frame.draws[0].fog_black && frame.vertices.size() == 8,
			"stdred has no distortion flag; the rocket draws through the distortion material"))
		return false;
	const auto &v = frame.vertices;
	if (!check(vertex_at(v[0], 0.0f, 0.15f, 0.0f) && vertex_at(v[1], 0.0f, 0.06f, 0.0f),
			"the distortion ribbon is 1.2x wider")) return false;
	// Point 0's inner pair: base alpha 0 -> 255 - (255^3 >> 16) = 2; point 1's:
	// 0xFA -> 255 - (5^3 >> 16) = 255; the outer pairs keep the raw base.
	if (!check(v[1].argb == 0x02C0C0C0u && v[5].argb == 0xFFC0C0C0u &&
			v[0].argb == 0x00C0C0C0u && v[4].argb == 0x00C0C0C0u,
			"the inner alpha is re-derived, the outer pair keeps the raw base"))
		return false;
	// The texture terms keep the unwidened size: A = 0.2 x 0.125.
	return check(near(v[0].v0, -0.025f), "the texture terms use the style width");
}

bool beam_contract() {
	// The NVG laser beam: the NVG material, fog to black, the ramp anchored to
	// its end (age = 32 - 3) [orig: Render_DrawTrailOrBeamSegments @ 0x5DCC34].
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		0.25f, 0.0f, 0.0f, 1.0f,
		0.5f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerRibbonFrame frame;
	r::append_tracer_beam(points, 3, 8, top_down_view(), frame);
	if (!check(frame.draws.size() == 1 &&
			frame.draws[0].shader == r::TracerShader::NvgLaser &&
			frame.draws[0].fog_black && frame.vertices.size() == 8,
			"the beam draws the NVG cross-section")) return false;
	// Point 1: index (3 - 1) + 29 - 1 = 30 -> alpha 180.
	return check(frame.vertices[5].argb == ((180u << 24) | 0xFF2020u) &&
			frame.vertices[0].argb == 0xC04040u,
			"the beam ramp is anchored to the table's end");
}

bool per_channel_draw_contract() {
	// Each channel is its own draw, in pool order [orig: the slot walk
	// @ 0x5DCB17..0x5DCB2E].
	const float run_a[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	const r::TracerChannelInput channels[3] = {
			{1, 0, 3, run_a}, {3, 0, 3, run_a}, {11, 0, 3, run_a}};
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(channels, 3, top_down_view(), r::TracerPass::Main, frame);
	if (!check(frame.channels == 3 && frame.draws.size() == 3 &&
			frame.draws[0].shader == r::TracerShader::Stock &&
			frame.draws[1].shader == r::TracerShader::Smoke &&
			frame.draws[2].shader == r::TracerShader::Stock,
			"one draw per channel in pool order")) return false;
	if (!check(frame.draws[1].first_index == 6 && frame.draws[1].index_count == 18 &&
			frame.draws[1].first_vertex == 4 && frame.draws[1].vertex_count == 8 &&
			frame.draws[2].first_index == 24 && frame.draws[2].first_vertex == 12 &&
			frame.indices[24] == 12,
			"each draw indexes its own contiguous vertices")) return false;
	r::compile_tracer_ribbons(nullptr, 0, top_down_view(), r::TracerPass::Main, frame);
	return check(frame.channels == 0 && frame.vertices.empty() && frame.draws.empty(),
			"no channels compile to an empty frame");
}

bool rung_contract() {
	// The pool draws in the far-side slot while the eye is below the water and
	// in the camera-side slot at or above it [orig: CEffectEmitterPool_RenderMainPass
	// @ 0x5DCAF0; calls @ 0x5C95AC / @ 0x5C9687].
	return check(r::tracer_rung(false) == r::kRungTracerFarSide &&
			r::tracer_rung(true) == r::kRungTracerCameraSide &&
			r::kRungAlphaFarSide < r::kRungTracerFarSide &&
			r::kRungTracerFarSide < r::kRungWater &&
			r::kRungAlphaCameraSide < r::kRungTracerCameraSide,
			"the tracer rungs follow each side's alpha flush");
}

} // namespace

int main() {
	if (!style_table_contract()) return 1;
	if (!strip_geometry_contract()) return 1;
	if (!minimum_width_contract()) return 1;
	if (!screen_parallel_plane_contract()) return 1;
	if (!cross_section_contract()) return 1;
	if (!distortion_pass_contract()) return 1;
	if (!beam_contract()) return 1;
	if (!per_channel_draw_contract()) return 1;
	if (!rung_contract()) return 1;
	std::puts("renderer_tracer_frame_test ok");
	return 0;
}
