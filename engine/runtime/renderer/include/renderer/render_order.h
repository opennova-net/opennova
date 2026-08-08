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

namespace renderer {

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
constexpr uint32_t kSubmitEntryBit3 = 0x10;      // entry flag bit 3 (consumer unwitnessed - REN-4)
constexpr uint32_t kSubmitBoneAlphaBelow = 0x20; // bone path: transparents to the below-water queue
constexpr uint32_t kSubmitAltStreamSub = 0x40;   // alt vertex stream for sub-objects (robj > 0)
constexpr uint32_t kSubmitAltStream = 0x80;      // alt vertex stream unconditionally
constexpr uint32_t kSubmitNoGlowCopy = 0x100;    // suppress the Q3 glow/envmap copy
constexpr uint32_t kSubmitMatchTerrainPass = 0x200; // MATCHTERRAIN class (decal sub-pass)
constexpr uint32_t kSubmitRepeatDraw = 0x10000000;  // dual-LOD near-LOD redraw; one-shot
                                                    // overlay children skip (world s13)

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
// [orig: g_BatchSortDepthPlane @ 0x2721A08..38]; `effect_index` is the
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
// depth sort matches the per-queue ~float-bits keys above). Witnessed frame
// bracket [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]: sky pass
// (dome -> star field -> bodies) -> far-water-side alpha -> water surface ->
// camera-side alpha -> weather/particle overlays -> sun glow last
// [orig: render_skybox_sun_glow @ 0x5c9714, the frame's final draw].
// Values keep the celestial group before all world alpha and leave the
// camera-side rung at Godot's default 0 so unclassified transparents land
// there naturally.
constexpr int kRungSkyStars = -6;        // star field (sky pass, before bodies)
constexpr int kRungSkyBody = -5;         // sun / moon bodies
constexpr int kRungAlphaFarSide = -2;    // world alpha on the water side AWAY from the camera
constexpr int kRungWater = -1;           // the water surface (drawn between the side brackets)
constexpr int kRungAlphaCameraSide = 0;  // world alpha on the camera's side (the default rung)
constexpr int kRungOverlayFx = 1;        // weather / particle / trail overlays (PTL substrate)
constexpr int kRungSunGlow = 2;          // the sun-glow lens glare, drawn last

// The rung for a world transparent on a given water side. The original
// flushes the far side first and the camera side last (mode camAbove?3:2
// then 3-camAbove [orig: @ 0x5c9596; @ 0x5c967a]). The host applies the
// camera-above case statically today (below -> far, above -> camera side);
// the underwater-camera swap is a tracked residual
// (docs/render/render-order-re.md D-RORD-3 note).
int transparent_rung_for(TransparentQueue side, bool camera_above_water);

} // namespace renderer
