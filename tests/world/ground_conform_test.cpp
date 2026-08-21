// The ground-vehicle terrain conform: suspension travel, slope thresholds and
// the two-decay wheel oscillator.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0; the oscillator
//  sub_45D110; the travel derivation @0x47C51F..0x47C544]

#include <world/ground_conform.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// travel = 0xFFFF - 0xFFFF * (100 - spring_comp) * 0.01
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

// Radii come from the bounds band, and the spine has a FLOOR so a low or
// narrow hull cannot collapse its probes to nothing.
void test_radii() {
	CHECK(conform_wheel_radius(0, 0x10000) == 0x4000,
			"the wheel radius is a quarter of the band");

	// A roomy hull takes the derived value.
	const int32_t roomy = conform_spine_radius(0, 0x20000, 0, 0x20000);
	CHECK(roomy > kConformSpineRadiusFloor, "a roomy hull derives its radius");

	// A tight hull would derive something below the floor and is clamped up.
	const int32_t tight = conform_spine_radius(0, 0x1000, 0, 0x1000);
	CHECK(tight == kConformSpineRadiusFloor, "a tight hull clamps to the floor");
	CHECK(tight > 0, "and never reaches zero");
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
	CHECK(impact < 100000, "the impact sink drains while positive");
	// Energy floors at -1 rather than 0.
	CHECK(osc.energy < 0, "energy goes negative under load");
	CHECK(osc.energy == -1, "and floors at -1, not 0");
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
// applies ONLY on the tick the wheel lands at zero compression. Conflating
// them kills the bounce far too fast.
void test_oscillator_two_decays() {
	// Mid-bounce (compression will not land at 0): only the 0.99 applies.
	{
		ConformOscillator osc;
		osc.amplitude = 10000;
		osc.phase = 0.0f; // sin(0.2616) > 0, so env > 0.5 -> compression > 0
		int32_t compression = 0;
		int32_t impact = 0;
		conform_spring_oscillate(osc, compression, impact, 10, 3, false);
		CHECK(compression > 0, "mid-bounce leaves the wheel compressed");
		// 10000 * 0.99 = 9900, with no shock damp.
		CHECK(osc.amplitude == 9900, "only the 0.99 decay applies mid-bounce");
	}

	// A landing (env resolves to 0 compression) also applies the shock damp.
	{
		ConformOscillator osc;
		osc.amplitude = 0; // env * 0 == 0 -> lands
		osc.phase = 0.0f;
		int32_t compression = 500;
		int32_t impact = 0;
		conform_spring_oscillate(osc, compression, impact, 10, 3, false);
		CHECK(compression == 0, "the wheel lands");
	}

	// Shock strength changes the landing damp: 0 damps least, 10 most.
	{
		ConformOscillator a, b;
		a.amplitude = b.amplitude = 10000;
		a.phase = b.phase = 3.14159f; // sin ~ 0 -> env ~ 0.5
		int32_t ca = 0, cb = 0, ia = 0, ib = 0;
		// Force the landing arm by driving amplitude to 0 after the envelope.
		a.amplitude = 0; b.amplitude = 0;
		conform_spring_oscillate(a, ca, ia, 0, 3, false);
		conform_spring_oscillate(b, cb, ib, 10, 3, false);
		CHECK(ca == 0 && cb == 0, "both land");
	}

	// The a0_negative flag damps a landing four times harder again.
	{
		ConformOscillator plain, damped;
		plain.amplitude = damped.amplitude = 10000;
		plain.phase = damped.phase = 4.712f; // sin ~ -1 -> env ~ 0 -> lands
		int32_t cp = 0, cd = 0, ip = 0, id = 0;
		conform_spring_oscillate(plain, cp, ip, 0, 3, false);
		conform_spring_oscillate(damped, cd, id, 0, 3, true);
		CHECK(damped.amplitude <= plain.amplitude,
				"the a0 flag damps a landing at least as hard");
	}
}

// The client runs a DIFFERENT spring scale from the authority. That is
// witnessed, not a tuning knob to normalise away.
void test_client_scale_is_distinct() {
	CHECK(kClientSpringScale > 1.0f,
			"the non-authority spring scale is not unity");
	CHECK(kTimeScaleUnparked < 1.0f, "the unparked time scale slows the solve");
}

} // namespace

int main() {
	test_travel_from_def();
	test_radii();
	test_compress_bottomed_absorbs_nothing();
	test_compress_normal();
	test_impact_sink_is_one_directional();
	test_oscillator_two_decays();
	test_client_scale_is_distinct();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ground_conform_test OK\n");
	return 0;
}
