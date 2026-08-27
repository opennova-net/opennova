// The medic cross quad: the white field + red cross primitive behind the
// friendly-tag medic plate, the map medic marker, and the help-screen icons.
// [orig: HUD_DrawMedicCrossQuad @0x59BCB0]

#include <runtime/hud/hud_medic_cross.h>

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

// A 100x100 rect at the origin: inset 12.5, centre 50. The field spans the
// whole rect; the bars are a quarter wide and run to an eighth of each end.
void test_geometry() {
	const auto q = medic_cross_quads(0.0f, 0.0f, 100.0f, 100.0f, 0xFF);
	CHECK(q.size() == 3, "three quads");

	CHECK(near(q[0].x0, 0.0f) && near(q[0].y0, 0.0f) && near(q[0].x1, 100.0f) &&
					near(q[0].y1, 100.0f),
			"the field is the whole rect");

	// The VERTICAL bar: x straddles the centre by dx, y inset by dy top and bottom.
	CHECK(near(q[1].x0, 37.5f) && near(q[1].x1, 62.5f), "vertical bar straddles centre x");
	CHECK(near(q[1].y0, 12.5f) && near(q[1].y1, 87.5f), "vertical bar inset an eighth");

	// The HORIZONTAL bar: y straddles the centre by dy, x inset by dx each side.
	CHECK(near(q[2].y0, 37.5f) && near(q[2].y1, 62.5f), "horizontal bar straddles centre y");
	CHECK(near(q[2].x0, 12.5f) && near(q[2].x1, 87.5f), "horizontal bar inset an eighth");

	// The bars are a CROSS, not two inset frames: they meet at the centre.
	CHECK(q[1].x0 < 50.0f && q[1].x1 > 50.0f && q[2].y0 < 50.0f && q[2].y1 > 50.0f,
			"both bars pass through the centre");
}

// A non-square rect insets x and y INDEPENDENTLY — one shared factor would
// skew the bars on a wide marker.
void test_non_square() {
	const auto q = medic_cross_quads(0.0f, 0.0f, 400.0f, 100.0f, 0xFF);
	// dx = 50, dy = 12.5, cx = 200, cy = 50.
	CHECK(near(q[1].x0, 150.0f) && near(q[1].x1, 250.0f), "vertical bar width uses dx");
	CHECK(near(q[1].y0, 12.5f) && near(q[1].y1, 87.5f), "vertical bar ends use dy");
	CHECK(near(q[2].x0, 50.0f) && near(q[2].x1, 350.0f), "horizontal bar ends use dx");
	CHECK(near(q[2].y0, 37.5f) && near(q[2].y1, 62.5f), "horizontal bar height uses dy");
}

// The field is WHITE, the bars RED, and the order is field-then-bars so the
// cross paints over the field.
void test_colors_and_order() {
	const auto q = medic_cross_quads(0.0f, 0.0f, 10.0f, 10.0f, 0xFF);
	CHECK(q[0].color == 0xFFFFFFFFu, "the field is white");
	CHECK(q[1].color == 0xFFFF0000u, "the vertical bar is red");
	CHECK(q[2].color == 0xFFFF0000u, "the horizontal bar is red");

	// Alpha rides the top byte and clamps rather than wrapping.
	CHECK((medic_cross_quads(0, 0, 1, 1, 0x80)[0].color >> 24) == 0x80u,
			"alpha rides the top byte");
	CHECK((medic_cross_quads(0, 0, 1, 1, 300)[0].color >> 24) == 0xFFu,
			"over-range alpha clamps");
	CHECK((medic_cross_quads(0, 0, 1, 1, -5)[0].color >> 24) == 0x00u,
			"negative alpha clamps");
	CHECK((medic_cross_quads(0, 0, 1, 1, 0x80)[1].color & 0x00FFFFFFu) == 0x00FF0000u,
			"the bar keeps its red under any alpha");
}

} // namespace

int main() {
	test_geometry();
	test_non_square();
	test_colors_and_order();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_medic_cross_test OK\n");
	return 0;
}
