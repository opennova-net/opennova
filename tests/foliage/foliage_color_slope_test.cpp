// Locks in the bounded-stand-in slope→shade mapping used by
// NovaFoliageDispatcher::_append_render_instance. The shade is a temporary
// approximation of spec §4.4.8 color_lower/color_upper band blend until
// §7.2 is decompiled — keeping this test ensures the floor constant
// doesn't drift silently.

#include <foliage/placement.h>

#include <cmath>
#include <cstdio>

using namespace opennova::foliage;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

} // namespace

int main() {
	// Flat ground (|normal.y| = 1) → full brightness.
	if (!expect(near(foliage_slope_shade_from_normal_y(1.0f), 1.0f),
	            "flat ground must return shade 1.0")) return 1;

	// 45° slope (normal tilts to ~0.707) → passes through unclamped.
	if (!expect(near(foliage_slope_shade_from_normal_y(0.70710678f), 0.70710678f),
	            "45° slope must pass through unclamped")) return 1;

	// 60° slope (normal.y = 0.5) → passes through.
	if (!expect(near(foliage_slope_shade_from_normal_y(0.5f), 0.5f),
	            "60° slope must pass through unclamped")) return 1;

	// Steeper than the floor (~66°, normal.y = 0.4): boundary.
	if (!expect(near(foliage_slope_shade_from_normal_y(0.4f), FOLIAGE_SLOPE_SHADE_FLOOR),
	            "normal.y = floor must return floor")) return 1;

	// Very steep / near-vertical: clamped to floor.
	if (!expect(near(foliage_slope_shade_from_normal_y(0.2f), FOLIAGE_SLOPE_SHADE_FLOOR),
	            "steep slopes must clamp to FOLIAGE_SLOPE_SHADE_FLOOR")) return 1;
	if (!expect(near(foliage_slope_shade_from_normal_y(0.0f), FOLIAGE_SLOPE_SHADE_FLOOR),
	            "vertical must clamp to FOLIAGE_SLOPE_SHADE_FLOOR")) return 1;

	// Back-facing normal (negative Y): absolute value is taken internally.
	if (!expect(near(foliage_slope_shade_from_normal_y(-0.5f), 0.5f),
	            "back-facing normal must be abs'd, not floored to black")) return 1;
	if (!expect(near(foliage_slope_shade_from_normal_y(-1.0f), 1.0f),
	            "fully inverted flat ground must still return 1.0")) return 1;

	// Out-of-range (shouldn't happen with normalized inputs, but clamp anyway).
	if (!expect(near(foliage_slope_shade_from_normal_y(1.5f), 1.0f),
	            "over-unity input must clamp to 1.0")) return 1;

	// Floor constant itself: make drift visible if someone changes it.
	if (!expect(near(FOLIAGE_SLOPE_SHADE_FLOOR, 0.4f),
	            "FOLIAGE_SLOPE_SHADE_FLOOR must remain 0.4 — change intentionally")) return 1;

	std::printf("OK: foliage_slope_shade_from_normal_y clamps to [0.4, 1.0] with abs()\n");
	return 0;
}
