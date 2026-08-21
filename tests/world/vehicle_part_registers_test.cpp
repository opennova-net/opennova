// Vehicle part-animation registers: rotor spin, the three-case motor clamp,
// the speed-dependent steer rate and the wheel phase.
// [orig: RotorSpin_Update @0x4928B0; the motor @0x48C1C6..0x48C32A; the steer
//  @0x48C0C5..0x48C14E]

#include <world/vehicle_part_registers.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// A rotor spins up and down at the SAME rate, so it takes as long to stop as
// to start.
void test_rotor_symmetry() {
	int32_t up = 0;
	for (int i = 0; i < 5; ++i) up = rotor_step(up, true);
	CHECK(up == kRotorAccel * 5, "spin-up is linear");

	int32_t down = up;
	for (int i = 0; i < 5; ++i) down = rotor_step(down, false);
	CHECK(down == 0, "spin-down takes the same number of ticks");

	// Both ends clamp rather than running away.
	int32_t fast = kRotorSpeedMax;
	CHECK(rotor_step(fast, true) == kRotorSpeedMax, "spin-up caps");
	CHECK(rotor_step(0, false) == 0, "spin-down floors at zero");
	// A partial step down does not undershoot past zero.
	CHECK(rotor_step(kRotorAccel / 2, false) == 0, "the last step lands on zero");
}

// The PANM track reads the HIGH WORD, so the accumulator carries far more
// precision than the animation samples.
void test_register_is_the_high_word() {
	CHECK(part_register(0x00010000) == 1, "bit 16 is register 1");
	CHECK(part_register(0x0001FFFF) == 1,
			"the low word is precision the track never sees");
	CHECK(part_register(0x00020000) == 2, "and rolls at the word boundary");
	// A negative accumulator's high word wraps as unsigned — the steer track
	// relies on that.
	CHECK(part_register(-0x10000) == 0xFFFF, "negatives wrap into the word");
}

// THE THREE CLAMP CASES ARE NOT INTERCHANGEABLE.
void test_motor_clamp_cases() {
	// A REVERSAL integrates UNCLAMPED, so reverse bites immediately.
	const int32_t big = ground_speed_accel(-1000, 1000);
	CHECK(ground_speed_clamp(big, -1000, 1000, 5, 5) == big,
			"a reversal ignores both limits");

	// A same-sign drive clamps to ACCELERATION.
	const int32_t drive = ground_speed_accel(1000, 100);
	CHECK(ground_speed_clamp(drive, 1000, 100, 5, 99) == 5,
			"a drive clamps to the acceleration limit");

	// A ZERO target clamps to DECELERATION — a different field.
	const int32_t brake = ground_speed_accel(0, 1000);
	CHECK(ground_speed_clamp(brake, 0, 1000, 99, 7) == -7,
			"braking clamps to the DECELERATION limit, not acceleration");
	CHECK(ground_speed_clamp(brake, 0, 1000, 99, 7) !=
					ground_speed_clamp(brake, 1000, 1000, 99, 7),
			"the two limits give different results — they are not one clamp");
}

// A coasting vehicle SNAPS to a stop rather than creeping forever.
void test_creep_snap() {
	// Below the snap while coasting -> zero.
	CHECK(ground_speed_step(20, 0, 1000, 1000) == 0,
			"a slow coast snaps to a stop");
	// The snap applies ONLY on a zero target — under power it keeps its speed.
	CHECK(ground_speed_step(20, 5000, 1000, 1000) != 0,
			"under power a slow vehicle does not snap to zero");
	// A zero acceleration lands exactly on the target instead of stalling short.
	CHECK(ground_speed_step(100, 100, 1000, 1000) == 100,
			"a zero step snaps to the target");
}

// The turn rate falls off with speed: full at rest, min at top speed.
void test_steer_rate_falloff() {
	CHECK(steer_min_rate(400, 0) == 100, "no turn_rate2 gives a quarter");
	CHECK(steer_min_rate(400, 250) == 250, "turn_rate2 wins when present");

	CHECK(steer_speed_factor(0, 1000) == 0x10000, "at rest the factor is full");
	CHECK(steer_speed_factor(1000, 1000) == 0, "at top speed it is zero");
	CHECK(steer_speed_factor(2000, 1000) == 0, "and never goes negative");
	CHECK(steer_speed_factor(500, 1000) > 0, "mid-speed is in between");

	// At rest -> the full turn rate; at top speed -> the minimum.
	const int32_t min_rate = steer_min_rate(400, 0);
	CHECK(steer_effective_rate(400, min_rate, 0x10000) == 400,
			"at rest a vehicle turns at its full rate");
	CHECK(steer_effective_rate(400, min_rate, 0) == min_rate,
			"at top speed it turns at the minimum");
	// A zero player_speed does not divide by zero.
	CHECK(steer_speed_factor(500, 0) == 0x10000, "a zero top speed is inert");
}

// Wheels advance from speed; the slip kick is a recorded gap a peer runs at 0.
void test_wheel_phase() {
	CHECK(wheel_phase_step(0, 1, 0) == (1 << 13), "speed drives the phase");
	CHECK(wheel_phase_step(0, 0, 0) == 0, "a stopped wheel does not turn");
	// The slip kick adds on top when present.
	CHECK(wheel_phase_step(0, 1, 500) == 500 + (1 << 13),
			"an active skid kicks the phase");
	// Aircraft wheels ride the forward command instead.
	CHECK(air_wheel_phase_step(0, 3) == (3 << 13), "air wheels use the command");
}

} // namespace

int main() {
	test_rotor_symmetry();
	test_register_is_the_high_word();
	test_motor_clamp_cases();
	test_creep_snap();
	test_steer_rate_falloff();
	test_wheel_phase();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_part_registers_test OK\n");
	return 0;
}
