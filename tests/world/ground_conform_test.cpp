// The ground-vehicle suspension spring leg: travel, the compress step and the
// two-decay wheel oscillator.
// [orig: Suspension_CompressWheelQuadratic @0x45CFB0; Suspension_OscillateWheelFast @0x45D110
//  (the oscillator, despite the name); the travel derivation
//  @0x47C51F..0x47C544]

#include <world/ground_conform.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The oscillator clamps the def's shock IN PLACE (it takes the field by
// reference); the kernel pins below hand it a throwaway lvalue.
int32_t oscillate(ConformOscillator &osc, int32_t &compression, int32_t &impact,
		int32_t shock, int32_t spring, int32_t entity_a0) {
	return conform_spring_oscillate(osc, compression, impact, shock, spring, entity_a0);
}

// The def's shock (+0x904) is clamped to [0, 10] through the reference, as
// retail writes the clamp back into the item def [orig: @0x45D18F..0x45D1A2].
void test_shock_clamps_in_place() {
	ConformOscillator osc;
	osc.amplitude = 1000;
	int32_t compression = 0, impact = 0;
	int32_t shock = 25;
	(void)conform_spring_oscillate(osc, compression, impact, shock, 3, 0);
	CHECK(shock == 10, "an over-range shock is written back as 10");
	shock = -4;
	(void)conform_spring_oscillate(osc, compression, impact, shock, 3, 0);
	CHECK(shock == 0, "a negative shock is written back as 0");
	shock = 7;
	(void)conform_spring_oscillate(osc, compression, impact, shock, 3, 0);
	CHECK(shock == 7, "an in-range shock is untouched");
}

// travel = 0xFFFF - ftol(0xFFFF * (100 - spring_comp) * 0.01)
void test_travel_from_def() {
	CHECK(conform_travel_from_def(100) == kSuspFull,
			"spring_comp 100 gives full travel");
	CHECK(conform_travel_from_def(0) == 0, "spring_comp 0 gives no travel");
	// 50% lands near half, allowing for the float round-trip.
	const int32_t half = conform_travel_from_def(50);
	CHECK(half > kSuspFull / 2 - 2 && half < kSuspFull / 2 + 2,
			"spring_comp 50 is about half travel");
	// Out-of-range percentages clamp rather than producing nonsense travel.
	CHECK(conform_travel_from_def(150) == kSuspFull, "over 100 clamps");
	CHECK(conform_travel_from_def(-20) == 0, "under 0 clamps");
}

// A wheel already past its travel limit ABSORBS NOTHING — that is what a
// bottomed suspension does, and returning the step anyway would let it keep
// eating energy forever.
void test_compress_bottomed_absorbs_nothing() {
	ConformOscillator osc;
	int32_t compression = 5000;
	int32_t impact = 1000;
	const int32_t applied =
			conform_spring_compress(osc, compression, impact, 100, 4000, 3);
	CHECK(applied == 0, "a bottomed wheel applies no step");
	CHECK(compression == 5000, "and does not compress further");
	CHECK(impact == 1000, "and drains no impact energy");
	CHECK(osc.energy == 0, "its stored energy is dropped");
	CHECK(osc.amplitude == 4000, "amplitude pins at the travel limit");
}

void test_compress_normal() {
	ConformOscillator osc;
	osc.extension = kSuspFull;
	int32_t compression = 0;
	int32_t impact = 100000;
	const int32_t applied =
			conform_spring_compress(osc, compression, impact, 100, 4000, 3);
	CHECK(applied == 100, "an unbottomed wheel applies its step");
	CHECK(compression == 100, "and compresses by it");
	CHECK(osc.extension == kSuspFull - 100, "extension is the complement");
	// The phase is FORCED, not advanced, so a wheel re-entering oscillation
	// always restarts from the same point on the curve.
	CHECK(osc.phase == kOscPhasePreset, "the phase is preset, not advanced");
	// The sink drains by exactly half the doubled product: 2*3*100*100 / 2.
	CHECK(impact == 100000 - 30000, "the impact sink drains spring * step^2");
	// Energy floors at -1 rather than 0.
	CHECK(osc.energy < 0, "energy goes negative under load");
	CHECK(osc.energy == -1, "and floors at -1, not 0");
}

// The doubled product is computed ONCE and wraps as int32 before either half
// is taken — past the wrap the two halves divide the WRAPPED double, which is
// not the same as negating an unwrapped spring * step^2.
void test_spring_term_wraps_as_the_doubled_product() {
	ConformOscillator osc;
	osc.energy = 0x7FFFFFFF; // far from the floor so the value survives
	int32_t compression = 0;
	int32_t impact = 0x7FFFFFFF;
	// 2 * 0x10000 * 0x1000 * 0x1000 = 2^45 -> wraps to 0 as int32.
	conform_spring_compress(osc, compression, impact, 0x1000, 0x7FFFFFFF, 0x10000);
	CHECK(osc.energy == 0x7FFFFFFF, "a wrapped-to-zero product drains nothing");
	CHECK(impact == 0x7FFFFFFF, "from either sink");
}

// The impact sink is ONE-DIRECTIONAL: it drains while positive and is left
// alone once it is not. A two-directional sink would let a vehicle recover
// energy it never had.
void test_impact_sink_is_one_directional() {
	ConformOscillator osc;
	int32_t compression = 0;
	int32_t impact = -50;
	conform_spring_compress(osc, compression, impact, 100, 4000, 3);
	CHECK(impact == -50, "a non-positive impact sink is left untouched");
}

// THE TWO DECAYS. Amplitude decays 0.99 every tick; the (11-shock)/11 damp
// applies ONLY on the tick the wheel lands at zero compression, and then it
// multiplies the amplitude that was just decayed. Conflating them kills the
// bounce far too fast.
void test_oscillator_two_decays() {
	// Mid-bounce (compression will not land at 0): only the 0.99 applies.
	{
		ConformOscillator osc;
		osc.amplitude = 10000;
		osc.phase = 0.0f; // sin(0.2616) > 0, so env > 0.5 -> compression > 0
		int32_t compression = 0;
		int32_t impact = 0;
		oscillate(osc, compression, impact, 10, 3, 0);
		CHECK(compression > 0, "mid-bounce leaves the wheel compressed");
		// 10000 * 0.99 = 9900, with no shock damp.
		CHECK(osc.amplitude == 9900, "only the 0.99 decay applies mid-bounce");
	}

	// A landing (env resolves to 0 compression) also applies the shock damp,
	// on top of the 0.99 — the order is decay first, then (11 - shock)/11.
	{
		ConformOscillator osc;
		osc.amplitude = 10000;
		osc.phase = 4.4509f; // + 0.2617 = 3pi/2 -> sin = -1 -> env = 0
		int32_t compression = 500;
		int32_t impact = 0;
		oscillate(osc, compression, impact, 0, 3, 0);
		CHECK(compression == 0, "the wheel lands");
		// shock 0: (11 - 0) * (1/11) * 9900 = 9900 (the damp is unity).
		CHECK(osc.amplitude == 9900 || osc.amplitude == 9899,
				"a landing with shock 0 keeps the decayed amplitude");
	}
	{
		ConformOscillator osc;
		osc.amplitude = 10000;
		osc.phase = 4.4509f;
		int32_t compression = 500;
		int32_t impact = 0;
		oscillate(osc, compression, impact, 10, 3, 0);
		CHECK(compression == 0, "the wheel lands");
		// shock 10: (11 - 10) * (1/11) * 9900 = 900 — the damp multiplies the
		// DECAYED amplitude, not the original 10000 (which would give 909).
		CHECK(osc.amplitude == 900 || osc.amplitude == 899,
				"the landing damp multiplies the already-decayed amplitude");
	}
	// Shock out of range clamps into [0, 10] before the damp.
	{
		ConformOscillator a, b;
		a.amplitude = b.amplitude = 10000;
		a.phase = b.phase = 4.4509f;
		int32_t ca = 500, cb = 500, ia = 0, ib = 0;
		oscillate(a, ca, ia, 25, 3, 0);
		oscillate(b, cb, ib, 10, 3, 0);
		CHECK(a.amplitude == b.amplitude, "shock above 10 clamps to 10");
	}

	// The entity+0xA0 threshold damps a landing four times harder; -2000
	// itself does NOT (the test is strictly below).
	{
		ConformOscillator plain, edge, damped;
		plain.amplitude = edge.amplitude = damped.amplitude = 10000;
		plain.phase = edge.phase = damped.phase = 4.4509f;
		int32_t cp = 500, ce = 500, cd = 500, ip = 0, ie = 0, id = 0;
		oscillate(plain, cp, ip, 0, 3, 0);
		oscillate(edge, ce, ie, 0, 3, -2000);
		oscillate(damped, cd, id, 0, 3, -2001);
		CHECK(edge.amplitude == plain.amplitude, "-2000 exactly is not below");
		CHECK(damped.amplitude == (plain.amplitude >> 2),
				"below -2000 the landing is damped four times harder");
	}
}

} // namespace

int main() {
	test_travel_from_def();
	test_compress_bottomed_absorbs_nothing();
	test_compress_normal();
	test_spring_term_wraps_as_the_doubled_product();
	test_impact_sink_is_one_directional();
	test_oscillator_two_decays();
	test_shock_clamps_in_place();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ground_conform_test OK\n");
	return 0;
}
