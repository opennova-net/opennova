#pragma once

#include <cstdint>
#include <vector>

// The scoped-view circle mask: the near-black annulus the scene frame draws
// over the scoped view every scoped frame, plus the reticle cross and cardinal
// grid ticks it chains when the equipped weapon authored no SIGHTS row.
// [orig: Hud_DrawScopeCircleMask @0x5d17a0 (0x5d17a0..0x5d1d01);
//  draw_minimap_crosshair_and_grid @0x5d1160 (0x5d1160..0x5d1723)]
//
// The mask is NOT a fallback for a rowless weapon: `Hud_DrawScopeCircleMask`
// is called unconditionally on the Scoped arm of the scene frame's overlay
// fork, right after the SIGHTS card draws, and its ONLY argument gates the
// inner cross/grid [orig: Render_ProcessMainSceneFrame
//  @0x5cab08..0x5cab15 `v11 = draw_weapon_sight_overlays(...);
//  Hud_DrawScopeCircleMask(!v11, ...)`; the same shape in the second caller
//  render_hud_overlay @0x5d82e5..0x5d82f2 `xor ecx,ecx; cmp eax,ebx; setz cl`].
//
// Everything here is device-free geometry in PIXELS of the live viewport: the
// embedder rasterises the three batches as flat vertex-coloured triangles.
// Retail submits them through the dynamic vertex buffer as XYZRHW + DIFFUSE +
// SPECULAR + TEX2 vertices (stride 40, the specular dword and both texcoord
// sets left stale), with device render-state slot 1 (ring) / 2 (cross + grid)
// and effect pass 0x700000 — the untextured alpha-blended overlay pass, which
// is why the cross and grid write no texcoords at all
// [orig: @0x5d185b/@0x5d1866, @0x5d139e/@0x5d13a9;
//  GDynamicVB_DrawPrimitive(5 /* D3DPT_TRIANGLESTRIP */, &unk_2BE1088, 130)
//  @0x5d1cc4; GDynamicVB_DrawIndexedPrimitive(4 /* TRIANGLELIST */, ...)
//  @0x5d1435 / @0x5d1704].
//
// Witness record: docs/interface/hud-re.md.

namespace opennova::hud {

// ---------------------------------------------------------------------------
// The scene frame's scoped overlay fork.

// The four mutually exclusive overlays the frame picks between after the world
// pass, in retail's test order.
// [orig: Render_ProcessMainSceneFrame @0x5caae1..0x5cab26;
//  render_hud_overlay @0x5d82c7..0x5d82f7]
enum class ScopedViewOverlay {
	// Neither selector byte: the projected entity markers.
	// [orig: Render_DrawEntityOverlayMarkers @0x5cab26]
	kEntityMarkers = 0,
	// The binocular mask (its own drawer, no circle mask).
	// [orig: sub_5CFE60 @0x5caaec]
	kBinocularMask = 1,
	// The Sighted selector: the SIGHTS card alone, never the circle mask.
	// [orig: draw_weapon_sight_overlays @0x5caafa]
	kSightedCard = 2,
	// The Scoped selector: the SIGHTS card, then the circle mask with
	// draw_crosshair = (the card drew no row).
	// [orig: @0x5cab08..0x5cab15]
	kScopedCardWithCircleMask = 3,
};

ScopedViewOverlay scoped_view_overlay(bool binoculars_view_active, bool sighted,
		bool scoped);

// The two selector bytes' weapon.def halves, as the frame latches them before
// the fork. `Player_IsVehicleHasAttackCapability`'s clear of BOTH bytes
// @0x5ca2ff/@0x5ca304 is the embedder's vehicle-attack context, not a def bit.
// [orig: Player_IsEquippedWeaponScoped @0x4dcc80 + the Inset split
//  @0x5ca2be..0x5ca2c7; Player_IsVehicleGunnerScoped @0x4dcd30 called
//  @0x5ca2cc, byte set @0x5ca2d5]
bool scoped_selector_from_def(uint32_t weapon_flags, uint32_t weapon_flags2);
bool sighted_selector_from_def(uint32_t weapon_flags, bool slot_switching_from);

// ---------------------------------------------------------------------------
// Geometry.

// One submitted vertex: viewport pixels plus retail's D3DCOLOR (0xAARRGGBB).
struct ScopeMaskVertex {
	float x = 0.0f;
	float y = 0.0f;
	uint32_t argb = 0u;
};

// The derived frame of the whole drawing, from the viewport rect alone.
// [orig: @0x5d17cc..0x5d1857 and @0x5d1184..0x5d12b5]
struct ScopeCircleMaskGeometry {
	float center_x = 0.0f; // (x0 + x1) >> 1 [orig: @0x5d17cc]
	float center_y = 0.0f; // (y0 + y1) >> 1 [orig: @0x5d17e1]
	// cx / cy * 0.75 and 3 / (selected H/W ratio * 4): at the native ratio the
	// two are equal and the ring is a true circle; a forced 4:3/16:10/16:9/5:4
	// ratio keeps scale_y fixed and stretches the ring horizontally.
	// [orig: @0x5d17f5 / @0x5d1811 over sub_58A920 -> flt_8409EC]
	float scale_x = 0.0f;
	float scale_y = 0.0f;
	// ((y1 - y0) >> 3) + ((y1 - y0) >> 1) — five eighths of the viewport
	// height, as an INTEGER shift pair. [orig: @0x5d1830]
	float ring_size = 0.0f;
	float radius_inner = 0.0f; // 0.71f * ring_size [orig: @0x5d184d]
	float radius_outer = 0.0f; // ring_size * 1.5f  [orig: @0x5d1857]
	// The cross/grid unit: half the arm thickness and the diamond half-size,
	// (W + W) * (1/640) = W/320 screen pixels [orig: @0x5d12ad..0x5d12b5].
	float arm_half_thickness = 0.0f;
	// The cardinal tick pitch, (W * 10) * (1/640) = W/64 screen pixels — plain
	// screen space, NOT scaled by scale_x/scale_y [orig: @0x5d160a..0x5d1635].
	float tick_spacing = 0.0f;
};

// The ring: 64 quad segments, 65 angle stops (the 65th closes the loop), two
// vertices per stop, submitted as ONE 130-vertex triangle strip.
// [orig: the loop @0x5d18ad..0x5d1ca1 (five segments per iteration, break at
//  `one - 2 > 64`); the draw @0x5d1cc4]
inline constexpr int kScopeRingSegments = 64;
inline constexpr int kScopeRingVertexCount = 130;
// The BAM table stride per segment: the sin/cos tables hold 1024 entries per
// revolution, and the loop steps the index by 16 (0x4000000 of BAM per
// segment). [orig: the `>> 22` index pairs @0x5d18ad..0x5d1bdd]
inline constexpr int kScopeRingTableStep = 16;
inline constexpr int kScopeRingTableEntries = 1024;
// The two ring colours, inner then outer [orig: @0x5d18cf / @0x5d1936].
inline constexpr uint32_t kScopeRingInnerColor = 0xFF181820u;
inline constexpr uint32_t kScopeRingOuterColor = 0xFF040408u;

// The reticle cross: four tapered spokes (left, up, right, down), each a
// 7-vertex / 18-index fan-and-band. The spoke runs from the exact viewport
// centre out to 0.71 of the ring radii, with its inner break at 0.4; the two
// on-axis vertices are opaque black, the four edge vertices fully transparent,
// and the centre vertex carries alpha 0x20.
// [orig: the four GDynamicVB_DrawIndexedPrimitive calls @0x5d1435 / @0x5d14bf
//  / @0x5d153b / @0x5d15b7; the 0.4 / 0.71 literals flt_7C56A0 / flt_7DC624]
inline constexpr int kScopeCrosshairArms = 4;
inline constexpr int kScopeCrosshairArmVertices = 7;
inline constexpr int kScopeCrosshairArmIndices = 18;
inline constexpr float kScopeCrosshairInnerFraction = 0.4f;  // flt_7C56A0
inline constexpr float kScopeCrosshairOuterFraction = 0.71f; // flt_7DC624
inline constexpr uint32_t kScopeCrosshairCenterColor = 0x20000000u; // @0x5d132c
inline constexpr uint32_t kScopeCrosshairAxisColor = 0xFF000000u;   // @0x5d1344
inline constexpr uint32_t kScopeCrosshairEdgeColor = 0x00000000u;   // @0x5d1338

// The cardinal grid: four ticks in each of the four directions (+X, -X, +Y,
// -Y in retail's switch order), each a 5-vertex / 12-index diamond of half-size
// arm_half_thickness at i * tick_spacing from the centre.
// [orig: the nested loop @0x5d1653..0x5d171d; the switch @0x5d167c;
//  the draw @0x5d1704]
inline constexpr int kScopeGridDirections = 4;
inline constexpr int kScopeGridTicksPerDirection = 4;
inline constexpr int kScopeGridTickVertices = 5;
inline constexpr int kScopeGridTickIndices = 12;
inline constexpr uint32_t kScopeGridTickCenterColor = 0xFF000000u; // @0x5d162b
inline constexpr uint32_t kScopeGridTickEdgeColor = 0x10000000u;   // @0x5d1639

// The whole drawing, in submit order: the ring first, then (only when
// draw_crosshair) the cross and the grid. Each batch is a TRIANGLE LIST —
// the ring's 130-vertex strip is expanded here so the embedder submits one
// index form for all three.
struct ScopeCircleMask {
	ScopeCircleMaskGeometry geometry;
	std::vector<ScopeMaskVertex> ring;
	std::vector<uint16_t> ring_indices;
	std::vector<ScopeMaskVertex> crosshair;
	std::vector<uint16_t> crosshair_indices;
	std::vector<ScopeMaskVertex> grid;
	std::vector<uint16_t> grid_indices;
};

// `aspect_mode` is the renderer's selected-ratio mode (renderer/aspect_ratio.h):
// 0..3 are the four literal ratios, anything else (the port's default) is the
// viewport's own H/W. `screen_width` is the full surface width the cross/grid
// unit keys on (retail's overlayCtx @0x24C1420), which the viewport rect does
// not supply.
ScopeCircleMaskGeometry scope_circle_mask_geometry(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1, int32_t screen_width, int aspect_mode = -1);

ScopeCircleMask build_scope_circle_mask(int32_t x0, int32_t y0, int32_t x1,
		int32_t y1, int32_t screen_width, bool draw_crosshair,
		int aspect_mode = -1);

// The NVG view's scoped lens (renderer/nvg_scope_lens.h) draws its own ring;
// when the SIGHTS card drew no authored row it chains only the cross and the
// grid, about the same centre and ring size but at UNIT scale -- no aspect
// stretch, no annulus. The result's ring is empty.
// [orig: draw_minimap_compass_border @0x5d2798..0x5d27bc:
//  `if (!draw_weapon_sight_overlays()) draw_minimap_crosshair_and_grid(ring,
//  cx, cy, 1.0, 1.0)`]
ScopeCircleMask build_nvg_lens_reticle(int32_t x0, int32_t y0, int32_t x1,
		int32_t y1, int32_t screen_width);

} // namespace opennova::hud
