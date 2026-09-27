#pragma once

#include <array>
#include <cstdint>

namespace opennova::hud {

// THE MEDIC CROSS QUAD — a white field with a red cross
// [orig: HUD_DrawMedicCrossQuad @0x59BCB0].
//
// One primitive, three callers: the friendly-tag medic plate
// (HUD_DrawEntityLabel, the call @0x5a436c), the MAP medic marker for a
// teammate whose AnimMap slot 8 is active (HUD_DrawEntityLabelsAndMarkers, the
// call @0x5a4d40), and the help-screen icons (HUD_DrawHelpScreenIcons, the call
// @0x497620). It is NOT a map "target bracket" — an earlier reading of this
// routine (and the file it first landed in, hud_map_bracket.h) mistook the two
// red bars for inset frame bands; they are a vertical and a horizontal bar.
//
// Retail builds TWELVE vertices as three quads and draws them through one
// 18-index list [orig: the index stores @0x59bcd2..0x59bdc8 — {0,1,2, 1,3,2}
// repeated at +4 and +8]:
//   verts 0..3  — the full rect, WHITE           [orig: the corner loop @0x59be3e]
//   verts 4..7  — x = mid_x -/+ dx, y = y1+dy..y2-dy: the VERTICAL bar, RED
//                                                 [orig: @0x59be89..0x59bebe]
//   verts 8..11 — y = mid_y -/+ dy, x = x1+dx..x2-dx: the HORIZONTAL bar, RED
//                                                 [orig: @0x59bec9..0x59bf03]
// with dx = (x2 - x1) / 8 and dy = (y2 - y1) / 8 [orig: the 0.125 immediates
// @0x59bd1c / @0x59bd7e], mid = the 0.5 midpoint [orig: @0x59bdd2 / @0x59bdda].
// The white/red carry the caller's alpha in the high byte [orig: unk_FFFFFF +
// alpha<<24 @0x59be64; unk_FF0000 + alpha<<24 @0x59be76].
//
// Each quad is axis-aligned, so the port carries the three quads directly (the
// {0,1,2,1,3,2} split is the device's quad-to-triangle fan) in the witnessed
// order: the field first, then the bars over it.
//
// Inputs are already-scaled SCREEN coordinates, matching what the original
// receives after the virtual-coordinate transform.

inline constexpr float kMedicCrossInsetFactor = 0.125f;
inline constexpr float kMedicCrossCenterFactor = 0.5f;
inline constexpr int kMedicCrossQuadCount = 3;

struct MedicCrossQuad {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	uint32_t color = 0xFFFFFFFFu; // 0xAARRGGBB
};

// The three quads for a rect, in the witnessed order, with `alpha` as the
// colour high byte (clamped to a byte rather than wrapped).
inline std::array<MedicCrossQuad, kMedicCrossQuadCount> medic_cross_quads(
		float x1, float y1, float x2, float y2, int alpha) {
	if (alpha < 0) alpha = 0;
	if (alpha > 0xFF) alpha = 0xFF;
	const uint32_t a = static_cast<uint32_t>(alpha) << 24;
	const float dx = (x2 - x1) * kMedicCrossInsetFactor;
	const float dy = (y2 - y1) * kMedicCrossInsetFactor;
	const float cx = (x1 + x2) * kMedicCrossCenterFactor;
	const float cy = (y1 + y2) * kMedicCrossCenterFactor;
	return {{
		// verts 0..3 — the white field.
		{x1, y1, x2, y2, a | 0x00FFFFFFu},
		// verts 4..7 — the vertical bar.
		{cx - dx, y1 + dy, cx + dx, y2 - dy, a | 0x00FF0000u},
		// verts 8..11 — the horizontal bar.
		{x1 + dx, cy - dy, x2 - dx, cy + dy, a | 0x00FF0000u},
	}};
}

} // namespace opennova::hud
