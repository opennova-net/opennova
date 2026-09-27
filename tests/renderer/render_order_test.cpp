// Draw-order semantics (REN-3) - pins the witnessed sort-key, queue-split,
// technique-class, and ladder rules of docs/render/render-order-re.md.

#include <runtime/renderer/render_order.h>
#include <runtime/renderer/scene_pass_gates.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++failures;                                                    \
		}                                                                  \
	} while (0)

using namespace opennova::renderer;

static uint32_t float_bits(float f) {
	uint32_t b = 0;
	std::memcpy(&b, &f, sizeof(b));
	return b;
}

int main() {
	// --- technique_class_for_submit: stack defaults win, in bit order
	// [orig: collect_render_objects_for_batch @ 0x5d90d7..0x5d9145].
	CHECK(technique_class_for_submit(0, 0) == TechniqueClass::Normal);
	CHECK(technique_class_for_submit(kStackDefaultClip, 0) == TechniqueClass::Clip);
	CHECK(technique_class_for_submit(kStackDefaultProjShadow, 0) == TechniqueClass::ProjShadow);
	CHECK(technique_class_for_submit(kStackDefaultDepthMask, 0) == TechniqueClass::DepthMask);
	// Stack default beats any submit request.
	CHECK(technique_class_for_submit(kStackDefaultClip, kSubmitMatchTerrainPass) == TechniqueClass::Clip);
	CHECK(technique_class_for_submit(kStackDefaultProjShadow, kSubmitClipPass) == TechniqueClass::ProjShadow);
	// Submit-flag order: CLIP, then DEPTHMASK, then PROJSHAD, then MATCHTERRAIN
	// (the original tests renderFlags&1, &4, &2, &0x200 in that order).
	CHECK(technique_class_for_submit(0, kSubmitClipPass) == TechniqueClass::Clip);
	CHECK(technique_class_for_submit(0, kSubmitDepthMaskPass) == TechniqueClass::DepthMask);
	CHECK(technique_class_for_submit(0, kSubmitProjShadowPass) == TechniqueClass::ProjShadow);
	CHECK(technique_class_for_submit(0, kSubmitDepthMaskPass | kSubmitProjShadowPass) == TechniqueClass::DepthMask);
	CHECK(technique_class_for_submit(0, kSubmitMatchTerrainPass) == TechniqueClass::MatchTerrain);
	CHECK(technique_class_for_submit(0, kSubmitFirstPassOnly | kSubmitAltStream | kSubmitNoGlowCopy) == TechniqueClass::Normal);

	// --- opaque_sort_key bit layout [orig: @ 0x5d928e..0x5d92c8].
	// dist 0x155 (341), effect 0x2A (42), no alpha test:
	// bits[5..0] = (341>>4)&0x3F = 0x15; bits[11..6] = 0x2A;
	// bits[13..12] = (341>>8)&3 = 1; bit14 = 0.
	CHECK(opaque_sort_key(341.0f, 42, false) == ((1u << 12) | (0x2Au << 6) | 0x15u));
	CHECK(opaque_sort_key(341.0f, 42, true) == ((1u << 14) | (1u << 12) | (0x2Au << 6) | 0x15u));
	// Clamping [orig: @ 0x5d927c..0x5d9289]: negatives to 0, >1023 to 1023.
	CHECK(opaque_sort_key(-5.0f, 0, false) == 0u);
	CHECK(opaque_sort_key(4096.0f, 0, false) == ((3u << 12) | ((1023u >> 4) & 0x3Fu)));
	// Effect index masks to 6 bits.
	CHECK(opaque_sort_key(0.0f, 0x7F, false) == (0x3Fu << 6));
	// Ordering invariants: alpha-tested sorts after every plain-opaque key;
	// nearer coarse slab sorts first at equal effect.
	CHECK(opaque_sort_key(1023.0f, 0x3F, false) < opaque_sort_key(0.0f, 0, true));
	CHECK(opaque_sort_key(0.0f, 5, false) < opaque_sort_key(600.0f, 5, false));
	// Bits 15+ stay zero (the original's stack-garbage OR is D-RORD-6,
	// deliberately not reproduced).
	CHECK((opaque_sort_key(1023.0f, 0x3F, true) & 0xFFFF8000u) == 0u);

	// --- transparent_sort_key: exact ~bit_cast [orig: @ 0x5d931c..0x5d9326]
	// and back-to-front ordering (farther draws first = smaller key).
	CHECK(transparent_sort_key(100.0f) == ~float_bits(100.0f));
	CHECK(transparent_sort_key(0.0f) == 0xFFFFFFFFu);
	CHECK(transparent_sort_key(500.0f) < transparent_sort_key(10.0f));
	CHECK(transparent_sort_key(10.25f) < transparent_sort_key(10.0f));

	// --- transparent_queue_for: >= keeps the strip above water
	// [orig: fcomp/jp @ 0x5d934e..0x5d9359].
	CHECK(transparent_queue_for(10.0f, 5.0f) == TransparentQueue::AboveWater);
	CHECK(transparent_queue_for(5.0f, 5.0f) == TransparentQueue::AboveWater);
	CHECK(transparent_queue_for(4.99f, 5.0f) == TransparentQueue::BelowWater);

	// --- the ladder: strict frame order
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0 — the sky pass SkyDome_RenderWithSkyfog
	// @ 0x5ca81a, then Player_RenderViewModelIfAlive @ 0x5ca829, then
	// Terrain_RenderWorldScene @ 0x5c93a0].
	// Inside render_skybox the gradient pass precedes the bodies, which
	// precede the cloud layers [orig: render_skybox @ 0x579080: gradient
	// draw @ 0x5798dc, bodies @ 0x5798e0, clouds @ 0x5798f1..0x579b15].
	CHECK(kRungSkyDome < kRungSkyBody);
	CHECK(kRungSkyBody < kRungSkyClouds);
	// The viewmodel draws after the whole sky pass and before every world draw.
	CHECK(kRungSkyClouds < kRungViewmodel);
	// The slot drapes follow the terrain batch inside the terrain sector pass,
	// after the viewmodel and before every model/foliage/water/transparent
	// submit [orig: Render_ProcessMainSceneFrame @ 0x5ca829 -> @ 0x5ca867
	// (Terrain_RenderMainSectorPass: @ 0x610c34 batch, @ 0x610c47 drapes) ->
	// Terrain_RenderWorldScene @ 0x5ca8ec].
	CHECK(kRungViewmodel < kRungSlotDrape);
	CHECK(kRungSlotDrape < kRungObjectPostMultiply);
	{
		const int world_rungs[] = {kRungObjectPostMultiply, kRungFoliageMaskFarSide,
				kRungAlphaFarSide, kRungTracerFarSide, kRungParticleFarSide,
				kRungFoliageFarSide, kRungWater, kRungWaterDecals,
				kRungFoliageMaskCameraSide, kRungScars, kRungFoliageCameraSide,
				kRungAlphaCameraSide, kRungTracerCameraSide};
		for (int rung : world_rungs) {
			CHECK(kRungSlotDrape < rung);
		}
		CHECK(kRungSlotDrape > kRungSkyClouds);
		CHECK(kRungSlotDrape > kRungSkyBody);
		CHECK(kRungSlotDrape > kRungSkyDome);
	}
	CHECK(kRungViewmodel < kRungObjectPostMultiply);
	// The non-person wave's flush @ 0x5c9506 (the post-multiply passes) ->
	// the far person wave's foliage MODEL masks @ 0x5c955f -> the far-side
	// alpha flush @ 0x5c9596.
	CHECK(kRungObjectPostMultiply < kRungFoliageMaskFarSide);
	CHECK(kRungFoliageMaskFarSide < kRungAlphaFarSide);
	// Far-side alpha flush @ 0x5c9596 -> tracer pass 0 @ 0x5c95ac -> particle
	// pass A @ 0x5c95b5 -> foliage pass 0 @ 0x5c95c5 -> the water pass
	// @ 0x5c95dc.
	CHECK(kRungAlphaFarSide < kRungTracerFarSide);
	CHECK(kRungTracerFarSide < kRungParticleFarSide);
	CHECK(kRungParticleFarSide < kRungFoliageFarSide);
	CHECK(kRungFoliageFarSide < kRungWater);
	// The wake decals inside the water pass, after the surface strip
	// [orig: render_water_surface @ 0x5c3426 then WaterRing_DrawAll @ 0x5c3432].
	CHECK(kRungWater < kRungWaterDecals);
	// The camera person wave's masks @ 0x5c9638 -> the Scar_DrawBatches call
	// @ 0x5c9658 -> foliage pass 1 @ 0x5c9665 -> camera-side alpha flush @ 0x5c967a ->
	// tracer pass 1 @ 0x5c9687.
	CHECK(kRungWaterDecals < kRungFoliageMaskCameraSide);
	CHECK(kRungFoliageMaskCameraSide < kRungScars);
	CHECK(kRungScars < kRungFoliageCameraSide);
	CHECK(kRungFoliageCameraSide < kRungAlphaCameraSide);
	CHECK(kRungAlphaCameraSide < kRungTracerCameraSide);
	CHECK(kRungAlphaCameraSide == 0); // the default rung stays Godot's default

	// --- the water bracket swap [orig: SortAndFlush(camAbove?3:2) @ 0x5c9596;
	// SortAndFlush(3-camAbove) @ 0x5c967a]: the side away from the camera is
	// the far bracket.
	CHECK(transparent_rung_for(TransparentQueue::BelowWater, true) == kRungAlphaFarSide);
	CHECK(transparent_rung_for(TransparentQueue::AboveWater, true) == kRungAlphaCameraSide);
	CHECK(transparent_rung_for(TransparentQueue::AboveWater, false) == kRungAlphaFarSide);
	CHECK(transparent_rung_for(TransparentQueue::BelowWater, false) == kRungAlphaCameraSide);

	CHECK(entity_uses_thermal_wave(3));
	CHECK(!entity_uses_thermal_wave(1));
	CHECK(!entity_uses_thermal_wave(5));
	CHECK(scene_far_plane(1000.9f) == 1001.0f);
	CHECK(scene_far_plane(1000.0f) == 1001.0f);

	// --- the blink/waterline pass gates [orig: Render_ProcessMainSceneFrame
	// @ 0x5ca192..0x5ca1bd; render_main_scene @ 0x5c1342..0x5c1353]: the
	// indoors letter 0x2 skips the terrain pass and the MIRROR's sky, the sky
	// letter 0x4 and an eye at/below the water skip the main frame's sky.
	{
		const ScenePassGates open = scene_pass_gates(0, false);
		CHECK(open.terrain && open.sky && open.mirror_sky);
		const ScenePassGates indoors = scene_pass_gates(0x2, false);
		CHECK(!indoors.terrain);
		CHECK(indoors.sky); // indoors alone keeps the beauty sky bracket
		CHECK(!indoors.mirror_sky);
		const ScenePassGates sky_off = scene_pass_gates(0x4, false);
		CHECK(sky_off.terrain && !sky_off.sky && sky_off.mirror_sky);
		const ScenePassGates underwater = scene_pass_gates(0, true);
		CHECK(underwater.terrain && !underwater.sky && underwater.mirror_sky);
		const ScenePassGates water_letter = scene_pass_gates(0x8, false);
		CHECK(water_letter.terrain && water_letter.sky && water_letter.mirror_sky);
		// The scene core's detail-foliage passes follow the indoors letter
		// alone [orig: Terrain_RenderWorldScene @ 0x5C93D5..0x5C93E0, the skips
		// @ 0x5C95BD..0x5C95BF / @ 0x5C965D..0x5C965F]; the terrain gate
		// keeps its own.
		CHECK(open.detail_foliage);
		CHECK(!indoors.detail_foliage && !indoors.terrain);
		CHECK(sky_off.detail_foliage && underwater.detail_foliage);
		CHECK(water_letter.detail_foliage);
		CHECK(!scene_pass_gates(0x2 | 0x4 | 0x8, true).detail_foliage);
	}

	// --- the reopen edges: a letter read in the frame that draws it
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca192 ahead of @ 0x5ca645;
	// Terrain_RenderWorldScene @ 0x5c93b8 ahead of @ 0x5c95d4]. Only a
	// cleared indoors or water letter reopens a pass; a set letter, a steady
	// value and the sky letter take no edge.
	{
		const ScenePassGateEdges out_of_room = scene_pass_gate_edges(0x2, 0);
		CHECK(out_of_room.terrain_opened && !out_of_room.water_opened);
		const ScenePassGateEdges water_back = scene_pass_gate_edges(0x8, 0);
		CHECK(!water_back.terrain_opened && water_back.water_opened);
		const ScenePassGateEdges both = scene_pass_gate_edges(0x2 | 0x8, 0);
		CHECK(both.terrain_opened && both.water_opened);
		// The letters beside the cleared one ride along untouched: 00TRa's
		// Armry01 room box with the indoors letter (flags 0x28, letters 0x2E)
		// into its box without it (letters 0x28): the water letter stays.
		const ScenePassGateEdges next_room = scene_pass_gate_edges(0x2E, 0x28);
		CHECK(next_room.terrain_opened && !next_room.water_opened);
		const ScenePassGateEdges keeps_sky = scene_pass_gate_edges(0x0E, 0x04);
		CHECK(keeps_sky.terrain_opened && keeps_sky.water_opened);
		const uint32_t no_edge[][2] = {
				{0, 0x2}, {0, 0x8}, {0x2, 0x2}, {0x8, 0x8}, {0x2A, 0x2A},
				{0x4, 0}, {0, 0x4}, {0x28, 0x2A}, {0, 0}};
		for (const auto &pair : no_edge) {
			const ScenePassGateEdges e = scene_pass_gate_edges(pair[0], pair[1]);
			CHECK(!e.terrain_opened && !e.water_opened);
		}
	}

	if (failures != 0) {
		std::printf("renderer_render_order: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_render_order ok\n");
	return 0;
}
