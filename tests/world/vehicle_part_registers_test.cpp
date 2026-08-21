// Vehicle part-animation registers: the rotor spin machine, the PRNG-rolled
// rate for non-player-control items, the high-word register and the wheel
// phase.
// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0; the spawn seed
//  @0x40EE70..0x40EE94; the register reads @0x492ACA / @0x4929B4]

#include <world/vehicle_part_registers.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// A player-control rotor spins up and down at the SAME rate, so it takes as
// long to stop as to start — and it never touches the PRNG.
void test_player_control_rotor_symmetry() {
	RotorState s;
	CHECK(!rotor_rate_needs_roll(s, kItemAttribPlayerControl),
			"a player-control item never rolls");
	rotor_seed_rate(s, kItemAttribPlayerControl, true, 0);
	CHECK(s.rate == kRotorRateFull, "occupied: the full rate");
	for (int i = 0; i < 5; ++i) rotor_tick(s, true);
	CHECK(s.speed == kRotorRateFull * 5, "spin-up is linear");
	CHECK(s.angle == kRotorRateFull * (1 + 2 + 3 + 4 + 5),
			"the angle accumulates the speed every tick");

	for (int i = 0; i < 5; ++i) rotor_tick(s, false);
	CHECK(s.speed == 0, "spin-down takes the same number of ticks");
	CHECK(s.rate == 0, "an unoccupied tick clears the rate");

	// Unoccupied, a player-control item seeds NOTHING.
	RotorState idle;
	rotor_seed_rate(idle, kItemAttribPlayerControl, false, 0);
	CHECK(idle.rate == 0, "unoccupied player-control seeds no rate");

	// Both ends clamp rather than running away.
	RotorState fast;
	fast.rate = kRotorRateFull;
	fast.speed = kRotorSpeedMax;
	rotor_tick(fast, true);
	CHECK(fast.speed == kRotorSpeedMax, "spin-up caps");
	RotorState low;
	low.speed = kRotorRateFull / 2;
	rotor_tick(low, false);
	CHECK(low.speed == 0, "the last step down lands on zero, not below");
}

// A NON-player-control item rolls its spin-up rate from the shared PRNG
// stream: three rates by percentage band, and it re-rolls every unoccupied
// tick because the rate is reset to zero.
void test_rolled_rate() {
	CHECK(rotor_rate_from_roll(0) == kRotorRateFull, "0% -> full");
	CHECK(rotor_rate_from_roll(33) == kRotorRateFull, "33 -> full (> 33 is the edge)");
	CHECK(rotor_rate_from_roll(34) == kRotorRateMid, "34 -> mid");
	CHECK(rotor_rate_from_roll(66) == kRotorRateMid, "66 -> mid (> 66 is the edge)");
	CHECK(rotor_rate_from_roll(67) == kRotorRateLow, "67 -> low");
	CHECK(rotor_rate_from_roll(99) == kRotorRateLow, "99 -> low");
	CHECK(rotor_rate_from_roll(100) == kRotorRateFull, "the roll is % 100");

	RotorState s;
	CHECK(rotor_rate_needs_roll(s, 0), "zero rate on a non-player item rolls");
	rotor_seed_rate(s, 0, false, kRotorRateLow);
	CHECK(s.rate == kRotorRateLow, "the rolled rate is taken even unoccupied");
	CHECK(!rotor_rate_needs_roll(s, 0), "a seeded rate does not roll again");
	rotor_tick(s, false);
	CHECK(s.rate == 0 && rotor_rate_needs_roll(s, 0),
			"an unoccupied tick resets the rate, so the next tick rolls again");

	// Spin-up at the rolled rate, spin-down at the FULL rate: a slow-start
	// rotor stops faster than it started.
	RotorState r;
	rotor_seed_rate(r, 0, true, kRotorRateLow);
	rotor_tick(r, true);
	CHECK(r.speed == kRotorRateLow, "spin-up uses the rolled rate");
	rotor_tick(r, false);
	CHECK(r.speed == 0, "spin-down uses the full rate and lands on zero");
}

// The spawn seed: full speed at once, the full rate armed.
void test_spawn_full() {
	RotorState s;
	rotor_spawn_full(s);
	CHECK(s.speed == kRotorSpeedMax && s.rate == kRotorRateFull,
			"a spawned-in-flight rotor is already at full speed");
	CHECK(kSpawnRotorFullBit == 0x20000, "the record flag");
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

// Wheels advance from speed; the slip kick is a recorded gap a peer runs at 0.
void test_wheel_phase() {
	CHECK(wheel_phase_step(0, 1, 0) == (1 << 13), "speed drives the phase");
	CHECK(wheel_phase_step(0, 0, 0) == 0, "a stopped wheel does not turn");
	// The slip kick adds on top when present.
	CHECK(wheel_phase_step(0, 1, 500) == 500 + (1 << 13),
			"an active skid kicks the phase");
	// Watercraft ride the forward command instead.
	CHECK(watercraft_wheel_phase_step(0, 3) == (3 << 13),
			"watercraft use the brain's forward command");
}

} // namespace

int main() {
	test_player_control_rotor_symmetry();
	test_rolled_rate();
	test_spawn_full();
	test_register_is_the_high_word();
	test_wheel_phase();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_part_registers_test OK\n");
	return 0;
}
