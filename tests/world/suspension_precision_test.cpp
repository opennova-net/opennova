// The suspension's x87 products (D-NET-515, the rest physics): retail loads each integer
// through `fild` and multiplies by a float constant at the FPU's length before the truncating
// _ftol2_sse. Our simulation keeps the 53-bit double (Q3-11); retail's control word is 24-bit
// on JO after the device, 64-bit on JO:CA after its boot effect and 53-bit on a dedicated host,
// and the 24-bit word differs from the 53 / 64-bit one at a one-word edge (spring_comp 0 / 20,
// flip 50 / 100) or where 24 bits cannot hold a product's integer part (the half of 201228300).
// So every pin here sits clear of those: each value is the same at 24, 53 and 64 bits. The
// 53-bit words at the edges are recorded in the D-NET-515 entry, not pinned.
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
	// 4587450 * 0.01f = 45874.4990 (45874.5 at 24 bits): 45874 at every length.
	CHECK(conform_travel_locked(30) == 45874, "spring_comp 30 locks 45874");
	CHECK(conform_travel_from_def(30) == 19661, "and leaves 19661");
	// 5898150 * 0.01f = 58981.4987: 58981 at every length.
	CHECK(conform_travel_locked(10) == 58981, "spring_comp 10 locks 58981");
	CHECK(conform_travel_from_def(10) == 6554, "and leaves 6554");
	CHECK(conform_travel_locked(100) == 0, "spring_comp 100 locks nothing");
	CHECK(conform_travel_from_def(100) == 65535, "and leaves full travel");
}

void test_half_energy() {
	// 2^24 + 1 through `fild` and * 0.5f is 8388608.5: 8388608 at every length (the 24-bit
	// round goes to even, the 53 / 64-bit truncation drops the half).
	CHECK(conform_half_energy(16777217) == 8388608, "2^24 + 1 halves to 8388608");
	CHECK(conform_half_energy(-16777217) == -8388608, "and -(2^24 + 1) to -8388608");
	CHECK(conform_half_energy(7) == 3, "a small odd word truncates");
	CHECK(conform_half_energy(-7) == -3, "toward zero, as _ftol2_sse");
}

// The crash tests' flip bound: ftol(fild(flip) * 0.01f * 65536.0f) [orig: @0x47D727..0x47D745].
void test_the_flip_bound() {
	VehicleTraits t;
	t.flip = 45; // 29491.1993: 29491 at every length
	CHECK(vehicle_flip_threshold_q16(t) == 29491, "flip 45 bounds at 29491");
	t.flip = 70; // 45875.1990: 45875 at every length
	CHECK(vehicle_flip_threshold_q16(t) == 45875, "flip 70 bounds at 45875");
	t.flip = 10; // 6553.5999: 6553 at every length
	CHECK(vehicle_flip_threshold_q16(t) == 6553, "flip 10 bounds at 6553");
}

} // namespace

int main() {
	test_the_locked_share();
	test_half_energy();
	test_the_flip_bound();
	if (failures == 0) std::printf("suspension_precision: all passed\n");
	return failures == 0 ? 0 : 1;
}
