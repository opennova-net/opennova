// The suspension's x87 products (D-NET-515, the rest physics): retail loads each integer
// through `fild` and multiplies by a float constant at the FPU's length before the truncating
// _ftol2_sse, so an integer past 2^24 keeps its low bits and a product just under a whole
// number stays under it. Oracles are the cited arithmetic (the 53-bit double, Q3-11; the same
// words at 64 bits, both clear of a one-word edge).
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - dword_815184 = ftol(fild((100 -
//  spring_comp) * 0xFFFF) * flt_7C56A8 (0.01f)) @0x47C51F..0x47C544; the landing impulse
//  `fild; fmul flt_7C3B94` (0.5f) @0x47EA86..0x47EA90 and `fild; fmul flt_7C6F18` (1.25f)
//  @0x47EA9F..0x47EAA7; the settle term @0x47EB12..0x47EB1C; the flip bound
//  ftol(fild(flip) * 0.01f * 65536.0f) @0x47D727..0x47D745]

#include <runtime/world/ground_conform.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_suspension.h>

#include <cstdint>
#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

void test_the_locked_share() {
	// 6553500 * 0.0099999997764825821 = 65534.9985: 65534 (the float product rounds to 65535).
	CHECK(conform_travel_locked(0) == 65534, "spring_comp 0 locks 65534");
	CHECK(conform_travel_from_def(0) == 1, "and leaves one unit of travel");
	// 5242800 * 0.01f = 52427.9988: 52427 (the float product rounds to 52428).
	CHECK(conform_travel_locked(20) == 52427, "spring_comp 20 locks 52427");
	CHECK(conform_travel_from_def(20) == 13108, "and leaves 13108");
	// 4587450 * 0.01f = 45874.4990: 45874 either way.
	CHECK(conform_travel_locked(30) == 45874, "spring_comp 30 locks 45874");
	CHECK(conform_travel_locked(100) == 0, "spring_comp 100 locks nothing");
}

void test_half_energy() {
	// The settle term's ceiling, spring 6 and the 4095 step: 2 * 6 * 4095^2 = 201228300, past
	// 2^24; its exact half is 100614150 (a float cast first rounds it to 201228304).
	CHECK(conform_half_energy(2 * 6 * 4095 * 4095) == 100614150, "the exact half past 2^24");
	CHECK(conform_half_energy(16777217) == 8388608, "2^24 + 1 halves to 8388608");
	CHECK(conform_half_energy(7) == 3, "a small odd word truncates");
	CHECK(conform_half_energy(-7) == -3, "toward zero, as _ftol2_sse");
}

// The crash tests' flip bound: ftol(fild(flip) * 0.01f * 65536.0f) [orig: @0x47D727..0x47D745].
void test_the_flip_bound() {
	VehicleTraits t;
	t.flip = 50; // 50 * 0.01f * 65536 = 32767.9993 (the float product rounds to 32768)
	CHECK(vehicle_flip_threshold_q16(t) == 32767, "flip 50 bounds at 32767");
	t.flip = 45; // 29491.1993
	CHECK(vehicle_flip_threshold_q16(t) == 29491, "flip 45 bounds at 29491");
	t.flip = 100; // 65535.9985
	CHECK(vehicle_flip_threshold_q16(t) == 65535, "flip 100 bounds at 65535");
}

} // namespace

int main() {
	test_the_locked_share();
	test_half_energy();
	test_the_flip_bound();
	if (failures == 0) std::printf("suspension_precision: all passed\n");
	return failures == 0 ? 0 : 1;
}
