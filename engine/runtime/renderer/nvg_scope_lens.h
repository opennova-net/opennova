#pragma once

// The NVG view's scoped lens: with a Scoped (non-Inset) sight raised under NVG
// the frame does not composite the NVG tint and glow over the whole screen.
// It clears the frame to black and draws them through a lens: four concentric
// bands whose texture radii compress the scene's inscribed circle into a disc
// of 0.71 x the ring size (five passes: the tint, then four quarter-strength
// glow passes that smear it by texel offsets, a radial shrink and a small
// rotation), and around the disc a ring out to 1.5 x the ring size textured
// with a 64 x 16 polar unwrap of the scene's edge. No NVG mask draws; the
// SIGHTS card (or, without an authored row, the reticle cross at unit scale,
// hud/scope_circle_mask.h build_nvg_lens_reticle) draws on top.
// [orig: Render_ProcessMainSceneFrame @0x5ca71a (sub_5D2B10 ->
//  draw_minimap_compass_border(1)); the polar unwrap at the tail of
//  terrain_scene_render @0x5d0a0e..0x5d0eb4]
//
// Every vertex here is a pre-transformed (XYZRHW, z 0.5, rhw 1) screen vertex
// in the pixel space of its target, D3D9 pixel centres on integers: the
// overlay rect for the lens, the 64 x 16 unwrap target for the polar passes.
// Strips are D3DPT_TRIANGLESTRIP order, 130 vertices each (65 angle stops of
// an inner and an outer vertex).

#include <array>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

struct NvgLensVertex {
	float x = 0.0f;
	float y = 0.0f;
	std::uint32_t argb = 0u; // D3DCOLOR
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
};

using NvgLensStrip = std::vector<NvgLensVertex>;

inline constexpr int kNvgLensStripVertices = 130;

// The polar unwrap target and its eight passes. Pass p strips the 64 columns
// (65 stops, one turn, stop k at BAM table index 16k - 8 + 2p) from row 0 at
// texture radius 0.48 - 0.01p to row 16 at 0.5 - 0.005p about the scene's
// centre, both texture sets alike, through ps text 0x7DC5A8 (4 x luma(0.30,
// 0.60, 0.10) x vertex colour; colour (255 / (p + 1)) << 24 | 0x3F3F3F) with
// blending off, into a target cleared to 0xFFFF0000: each pass overwrites the
// last, so the target holds pass 7's.
// [orig: terrain_scene_render -- the clear @0x5d0a17..0x5d0a1e, the state
//  dword_2BDFAB0 and the scene on stages 0/1 @0x5d0a41..0x5d0a5b, the base
//  angle 0x2200000 @0x5d0a75 and its 0x800000 step @0x5d0e8c, the radii and
//  colour @0x5d0a93..0x5d0af2, the stops @0x5d0b01..0x5d0e5c, the draw
//  @0x5d0e7f; init_view_effect_shaders_and_textures @0x5cf960 (the ps text)
//  and @0x5cf9de..0x5cf9ea (blend ONE/ZERO, off);
//  init_water_reflection_render_targets @0x5cf6c1..0x5cf6de (64 x 16)]
inline constexpr int kNvgPolarWidth = 64;
inline constexpr int kNvgPolarHeight = 16;
inline constexpr int kNvgPolarPasses = 8;
inline constexpr std::uint32_t kNvgPolarClearColor = 0xFFFF0000u;
inline constexpr std::array<float, 3> kNvgPolarLuma = {0.30f, 0.60f, 0.10f};
inline constexpr float kNvgPolarGain = 4.0f;
std::array<NvgLensStrip, kNvgPolarPasses> nvg_polar_unwrap_passes();

// The lens bands. Pass 0 draws the NVG tint stage (frame_fx_effects.h
// nvg_tint_color) from the scene with blending off; passes 1-4 add the glow
// target ONE/ONE, MODULATE2X by the vertex colour 0xFF202020 (a quarter
// strength each). Each pass is four strips, band b spanning the screen radii
// {0, 0.5, 0.7, 0.705, 0.71}[b..b+1] x ring and the texture radii
// {0, 0.324, 0.475, 0.4775, 0.48}[b..b+1] minus 0.00018 q x the steps
// {0, 8, 26, 32, 36}[b..b+1], q the glow pass (0 for passes 0 and 1); the
// texture coordinates carry +-0.5 / scene texel offsets keyed on the pass's
// bits 0/1, and glow passes q = 2 / 3 rotate them by +- (step << 17) BAM.
// [orig: draw_minimap_compass_border -- the ring size and centre
//  @0x5d1d65..0x5d1d7c / @0x5d1db5, the radii @0x5d1dc0..0x5d1e76, the steps
//  @0x5d1def..0x5d1e3b, the texture radii @0x5d1e7a..0x5d1ec2, the pixel-shader
//  arm's colours and five passes @0x5d1eec..0x5d1f18, the tint state and scene
//  @0x5d1f22..0x5d1f38, the glow state and target on pass 1 @0x5d2055..0x5d2060,
//  q @0x5d200c..0x5d2023, the texel offsets @0x5d2076..0x5d20a0, the shrink
//  @0x5d20b6, the stops @0x5d2179..0x5d2305 (rotation @0x5d21af..0x5d21ce), the
//  draw @0x5d2328; the glow state GfxShader_Create1TexModeId(0, 0x602)
//  init_view_effect_shaders_and_textures @0x5cfc23..0x5cfc34]
inline constexpr int kNvgLensPasses = 5;
inline constexpr int kNvgLensBands = 4;
inline constexpr std::uint32_t kNvgLensTintColor = 0xFF408040u;
inline constexpr std::uint32_t kNvgLensGlowColor = 0xFF202020u;
// The ring: stage 0 MODULATE2X(polar(uv0), diffuse), stage 1
// MODULATE2X(current, polar(uv1)), blending off; inner vertices (0.71 x ring)
// 0xFF181820 at (u, 0) / (u, 0), outer (1.5 x ring) 0xFF040408 at (u, 1) /
// (u, 0), u = k / 64 at BAM table index 16k.
// [orig: draw_minimap_compass_border -- the state dword_2BDFAB4 and the polar
//  target on stages 0/1 @0x5d2354..0x5d2380, the stops @0x5d23be..0x5d2770,
//  the draw @0x5d2793; the state sub_6790E0(0, 0, 0x400600, 2)
//  init_view_effect_shaders_and_textures @0x5cf92a..0x5cf94a]
inline constexpr std::uint32_t kNvgLensRingInnerColor = 0xFF181820u;
inline constexpr std::uint32_t kNvgLensRingOuterColor = 0xFF040408u;

struct NvgScopeLens {
	float center_x = 0.0f;
	float center_y = 0.0f;
	std::int32_t ring_size = 0; // ((y1 - y0) >> 3) + ((y1 - y0) >> 1)
	std::array<std::array<NvgLensStrip, kNvgLensBands>, kNvgLensPasses> passes;
	NvgLensStrip ring;
};

// The lens over the overlay rect (x0, y0)..(x1, y1), retail's inclusive
// viewport rect (the full screen is (0, 0)..(W - 1, H - 1)). `scene_side` is
// the NVG scene target's side the texel offsets key on (dword_2BDFA94/98).
NvgScopeLens build_nvg_scope_lens(std::int32_t x0, std::int32_t y0, std::int32_t x1,
		std::int32_t y1, int scene_side);

// The Scoped arm's NVG scene renders a SQUARE frustum over the 512 target
// (scaleY 1.0) at fov = ftol(flt_8409EC x 10485760 x (1 / zoom) x 0.5) Q16
// degrees, flt_8409EC the selected H/W ratio and zoom the slot's clamped
// magnification; the other arms render the frame's own frustum
// (scaleY = flt_8409EC x 512 / 512).
// [orig: sub_5D2990 @0x5d29e4..0x5d2a2a; sub_5D28D0 @0x5d2954..0x5d296d;
//  flt_2BDFA68 = 512 / 512 init_water_reflection_render_targets
//  @0x5cf5e1..0x5cf5ef]
std::int32_t nvg_scoped_scene_fov_q16(float selected_h_over_w, std::int32_t zoom);

// The Sighted arm's NVG scene keeps the frame's frustum shape at fov
// ftol((1 / zoom) x 5242880) Q16 degrees -- 80 / zoom through the x87
// reciprocal.
// [orig: Math_BuildScaledFixedPointToFloatMatrix (the Sighted NVG scene,
//  0x5d2a50) @0x5d2ab8..0x5d2acf]
std::int32_t nvg_sighted_scene_fov_q16(std::int32_t zoom);

} // namespace opennova::renderer
