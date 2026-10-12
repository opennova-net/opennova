#include <editor/preview/terrain_brush_shapes.h>

#include <algorithm>
#include <cmath>

namespace opennova::editor {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// A ring of `radius` metres about `centre`, each point on the ground (or at `flat_height`), as lines on the picture.
void ring(const TerrainBrushSpace &space, TerrainPoint centre, double radius, bool flat, double flat_height, uint32_t rgb,
          float thickness, uint8_t alpha, OverlayList &out) {
	CanvasPoint last;
	bool have_last = false;
	for (int i = 0; i <= kTerrainRingPoints; ++i) {
		const double a = kTwoPi * double(i % kTerrainRingPoints) / double(kTerrainRingPoints);
		const TerrainPoint at{centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)};
		CanvasPoint p;
		const bool shown = space.to_picture(at, flat ? flat_height : space.height(at), p.x, p.y);
		if (shown && have_last) out.line(last, p, rgb, thickness, OverlayRole::Normal, alpha);
		have_last = shown;
		last = p;
	}
}

} // namespace

void terrain_brush_ring(const TerrainBrushSpace &space, const TerrainBrush &brush, TerrainPoint centre, bool flat,
                        double flat_height, OverlayList &out) {
	const double radius = std::max(0.0, double(brush.radius));
	ring(space, centre, radius, flat, flat_height, kTerrainBrushRgb, 2.0f, 255, out);
	const double inner = radius * (1.0 - std::clamp(double(brush.falloff), 0.0, 1.0));
	if (inner > 0.0 && inner < radius) ring(space, centre, inner, flat, flat_height, kTerrainBrushSoftRgb, 1.0f, 140, out);
	CanvasPoint at;
	if (space.to_picture(centre, flat ? flat_height : space.height(centre), at.x, at.y))
		out.marker(at, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, kTerrainBrushRgb);
}

void terrain_brush_circle(const TerrainBrushSpace &space, const TerrainBrush &brush, TerrainPoint centre, OverlayList &out) {
	CanvasPoint at;
	if (!space.to_picture(centre, 0.0, at.x, at.y)) return;
	const double scale = space.pixels_per_metre();
	const double radius = std::max(0.0, double(brush.radius));
	out.ring(at, float(std::max(1.0, radius * scale)), OverlayRole::Normal, 2.0f, kTerrainBrushRgb);
	const double inner = radius * (1.0 - std::clamp(double(brush.falloff), 0.0, 1.0));
	if (inner > 0.0 && inner < radius) out.ring(at, float(inner * scale), OverlayRole::Normal, 1.0f, kTerrainBrushSoftRgb, 140);
	out.marker(at, OverlayGlyph::Cross, 5.0f, OverlayRole::Normal, kTerrainBrushRgb);
}

void terrain_tile_square(const TerrainBrushSpace &space, TerrainPoint corner, int rotation, bool flip_h, bool flip_v,
                         OverlayRole role, OverlayList &out) {
	const double side = kTerrainTileUnits;
	// The square's corners counter-clockwise from the south-west, each side cut in four.
	const TerrainPoint corners[4] = {
		corner, {corner.x + side, corner.y}, {corner.x + side, corner.y + side}, {corner.x, corner.y + side}};
	const uint32_t rgb = role == OverlayRole::Normal ? kTerrainBrushRgb : 0xFFFFFF;
	for (int s = 0; s < 4; ++s) {
		const TerrainPoint a = corners[s], b = corners[(s + 1) % 4];
		CanvasPoint last;
		bool have = false;
		for (int k = 0; k <= 4; ++k) {
			const TerrainPoint at{a.x + (b.x - a.x) * k / 4.0, a.y + (b.y - a.y) * k / 4.0};
			CanvasPoint p;
			const bool shown = space.to_picture(at, space.planar() ? 0.0 : space.height(at), p.x, p.y);
			if (shown && have) out.line(last, p, rgb, 1.5f, role);
			have = shown;
			last = p;
		}
	}
	// The tick: the corner the tile's first texel lands on (an unturned tile's south-west), turned and flipped as the
	// tile is (preview/tile_tools tile_stamp_flags: the brush's quarters clockwise, so the corner goes west, then north).
	int at = (4 - ((rotation % 4) + 4) % 4) % 4;
	if (flip_h) at = (at == 0 ? 1 : at == 1 ? 0 : at == 2 ? 3 : 2);
	if (flip_v) at = 3 - at;
	const TerrainPoint tick = corners[at];
	const TerrainPoint inward{tick.x + (corners[(at + 2) % 4].x - tick.x) * 0.2, tick.y + (corners[(at + 2) % 4].y - tick.y) * 0.2};
	CanvasPoint p, q;
	if (space.to_picture(tick, space.planar() ? 0.0 : space.height(tick), p.x, p.y) &&
	    space.to_picture(inward, space.planar() ? 0.0 : space.height(inward), q.x, q.y))
		out.line(p, q, rgb, 2.5f, role);
}

} // namespace opennova::editor
