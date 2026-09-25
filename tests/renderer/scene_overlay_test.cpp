// The post-particle overlay tail (runtime/renderer/scene_overlay): the
// witnessed slot order of the main scene and the mirror, the per-view murk
// gate, and the four builders' pass states.
// [orig: Terrain_RenderSceneWithReflection @ 0x5c9695..0x5c9714;
//  Water_RenderReflectedWorldScene @ 0x5c85fd]

#include <runtime/renderer/scene_overlay.h>

#include <cstdio>

using namespace opennova::renderer;

static int failures = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++failures;                                                    \
		}                                                                  \
	} while (0)

namespace {

void test_the_main_scene_order_and_the_mirror_subset() {
	// NVG beams @ 0x5c9695 -> precipitation @ 0x5c96a6 -> coronas @ 0x5c96ad
	// -> glint @ 0x5c96c0 -> murk @ 0x5c96f5 -> glare @ 0x5c9714.
	CHECK(kSceneOverlayOrder[0] == SceneOverlaySlot::NvgLaserBeams);
	CHECK(kSceneOverlayOrder[1] == SceneOverlaySlot::Precipitation);
	CHECK(kSceneOverlayOrder[2] == SceneOverlaySlot::LightCoronas);
	CHECK(kSceneOverlayOrder[3] == SceneOverlaySlot::WaterGlint);
	CHECK(kSceneOverlayOrder[4] == SceneOverlaySlot::UnderwaterMurk);
	CHECK(kSceneOverlayOrder[5] == SceneOverlaySlot::SunGlare);
	// The mirror draws the coronas (@ 0x5c85fd), then render_main_scene's dim
	// (@ 0x5c186c) and the far-band sun/moon and glow redraw (@ 0x5c18fb,
	// @ 0x5c1904).
	CHECK(kMirrorOverlayOrder.size() == 4);
	CHECK(kMirrorOverlayOrder[0] == SceneOverlaySlot::LightCoronas);
	CHECK(kMirrorOverlayOrder[1] == SceneOverlaySlot::MirrorDim);
	CHECK(kMirrorOverlayOrder[2] == SceneOverlaySlot::MirrorCelestialBodies);
	CHECK(kMirrorOverlayOrder[3] == SceneOverlaySlot::MirrorSunGlow);

	// Submitted out of order, compiled in the witnessed order; within a slot
	// the submission order holds.
	SceneOverlayFrame frame;
	const float rgb[3] = {0.2f, 0.4f, 0.6f};
	const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float uv[6] = {0, 0, 1, 0, 0, 1};
	const float unit[3] = {1.0f, 1.0f, 1.0f};
	append_self_lum_overlay(SceneOverlaySlot::SunGlare, tri, uv, 3, rgb, unit, 1.0f, 7, frame);
	append_underwater_murk_overlay(rgb, 200, 5.0f, frame);
	append_self_lum_overlay(SceneOverlaySlot::WaterGlint, tri, uv, 3, rgb, unit, 1.0f, 7, frame);
	std::vector<LightCoronaQuad> quads(1);
	quads[0].half_size = 0.5f;
	append_corona_overlay(quads, 3, frame);
	append_corona_overlay(quads, 4, frame);
	PrecipitationDrawFrame rain;
	rain.drops = 1;
	rain.vertices = {0, 0, 0, 0.5f, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1};
	append_precipitation_overlay(rain, 2, frame);

	std::vector<SceneOverlayBatch> list;
	compile_scene_overlay(frame, kSceneOverlayOrder.data(), kSceneOverlayOrder.size(), list);
	CHECK(list.size() == 6);
	if (list.size() == 6) {
		CHECK(list[0].slot == SceneOverlaySlot::Precipitation);
		CHECK(list[1].slot == SceneOverlaySlot::LightCoronas && list[1].texture == 3);
		CHECK(list[2].slot == SceneOverlaySlot::LightCoronas && list[2].texture == 4);
		CHECK(list[3].slot == SceneOverlaySlot::WaterGlint);
		CHECK(list[4].slot == SceneOverlaySlot::UnderwaterMurk);
		CHECK(list[5].slot == SceneOverlaySlot::SunGlare);
	}
	compile_scene_overlay(frame, kMirrorOverlayOrder.data(), kMirrorOverlayOrder.size(), list);
	CHECK(list.size() == 2);
	for (const SceneOverlayBatch &batch : list)
		CHECK(batch.slot == SceneOverlaySlot::LightCoronas);
}

void test_the_murk_gate_includes_the_waterline() {
	// cmp [eye+0Ch], Env_WaterHeightFixed @ 0x5c96ca / jg @ 0x5c96d1.
	SceneOverlayFrame frame;
	const float rgb[3] = {0.1f, 0.2f, 0.3f};
	append_underwater_murk_overlay(rgb, 0x80 + 96, 10.0f, frame);
	CHECK(frame.batches.size() == 1);
	const SceneOverlayBatch &murk = frame.batches[0];
	CHECK(scene_overlay_view_draws(murk, 9.0f));
	CHECK(scene_overlay_view_draws(murk, 10.0f));
	CHECK(!scene_overlay_view_draws(murk, 10.01f));
	CHECK(murk.shading == SceneOverlayShading::FlatBlend);
	CHECK(murk.depth == SceneOverlayDepth::Always);
	CHECK(murk.geometry == SceneOverlayGeometry::Screen);
	CHECK(murk.vertex_count == 6);
	CHECK(frame.vertices[0].color[3] == 224.0f / 255.0f);
	CHECK(frame.vertices[0].color[0] == 0.1f);
	// Every other batch draws in every view that admits its slot.
	std::vector<LightCoronaQuad> quads(1);
	append_corona_overlay(quads, 0, frame);
	CHECK(scene_overlay_view_draws(frame.batches[1], 1000.0f));
}

void test_the_builders_carry_the_pass_states() {
	SceneOverlayFrame frame;
	// Precipitation: MODULATE2X under SRCALPHA/INVSRCALPHA, z-tested, one
	// diffuse for every streak.
	PrecipitationDrawFrame rain;
	rain.drops = 2;
	rain.color_argb = 0xFF804020u;
	for (int i = 0; i < 6; ++i) {
		const float v[5] = {float(i), 1.0f, 2.0f, 0.5f, 1.0f};
		rain.vertices.insert(rain.vertices.end(), v, v + 5);
	}
	append_precipitation_overlay(rain, 9, frame);
	CHECK(frame.batches.size() == 1);
	CHECK(frame.batches[0].shading == SceneOverlayShading::Modulate2xBlend);
	CHECK(frame.batches[0].depth == SceneOverlayDepth::TestNoWrite);
	CHECK(frame.batches[0].vertex_count == 6);
	CHECK(frame.vertices[5].position[0] == 5.0f);
	CHECK(frame.vertices[0].color[0] == 128.0f / 255.0f);
	CHECK(frame.vertices[0].color[2] == 32.0f / 255.0f);
	CHECK(frame.vertices[0].color[3] == 1.0f);
	// A dry frame adds nothing.
	PrecipitationDrawFrame dry;
	append_precipitation_overlay(dry, 9, frame);
	CHECK(frame.batches.size() == 1);

	// Coronas: camera-facing billboards around the render-frame centre.
	std::vector<LightCoronaQuad> quads(1);
	quads[0].center = {1.0f, 2.0f, 3.0f};
	quads[0].half_size = 0.25f;
	quads[0].rgb = {0.5f, 0.25f, 0.125f};
	append_corona_overlay(quads, 1, frame);
	const SceneOverlayBatch &corona = frame.batches[1];
	CHECK(corona.shading == SceneOverlayShading::AdditiveModulate);
	CHECK(corona.depth == SceneOverlayDepth::TestNoWrite);
	CHECK(corona.geometry == SceneOverlayGeometry::Billboard);
	CHECK(corona.vertex_count == 6);
	const SceneOverlayVertex &c0 = frame.vertices[corona.first_vertex];
	CHECK(c0.position[0] == 1.0f && c0.position[1] == 3.0f && c0.position[2] == -2.0f);
	CHECK(c0.corner[0] == -0.25f && c0.corner[1] == -0.25f);
	CHECK(c0.color[0] == 0.5f && c0.color[1] == 0.25f && c0.color[2] == 0.125f);

	// SELFLUM: sat(SelfLumColor x gain) as the diffuse, the fog as its alpha,
	// ZFUNC ALWAYS.
	const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float uv[6] = {0, 0, 1, 0, 0, 1};
	const float self_lum[3] = {1.0f, 200.0f / 255.0f, 108.0f / 255.0f};
	const float scale[3] = {2.0f, 2.0f, 2.0f};
	append_self_lum_overlay(SceneOverlaySlot::SunGlare, tri, uv, 3, self_lum, scale, 0.75f, 4,
			frame);
	const SceneOverlayBatch &glare = frame.batches[2];
	CHECK(glare.shading == SceneOverlayShading::SelfLumAdditive);
	CHECK(glare.depth == SceneOverlayDepth::Always);
	const SceneOverlayVertex &g0 = frame.vertices[glare.first_vertex];
	CHECK(g0.color[0] == 1.0f);
	CHECK(g0.color[1] == 1.0f);
	CHECK(g0.color[2] == 216.0f / 255.0f);
	CHECK(g0.color[3] == 0.75f);
	CHECK(kSunGlareLightScale == 1.0f);
}

void test_the_mirror_closes_with_the_dim_and_the_far_band_redraw() {
	// The dim: one viewport quad under 0xFF404040, DESTCOLOR / ZERO, ALWAYS
	// [orig: render_main_scene @ 0x5c1856..0x5c189e].
	SceneOverlayFrame frame;
	const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float uv[6] = {0, 0, 1, 0, 0, 1};
	const float rgb[3] = {0.5f, 0.5f, 0.5f};
	const float unit[3] = {1.0f, 1.0f, 1.0f};
	append_self_lum_overlay(SceneOverlaySlot::MirrorSunGlow, tri, uv, 3, rgb, unit, 1.0f, 1,
			frame, SceneOverlayDepth::FarBand);
	append_self_lum_overlay(SceneOverlaySlot::MirrorCelestialBodies, tri, uv, 3, rgb, unit, 1.0f,
			2, frame, SceneOverlayDepth::FarBand);
	append_mirror_dim_overlay(64.0f / 255.0f, frame);
	std::vector<LightCoronaQuad> quads(1);
	quads[0].half_size = 0.5f;
	append_corona_overlay(quads, 3, frame);
	std::vector<SceneOverlayBatch> list;
	compile_scene_overlay(frame, kMirrorOverlayOrder.data(), kMirrorOverlayOrder.size(), list);
	CHECK(list.size() == 4);
	if (list.size() == 4) {
		CHECK(list[0].slot == SceneOverlaySlot::LightCoronas);
		CHECK(list[1].slot == SceneOverlaySlot::MirrorDim);
		CHECK(list[1].shading == SceneOverlayShading::DimMultiply);
		CHECK(list[1].depth == SceneOverlayDepth::Always);
		CHECK(list[1].geometry == SceneOverlayGeometry::Screen);
		CHECK(list[1].vertex_count == 6);
		const SceneOverlayVertex &dim = frame.vertices[list[1].first_vertex];
		CHECK(dim.color[0] == 64.0f / 255.0f && dim.color[1] == 64.0f / 255.0f &&
				dim.color[2] == 64.0f / 255.0f && dim.color[3] == 1.0f);
		CHECK(list[2].slot == SceneOverlaySlot::MirrorCelestialBodies);
		CHECK(list[2].depth == SceneOverlayDepth::FarBand);
		CHECK(list[2].shading == SceneOverlayShading::SelfLumAdditive);
		CHECK(list[3].slot == SceneOverlaySlot::MirrorSunGlow);
		CHECK(list[3].depth == SceneOverlayDepth::FarBand);
	}
	// The main scene never draws the mirror's closing slots.
	compile_scene_overlay(frame, kSceneOverlayOrder.data(), kSceneOverlayOrder.size(), list);
	CHECK(list.size() == 1);
}

} // namespace

int main() {
	test_the_main_scene_order_and_the_mirror_subset();
	test_the_mirror_closes_with_the_dim_and_the_far_band_redraw();
	test_the_murk_gate_includes_the_waterline();
	test_the_builders_carry_the_pass_states();
	if (failures != 0) {
		std::printf("renderer_scene_overlay: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_scene_overlay ok\n");
	return 0;
}
