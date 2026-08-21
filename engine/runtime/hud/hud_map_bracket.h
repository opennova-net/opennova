#pragma once

#include <array>
#include <cstdint>

namespace opennova::hud {

// THE MAP TARGET BRACKET — the three-band mesh retail draws around a marked
// target on the map [orig: FUN_0059BCB0 @0x59BCB0].
//
// It is not a rectangle outline. It is TWELVE vertices forming three quads:
// an outer frame plus two inner bands, each inset from the target rect by a
// fixed fraction of its own size. The two inner bands are what read as the
// bracket's "tick marks" — drawing a plain outline instead loses the shape
// entirely.
//
// Inputs are already-scaled SCREEN coordinates, matching what the original
// receives after the virtual-coordinate transform. Target selection belongs to
// the caller; this only builds the mesh.

// The inset is an eighth of the rect, and the centre is its midpoint
// [orig: DAT_007c8ae8 = 0x3E000000 (0.125), DAT_007c3b94 = 0x3F000000 (0.5)].
inline constexpr float kBracketInsetFactor = 0.125f;
inline constexpr float kBracketCenterFactor = 0.5f;

inline constexpr int kBracketVertexCount = 12;
inline constexpr int kBracketIndexCount = 18; // three quads, two triangles each

// The triangle list [orig: the index run at @0x59BCB0]. Three quads wound the
// same way: {0,1,2, 1,3,2} repeated at +4 and +8.
inline constexpr std::array<int, kBracketIndexCount> kBracketIndices = {
	0, 1, 2, 1, 3, 2,
	4, 5, 6, 5, 7, 6,
	8, 9, 10, 9, 11, 10,
};

struct BracketVertex {
	float x = 0.0f;
	float y = 0.0f;
};

// Build the twelve vertices for a target rect, in the witnessed order.
//
// The order is not decorative: the index list above pairs specific vertices
// into triangles, so reordering these silently produces a different mesh
// rather than a compile error.
inline std::array<BracketVertex, kBracketVertexCount> bracket_vertices(
		float x1, float y1, float x2, float y2) {
	const float dx = (x2 - x1) * kBracketInsetFactor;
	const float dy = (y2 - y1) * kBracketInsetFactor;
	const float cx = (x1 + x2) * kBracketCenterFactor;
	const float cy = (y1 + y2) * kBracketCenterFactor;
	return {{
		// 0..3 — the outer frame, corners in the original's order.
		{x2, y2}, {x1, y2}, {x2, y1}, {x1, y1},
		// 4..7 — the horizontal inner band, straddling the centre in x.
		{cx + dx, y2 - dy}, {cx - dx, y2 - dy},
		{cx + dx, y1 + dy}, {cx - dx, y1 + dy},
		// 8..11 — the vertical inner band, straddling the centre in y.
		{x2 - dx, cy + dy}, {x1 + dx, cy + dy},
		{x2 - dx, cy - dy}, {x1 + dx, cy - dy},
	}};
}

// Per-vertex colours, packed 0xAARRGGBB: the outer four are WHITE and the eight
// inner are RED [orig: @0x59BD65..0x59BE61]. The alpha argument the original
// takes as param_5 is unused by this routine.
inline std::array<uint32_t, kBracketVertexCount> bracket_colors(int alpha) {
	if (alpha < 0) alpha = 0;
	if (alpha > 0xFF) alpha = 0xFF;
	const uint32_t a = static_cast<uint32_t>(alpha) << 24;
	const uint32_t outer = a | 0x00FFFFFFu;
	const uint32_t inner = a | 0x00FF0000u;
	std::array<uint32_t, kBracketVertexCount> out{};
	for (int i = 0; i < 4; ++i) out[static_cast<size_t>(i)] = outer;
	for (int i = 4; i < kBracketVertexCount; ++i)
		out[static_cast<size_t>(i)] = inner;
	return out;
}

// THE MAP SCALE, metres per screen pixel: zoom / (diameter * 200)
// [orig: HUD_DrawMapOverlay @0x5A6501 — fild (x2s-x1s) @0x5a64eb, fmul 200.0
//  @0x5a6505, fdivr the zoom @0x5a650b].
//
// The divisor is the DISC's own diameter, and with the spin flag set (the
// spinmap always passes it) the x extents are first replaced by cx -/+ halfH
// [orig: the test @0x5a63bf, the replace @0x5a6411-0x5a6433]. So the diameter
// is taken from the HEIGHT, and the covered world span works out to
// zoom / 400 — 163.84 m of radius at the default zoom, independent of
// resolution, aspect and the authored rect size. A scale derived from the
// WIDTH would change the world span with the window, which retail's does not.
inline constexpr float kMapZoomDivisor = 200.0f;
inline constexpr float kMapDefaultZoom = 65536.0f;

inline float map_scale_per_px(float zoom, float diameter_px) {
	if (diameter_px <= 0.0f) return 0.0f;
	return zoom / (diameter_px * kMapZoomDivisor);
}

// The world radius the disc covers, for a given zoom: zoom / 400.
inline float map_world_radius(float zoom) {
	return zoom / (2.0f * kMapZoomDivisor);
}

} // namespace opennova::hud
