// The map target bracket mesh and the map scale derivation.
// [orig: FUN_0059BCB0 @0x59BCB0; HUD_DrawMapOverlay @0x5A6501]

#include <hud/hud_map_bracket.h>

#include <cmath>
#include <cstdio>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// The bracket is THREE QUADS, not an outline. A 100x100 rect at the origin
// gives inset 12.5 and centre 50.
void test_bracket_geometry() {
	const auto v = bracket_vertices(0.0f, 0.0f, 100.0f, 100.0f);

	// 0..3: the outer frame, in the original's corner order — (x2,y2) first.
	CHECK(near(v[0].x, 100.0f) && near(v[0].y, 100.0f), "v0 is the far corner");
	CHECK(near(v[1].x, 0.0f) && near(v[1].y, 100.0f), "v1 is x1,y2");
	CHECK(near(v[2].x, 100.0f) && near(v[2].y, 0.0f), "v2 is x2,y1");
	CHECK(near(v[3].x, 0.0f) && near(v[3].y, 0.0f), "v3 is the near corner");

	// 4..7: the horizontal band straddles the centre in x, inset in y.
	CHECK(near(v[4].x, 62.5f) && near(v[4].y, 87.5f), "v4 straddles centre x");
	CHECK(near(v[5].x, 37.5f), "v5 is the other side of centre");

	// 8..11: the vertical band straddles the centre in y, inset in x.
	CHECK(near(v[8].x, 87.5f) && near(v[8].y, 62.5f), "v8 straddles centre y");
	CHECK(near(v[11].x, 12.5f) && near(v[11].y, 37.5f), "v11 is its opposite");

	// The inset is an EIGHTH of the rect's own size, so it scales with it.
	const auto big = bracket_vertices(0.0f, 0.0f, 800.0f, 800.0f);
	CHECK(near(big[4].x, 500.0f), "the inset scales with the rect");
}

// A non-square rect must inset x and y INDEPENDENTLY — using one factor for
// both would skew the bands on a wide target.
void test_bracket_non_square() {
	const auto v = bracket_vertices(0.0f, 0.0f, 400.0f, 100.0f);
	// dx = 50, dy = 12.5, cx = 200, cy = 50.
	CHECK(near(v[4].x, 250.0f), "x inset uses the width");
	CHECK(near(v[4].y, 87.5f), "y inset uses the height, not the width");
	CHECK(near(v[8].x, 350.0f), "and the vertical band likewise");
}

// The index list pairs specific vertices; it must stay the witnessed run.
void test_bracket_indices() {
	CHECK(kBracketIndices.size() == 18, "three quads, two triangles each");
	// Each quad repeats the same winding at +4.
	for (int q = 0; q < 3; ++q) {
		const int b = q * 6;
		const int o = q * 4;
		CHECK(kBracketIndices[static_cast<size_t>(b + 0)] == o + 0 &&
						kBracketIndices[static_cast<size_t>(b + 1)] == o + 1 &&
						kBracketIndices[static_cast<size_t>(b + 2)] == o + 2,
				"first triangle of each quad");
		CHECK(kBracketIndices[static_cast<size_t>(b + 3)] == o + 1 &&
						kBracketIndices[static_cast<size_t>(b + 4)] == o + 3 &&
						kBracketIndices[static_cast<size_t>(b + 5)] == o + 2,
				"second triangle of each quad");
	}
}

// Outer band WHITE, the eight inner vertices RED. A single-colour bracket
// loses the read entirely.
void test_bracket_colors() {
	const auto c = bracket_colors(0xFF);
	for (int i = 0; i < 4; ++i)
		CHECK(c[static_cast<size_t>(i)] == 0xFFFFFFFFu, "the outer band is white");
	for (int i = 4; i < 12; ++i)
		CHECK(c[static_cast<size_t>(i)] == 0xFFFF0000u, "the inner bands are red");

	// Alpha rides the top byte and clamps rather than wrapping.
	CHECK((bracket_colors(0x80)[0] >> 24) == 0x80u, "alpha rides the top byte");
	CHECK((bracket_colors(300)[0] >> 24) == 0xFFu, "over-range alpha clamps");
	CHECK((bracket_colors(-5)[0] >> 24) == 0x00u, "negative alpha clamps");
}

// THE SCALE IS RESOLUTION-INDEPENDENT. The world span covered by the disc is
// zoom/400 regardless of how big the disc is drawn — a scale derived from the
// width would change the covered world with the window, which retail's doesn't.
void test_map_scale() {
	CHECK(near(map_world_radius(kMapDefaultZoom), 163.84f),
			"the default zoom covers 163.84 m of radius");
	CHECK(near(map_world_radius(kMapDefaultZoom) * 2.0f, 327.68f),
			"327.68 m across");

	// Same zoom, different disc sizes: metres-per-pixel changes, world span
	// does not.
	const float small = map_scale_per_px(kMapDefaultZoom, 128.0f);
	const float large = map_scale_per_px(kMapDefaultZoom, 256.0f);
	CHECK(small > large, "a smaller disc packs more metres into each pixel");
	CHECK(near(small * 128.0f, large * 256.0f),
			"but both cover the same world span");

	// Halving the zoom halves the covered world.
	CHECK(near(map_world_radius(kMapDefaultZoom / 2.0f),
					map_world_radius(kMapDefaultZoom) / 2.0f),
			"zoom scales the world radius linearly");

	// A degenerate diameter yields 0 rather than dividing by zero.
	CHECK(map_scale_per_px(kMapDefaultZoom, 0.0f) == 0.0f, "zero diameter is inert");
}

} // namespace

int main() {
	test_bracket_geometry();
	test_bracket_non_square();
	test_bracket_indices();
	test_bracket_colors();
	test_map_scale();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_map_bracket_test OK\n");
	return 0;
}
