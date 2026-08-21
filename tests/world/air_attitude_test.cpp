// The air-vehicle attitude integrator: sideslip feedback, the two-gain pitch
// pick, self-level, the ASYMMETRIC damp and the two ceilings.
// [orig: Entity_UpdateAircraftPhysics @0x490310]

#include <world/air_attitude.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// Feedback applies ONLY when the measured slip exceeds the command — a hull
// doing what it was told gets no correction at all.
void test_lateral_feedback_gate() {
	CHECK(sideslip_roll_delta(10, 100) == 0,
			"tracking the command produces no roll correction");
	CHECK(sideslip_roll_delta(100, 100) == 0,
			"exactly on command is not 'exceeding' it");
	CHECK(sideslip_roll_delta(200, 100) != 0, "sliding wide corrects");

	// Gain 8 on the difference, and the sign opposes the slide.
	CHECK(sideslip_roll_delta(200, 100) == 8 * 100 - 8 * 200,
			"the correction is 8 * (command - measured)");
	CHECK(sideslip_roll_delta(200, 100) < 0, "a positive slide rolls negative");
	// Magnitude is what gates it, so a negative slide corrects too.
	CHECK(sideslip_roll_delta(-200, 100) != 0, "a negative slide also corrects");
}

// THE PITCH FEEDBACK PICKS BETWEEN TWO GAIN PAIRS by asking whether the big
// step would move the pitch CLOSER TO ZERO. It is a levelling test, not a
// magnitude limit.
void test_pitch_two_gain_pick() {
	// Below the gate: nothing.
	CHECK(sideslip_pitch_delta(10, 100, 0).rate_delta == 0,
			"tracking the command produces no pitch correction");

	// A correction that LEVELS the hull takes the STRONG pair (<<4 / <<5).
	// pitch = 10000, measured 200 vs command 100 -> d = 100, d<<5 = 3200,
	// |10000 + 3200| > |10000| so this is NOT levelling -> weak pair.
	{
		const SideslipPitch weak = sideslip_pitch_delta(200, 100, 10000);
		CHECK(weak.rate_delta == (100 << 2) && weak.angle_delta == (100 << 2),
				"a correction that pitches FURTHER takes the weak pair");
	}
	// Same magnitude but the pitch is negative, so the positive step levels it:
	// pitch = -10000, d = 100, -10000 + 3200 = -6800, |−6800| < |−10000|.
	{
		const SideslipPitch strong = sideslip_pitch_delta(200, 100, -10000);
		CHECK(strong.rate_delta == (100 << 4) && strong.angle_delta == (100 << 5),
				"a correction that LEVELS takes the strong pair");
	}
	// The two pairs are genuinely different — one gain would collapse them.
	CHECK(sideslip_pitch_delta(200, 100, -10000).angle_delta !=
					sideslip_pitch_delta(200, 100, 10000).angle_delta,
			"the two gain pairs differ");
	// The strong pair's rate and angle shifts also differ from each other.
	const SideslipPitch s = sideslip_pitch_delta(200, 100, -10000);
	CHECK(s.rate_delta != s.angle_delta,
			"the strong pair uses DIFFERENT shifts for rate and angle");
}

// The self-level pull is symmetric about zero thanks to the round term.
void test_self_level_symmetry() {
	CHECK(self_level_delta(0) == 0, "level stays level");
	CHECK(self_level_delta(1024) > 0, "a positive pitch pulls down");
	CHECK(self_level_delta(-1024) < 0, "a negative pitch pulls up");
	// The +0x100 round term keeps the two directions matched in magnitude.
	CHECK(self_level_delta(1024) == -self_level_delta(-1024) ||
					self_level_delta(1024) + self_level_delta(-1024) <= 1,
			"the pull is symmetric within the rounding term");
}

// AIRBORNE DAMPS A SIXTEENTH, GROUNDED AN EIGHTH — a grounded hull settles
// twice as fast. The damp is NOT symmetric; see below.
void test_rate_damp() {
	const int32_t r = 16000;
	const int32_t air = rate_damp(r, true);
	const int32_t ground = rate_damp(r, false);
	CHECK(air < r && ground < r, "both damp toward zero");
	CHECK(r - ground > r - air, "grounded damps harder than airborne");

	// THE DAMP IS NOT SYMMETRIC. At large rates the negative side moves one
	// step further, because the shift truncates toward zero on the positive
	// side and the >> 31 term biases the negative one.
	const int32_t pos = r - rate_damp(r, true);
	const int32_t neg = rate_damp(-r, true) - (-r);
	CHECK(neg == pos + 1, "the negative side decays one step further");

	// AND THE CONSEQUENCE THAT MATTERS: a positive rate SMALLER than the
	// divisor produces a zero step and never decays at all, while its negative
	// mirror reaches zero. A hull left alone settles from below and creeps
	// from above.
	CHECK(rate_damp(7, true) == 7, "a small POSITIVE rate is stuck");
	CHECK(rate_damp(1, true) == 1, "even at 1");
	CHECK(rate_damp(-7, true) == -6, "a small negative rate keeps decaying");
	CHECK(rate_damp(-1, true) == 0, "and reaches zero");

	// A settled rate stays settled.
	CHECK(rate_damp(0, true) == 0, "zero stays zero");
	CHECK(rate_damp(0, false) == 0, "on the ground too");
}

// The def ceiling is OPTIONAL and inactive for every shipped aircraft; the
// global clamp is what actually binds.
void test_ceilings() {
	CHECK(apply_def_rate_ceiling(999999, 0) == 999999,
			"a zero def key leaves the rate alone");
	CHECK(apply_def_rate_ceiling(999999999, 1) == kAttitudeDefRateUnit,
			"a set key clamps to unit * key");
	CHECK(apply_def_rate_ceiling(-999999999, 1) == -kAttitudeDefRateUnit,
			"symmetrically");

	CHECK(apply_global_rate_clamp(999999999) == kAttitudeRateClamp,
			"the global clamp binds at 15 deg/tick");
	CHECK(apply_global_rate_clamp(-999999999) == -kAttitudeRateClamp,
			"in both directions");
	CHECK(apply_global_rate_clamp(1000) == 1000, "and leaves small rates alone");
}

} // namespace

int main() {
	test_lateral_feedback_gate();
	test_pitch_two_gain_pick();
	test_self_level_symmetry();
	test_rate_damp();
	test_ceilings();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("air_attitude_test OK\n");
	return 0;
}
