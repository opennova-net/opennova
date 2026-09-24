#pragma once

// Draw-order semantics - the witnessed ordering rules of the original
// renderer's batch system, as pure data + functions (REN-3, ADR 0023).
//
// The original sorts 68-byte batch entries into four queues and flushes them
// in a fixed frame bracket (docs/render/render-order-re.md). The queue
// machinery is a device-era artifact and is NOT reproduced; what this module
// carries is the ORDER ITSELF: the sort-key semantics, the technique-class
// selection, the transparent queue split around the water plane, and the
// frame's transparent ordering ladder that the Godot layer applies as
// render_priority rungs (generalizing the celestial ladder).
//
// Pure C++, no Godot deps. T1-pinned by renderer_state_vectors (section 3)
// and renderer_render_order.

#include <cstdint>

namespace opennova::renderer {

// Person entities (ItemDef type 3) enter the two BySide waves. The queued
// alpha draws flush after the world block is restored; the opaque draws use
// the flat block. Held models inherit their owner's wave.
// [orig: collect_visible_entities_for_terrain @0x5C8C60;
// Terrain_RenderSceneWithReflection @0x5C9511..0x5C9616]
inline bool entity_uses_thermal_wave(int item_type) { return item_type == 3; }

// The main scene pins this after applying FOV, independent of camera mode.
// [orig: Render_ProcessMainSceneFrame @0x5CA0F0]
float scene_far_plane(float fog_distance);

// The six technique classes, batch-selected per entry (flag bits 4-6) and
// mapped to the material def's cached pass blocks at draw time
// [orig: CRenderBatchQueue_FlushBatches @ 0x5d9ff3: NORMAL +600,
// PROJSHAD +680, DEPTHMASK +760, CLIP +840, GLOW +920, MATCHTERRAIN +1000].
enum class TechniqueClass : uint8_t {
	Normal = 0,
	ProjShadow = 1,
	DepthMask = 2,
	Clip = 3,
	Glow = 4,
	MatchTerrain = 5,
};

// Render_SubmitEntity's 4th argument - the render-flags word distributed to
// the collectors and bone callbacks [orig: Render_SubmitEntity @ 0x5dad80;
// full table in docs/render/render-order-re.md].
constexpr uint32_t kSubmitClipPass = 0x1;        // CLIP class; skinned path skips entirely
constexpr uint32_t kSubmitProjShadowPass = 0x2;  // PROJSHAD class
constexpr uint32_t kSubmitDepthMaskPass = 0x4;   // DEPTHMASK class; opaque strips only
constexpr uint32_t kSubmitFirstPassOnly = 0x8;   // run only the technique's first pass
// Entry flag bit 3: the flush sets ZFUNC ALWAYS for the entry's passes
// [orig: CRenderBatchQueue_FlushBatches @ 0x5da32a..0x5da341]; the sun glow
// and the water glint submit with it (0x110: the glow @ 0x5ad0f7, the glint
// submit @ 0x5ad470).
constexpr uint32_t kSubmitZAlways = 0x10;
constexpr uint32_t kSubmitBoneAlphaBelow = 0x20; // bone path: transparents to the below-water queue
constexpr uint32_t kSubmitAltStreamSub = 0x40;   // alt vertex stream for sub-objects (robj > 0)
constexpr uint32_t kSubmitAltStream = 0x80;      // alt vertex stream unconditionally
constexpr uint32_t kSubmitNoGlowCopy = 0x100;    // suppress the Q3 glow/envmap copy
constexpr uint32_t kSubmitMatchTerrainPass = 0x200; // MATCHTERRAIN class (decal sub-pass)
// The second (body) part of the composed player avatar; the one-shot overlay
// children (held weapon, carried object) skip it so they draw once per entity
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c8018; consumer
// BoneCallback_org0_World @ 0x4e3c87].
constexpr uint32_t kSubmitAvatarSecondPart = 0x10000000;

// Render-state-stack class-default bits (entry flags at stack entry +8),
// inherited by everything submitted under the pushed frame
// [orig: g_RenderStateStack @ 0x843580; Terrain_RenderSectorEntities
// @ 0x5c7c38 sets bit 0 for below-water mirror clipping].
constexpr uint32_t kStackDefaultClip = 0x1;
constexpr uint32_t kStackDefaultProjShadow = 0x2;
constexpr uint32_t kStackDefaultDepthMask = 0x4;

// Technique-class selection for a submitted batch entry: the state stack's
// class defaults win over the submit flags; NORMAL otherwise
// [orig: collect_render_objects_for_batch @ 0x5d90d7..0x5d9145;
// collect_render_batches_for_entity @ 0x5d95c0..0x5d961f].
TechniqueClass technique_class_for_submit(uint32_t stack_default_flags,
                                          uint32_t submit_flags);

// --- Sort keys -------------------------------------------------------------
//
// The comparator sorts entries by an UNSIGNED 32-bit key, ascending; the
// flush iterates ascending, so the smallest key draws first
// [orig: RenderBatch_QuickSort @ 0x5d8b40, cmp/jnb @ 0x5d8b93].

// Opaque key [orig: @ 0x5d928e..0x5d92c8]: draw plain opaques before
// alpha-tested ones; within, front-to-back in 256-unit depth slabs, effects
// grouped within a slab, 16-unit fine depth within an effect. `view_depth`
// is the render object's origin depth along the camera-forward plane
// [orig: g_BatchSortDepthPlaneX @ 0x2721A08, the plane's floats run to 0x2721A38]; `effect_index` is the
// effect's registry index [orig: the /1004 magic divide @ 0x5d924c].
// Bits 15..31 are zero here - the original ORs in residual stack garbage
// (constant within a call; D-RORD-6, never reproduced).
uint32_t opaque_sort_key(float view_depth, uint32_t effect_index, bool alpha_tested);

// Transparent / glow-copy key [orig: @ 0x5d931c..0x5d9326]:
// -1 - bit_cast<int32>(depth) = ~bits, which under the unsigned ascending
// sort is exact back-to-front by IEEE float order for non-negative depths.
// (The original's one-strip key lag is D-RORD-6, never reproduced.)
uint32_t transparent_sort_key(float view_depth);

// --- The water-plane transparent bracket -----------------------------------

// Which transparent queue a strip joins: above-water (Q1) when its center
// height reaches the water plane, else below-water (Q2)
// [orig: @ 0x5d932e..0x5d9354 vs g_WaterSplitHeightFloat @ 0x8437C4].
enum class TransparentQueue : uint8_t {
	AboveWater,
	BelowWater,
};
TransparentQueue transparent_queue_for(float world_height, float water_height);

// The frame's transparent ordering ladder, applied by the Godot layer as
// render_priority rungs (within one rung Godot's per-object back-to-front
// depth sort matches the per-queue ~float-bits keys above). The witnessed
// frame [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0]: the sky pass
// (dome -> bodies -> clouds, sub_579CB0 @ 0x5ca81a), then the first-person
// viewmodel (@ 0x5ca829), then the scene core [orig:
// Terrain_RenderSceneWithReflection @ 0x5c93a0]: far-water-side alpha
// (flush @ 0x5c9596) -> tracer pass 0 (@ 0x5c95ac) -> particle pass A
// (@ 0x5c95b5) -> detail foliage pass 0 (@ 0x5c95c5) -> the water surface
// with its decals (@ 0x5c95dc) -> the camera-side opaque wave -> the scars
// (@ 0x5c9658) -> detail foliage pass 1 (@ 0x5c9665) -> camera-side alpha
// (flush @ 0x5c967a) -> tracer pass 1 (@ 0x5c9687) -> particle pass B
// (@ 0x5c9690) -> the post-particle overlay tail (@ 0x5c9695..0x5c9714).
// Particle pass B and the overlay tail are compositor passes, not rungs.
// Values keep the sky group
// before all world alpha and leave the camera-side rung at Godot's default 0
// so unclassified transparents land there naturally.
// The sun/moon bodies inside the dome pass [orig: render_skybox @ 0x579080 ->
// render_celestial_bodies @ 0x5acaa0].
constexpr int kRungSkyBody = -12;
// The dome's cloud layers, drawn after the bodies inside the same pass
// [orig: render_skybox cloud pass @ 0x5798f1..0x579b15].
constexpr int kRungSkyClouds = -11;
// The first-person viewmodel flushes whole (its alpha strips included) after
// the sky pass and before every world draw [orig: sub_579CB0 @ 0x5ca81a then
// Player_RenderViewModelIfAlive @ 0x4e0140, called @ 0x5ca829]; its depth
// band keeps later world alpha off it.
constexpr int kRungViewmodel = -10;
// BmTxMirrT's P3 post-multiply is a PASS of the strip's own technique, not a
// second submit: FlushBatches runs every pass of one entry back to back
// (the pass loop @ 0x5da20b..0x5da23d over technique+4 passes, fog/blend per
// pass @ 0x5da2f7) inside the opaque queue flush, so retail draws it right
// after that strip's P0/P1 within flush(1) [orig: CRenderBatchQueue_FlushBatches
// @ 0x5d9f50; the Q0 flushes @ 0x5c9506..0x5c9581 and @ 0x5c9630..0x5c9647]
// and never inside the Q1/Q2 transparent flushes [orig: @ 0x5c9596;
// @ 0x5c967a]. Godot cannot interleave a blended pass into its opaque stage,
// so the rung sits above every sky rung and below every world transparent —
// after all opaques, before far-side alpha and the water. The far-side
// wave's flushes (@ 0x5c9557, @ 0x5c956e, @ 0x5c9581) precede the water pass
// (@ 0x5c95dc); the flushes after it (@ 0x5c9630, @ 0x5c9647) carry the
// camera-side wave, whose strips lie in front of the water surface, so the
// one rung orders both the way retail does.
constexpr int kRungObjectPostMultiply = -9;
constexpr int kRungAlphaFarSide = -8;    // world alpha on the water side AWAY from the camera
// The tracer pool's far-side pass, after the far-side alpha flush
// [orig: CEffectEmitterPool_RenderMainPass(0, side) @ 0x5c95ac].
constexpr int kRungTracerFarSide = -7;
// Particle pass A: the far-side particle subset, after the far-side tracers
// and before the far-side foliage [orig: EffectWorld_RenderParticlePass(0)
// @ 0x5c95b5].
constexpr int kRungParticleFarSide = -6;
// Detail foliage on the far side of the water, before the water surface
// [orig: Foliage_RenderFarPatchesPass(0) @ 0x5c95c5].
constexpr int kRungFoliageFarSide = -5;
constexpr int kRungWater = -4;           // the water surface (drawn between the side brackets)
// The water decals (the vehicle wake rings) inside the water pass, right
// after the surface strip [orig: render_water_surface @ 0x5c3426 strip then
// the wake bank scanner sub_5DE340 @ 0x5c3432].
constexpr int kRungWaterDecals = -3;
// The impact scars, after the camera-side opaque wave and before foliage
// pass 1 and the camera-side alpha [orig: Scar_DrawBatches @ 0x5c9658].
constexpr int kRungScars = -2;
// Detail foliage on the camera's side of the water, before the camera-side
// alpha flush [orig: Foliage_RenderFarPatchesPass(1) @ 0x5c9665].
constexpr int kRungFoliageCameraSide = -1;
constexpr int kRungAlphaCameraSide = 0;  // world alpha on the camera's side (the default rung)
// The tracer pool's camera-side pass, after the camera-side alpha flush and
// before particle pass B [orig: CEffectEmitterPool_RenderMainPass(1, side)
// @ 0x5c9687].
constexpr int kRungTracerCameraSide = 1;
constexpr int kRungSunGlow = 2;          // the sun-glow lens glare, drawn last

// The rung for a world transparent on a given water side. The original
// flushes the far side first and the camera side last (mode camAbove?3:2
// then 3-camAbove [orig: @ 0x5c9596; @ 0x5c967a]). The host applies the
// camera side is published from the adjusted render eye every frame, so the
// ordering mirrors when that eye crosses below the water plane.
int transparent_rung_for(TransparentQueue side, bool camera_above_water);

}  // namespace opennova::renderer