// Retail's network position smoother: the snap/dead-band classification, the
// non-monotonic duration ladder, the rounded increment and the vertical
// softening.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0, block @0x4B42A7..0x4B458A]

#include <world/net_position_smoother.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// A 2 m correction is a teleport, not a walk; a 0.1 m one is noise.
void test_classification() {
	CHECK(smooth_classify(kSmoothSnapAbove + 1) == SmoothOutcome::Snap,
			"above 2.0 m snaps");
	CHECK(smooth_classify(kSmoothSnapAbove) != SmoothOutcome::Snap,
			"exactly at the bound does NOT snap — the test is strictly greater");
	CHECK(smooth_classify(kSmoothDeadBelow - 1) == SmoothOutcome::DeadBand,
			"below 0.167 m is ignored");
	CHECK(smooth_classify(kSmoothDeadBelow) != SmoothOutcome::DeadBand,
			"exactly at the dead bound is NOT dead");
	CHECK(smooth_classify(kSmoothOne) == SmoothOutcome::Smooth,
			"a 1 m error smooths");
}

// THE LADDER IS NOT MONOTONIC. It climbs 6,7,8,9 then DROPS to 7 at 0x7000
// before climbing again. A tidied monotonic ladder would change the smoothing
// window across the mid-range, which is where ordinary movement error lands.
void test_ladder_is_not_monotonic() {
	CHECK(smooth_duration_ticks(0x2000) == 6, "under 0x3000 -> 6 ticks");
	CHECK(smooth_duration_ticks(0x3500) == 7, "under 0x4000 -> 7");
	CHECK(smooth_duration_ticks(0x4500) == 8, "under 0x5000 -> 8");
	CHECK(smooth_duration_ticks(0x5500) == 9, "under 0x6000 -> 9");
	// THE DROP.
	CHECK(smooth_duration_ticks(0x6500) == 7,
			"under 0x7000 drops BACK to 7 — the ladder is non-monotonic");
	CHECK(smooth_duration_ticks(0x6500) < smooth_duration_ticks(0x5500),
			"the 0x7000 band is SHORTER than the one below it");
	// Then it climbs again.
	CHECK(smooth_duration_ticks(0x7500) == 8, "then 8");
	CHECK(smooth_duration_ticks(0x9000) == 10, "then 10");
	CHECK(smooth_duration_ticks(0xFFFF) == 16, "up to 16");
	CHECK(smooth_duration_ticks(0x18000) == kSmoothLadderTop,
			"past the last band, 18");

	// The bands are BELOW tests, so a value exactly on a threshold takes the
	// NEXT band, not that one.
	CHECK(smooth_duration_ticks(0x3000) == 7,
			"exactly on a threshold falls to the next band");
}

// The increment rounds half up, via truncating integer division.
void test_increment_rounding() {
	// 100 over 8 ticks: (100 + 4) / 8 = 13, not 12.
	CHECK(smooth_increment(100, 8) == 13, "rounds half up");
	// An exact division is unchanged.
	CHECK(smooth_increment(80, 8) == 10, "an exact split is exact");
	// A zero error yields a zero step.
	CHECK(smooth_increment(0, 8) == 0, "no error, no step");
	// A zero duration is inert rather than dividing by zero.
	CHECK(smooth_increment(100, 0) == 0, "a zero duration is inert");

	// The increment is CONSTANT across the window — that constancy is what
	// keeps the first-person weapon lead's first difference at zero.
	const int32_t a = smooth_increment(1000, 10);
	const int32_t b = smooth_increment(1000, 10);
	CHECK(a == b, "the same goal yields the same step every tick");
}

// The vertical softening halves by ARITHMETIC SHIFT — halving with a divide
// rounds the wrong way for a negative z.
void test_vertical_softening() {
	// A large vertical error passes through untouched.
	CHECK(smooth_soften_z(1000, kSmoothOne) == 1000, "a big drop is unsoftened");
	// A mid error halves.
	CHECK(smooth_soften_z(1000, 0x4000) == 500, "a small drop halves");
	// A tiny error zeroes entirely.
	CHECK(smooth_soften_z(1000, 0x1000) == 0, "a tiny drop is dropped");
	// The magnitude is what is tested, so negatives behave the same.
	CHECK(smooth_soften_z(1000, -0x1000) == 0, "negative errors test magnitude");
	CHECK(smooth_soften_z(1000, -0x4000) == 500, "and halve the same way");
	// The shift floors toward negative infinity for a negative increment:
	// -3 >> 1 == -2, where -3/2 would be -1.
	CHECK(smooth_soften_z(-3, 0x4000) == -2,
			"the halving is an arithmetic SHIFT, not a divide");
}

// The position stops advancing at the duration, but the counter keeps climbing
// to its own separate ceiling.
void test_counter_and_stepping() {
	CHECK(smooth_should_step(0, 8), "a fresh goal steps");
	CHECK(smooth_should_step(7, 8), "up to the last tick");
	CHECK(!smooth_should_step(8, 8), "and stops at the duration");
	CHECK(!smooth_should_step(50, 8), "staying stopped after");

	CHECK(smooth_advance_counter(0) == 1, "the counter climbs");
	CHECK(smooth_advance_counter(kSmoothCounterMax - 1) == kSmoothCounterMax,
			"up to its ceiling");
	CHECK(smooth_advance_counter(kSmoothCounterMax) == kSmoothCounterMax,
			"and saturates rather than wrapping");
	// The ceiling is far beyond any ladder duration — the two limits are
	// independent.
	CHECK(kSmoothCounterMax > kSmoothLadderTop,
			"the counter ceiling outlives every smoothing window");
}

} // namespace

int main() {
	test_classification();
	test_ladder_is_not_monotonic();
	test_increment_rounding();
	test_vertical_softening();
	test_counter_and_stepping();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("net_position_smoother_test OK\n");
	return 0;
}
