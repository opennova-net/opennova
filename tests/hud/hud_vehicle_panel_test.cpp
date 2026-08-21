// The mounted-vehicle panel policy: seat-marker colour banding and geometry.
// [orig: HUD_DrawVehicleHealthBars @0x5A4FD0]

#include <hud/hud_vehicle_panel.h>

#include <cstdio>

using namespace opennova::hud;

namespace {
int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }        \
	} while (0)

void test_bands() {
	// The Q16 ratio thresholds: > 0xC000 good, > 0x6FFF middle, else bad.
	// 0xC000 is 0.75 and 0x6FFF just under 0.4375.
	CHECK(seat_health_band(100, 100) == SeatHealthBand::Good, "full health is good");
	CHECK(seat_health_band(80, 100) == SeatHealthBand::Good, "0.80 is good");
	CHECK(seat_health_band(50, 100) == SeatHealthBand::Middle, "0.50 is middle");
	CHECK(seat_health_band(20, 100) == SeatHealthBand::Bad, "0.20 is bad");
	CHECK(seat_health_band(0, 100) == SeatHealthBand::Bad, "zero health is bad");

	// Exactly 0.75 is NOT good -- the test is strictly greater.
	CHECK(seat_health_band(75, 100) == SeatHealthBand::Middle,
			"exactly 0.75 falls to middle, the compare is strict");

	// A zero max is forced to 1 before the divide rather than crashing.
	CHECK(seat_health_band(0, 0) == SeatHealthBand::Bad,
			"zero max health does not divide by zero");
	CHECK(seat_health_band(5, 0) == SeatHealthBand::Good,
			"zero max health treats any positive health as full");

	// THE WITNESSED ASYMMETRY: the good test is UNSIGNED, so a negative ratio
	// wraps large and reads GOOD instead of falling through to bad. This is
	// the behaviour that a tidy signed-everywhere rewrite would silently lose.
	CHECK(seat_health_band(-1, 100) == SeatHealthBand::Good,
			"negative health wraps unsigned and reads as good");
}

void test_geometry() {
	CHECK(kSeatMarkerW == 11 && kSeatMarkerH == 11, "the marker box is 11x11");

	int x0, y0, x1, y1;
	seat_marker_rect(100, 200, 5, 7, x0, y0, x1, y1);
	CHECK(x0 == 105 && y0 == 207, "the rect starts at base + slot offset");
	CHECK(x1 == 116 && y1 == 218, "and spans one marker box");

	// The label centres in the box; 11/2 floors to 5, not 5.5.
	CHECK(seat_label_x(100, 5) == 110, "label x centres at floor(w/2)");
	CHECK(seat_label_y(200, 7) == 212, "label y centres at floor(h/2)");
}
} // namespace

int main() {
	test_bands();
	test_geometry();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_vehicle_panel_test OK\n");
	return 0;
}
