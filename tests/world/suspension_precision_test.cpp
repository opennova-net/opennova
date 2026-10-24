// The suspension's x87 products (D-NET-515, the rest physics): retail loads each integer
// through `fild` and multiplies by a float constant at the FPU's length before the truncating
// _ftol2_sse. Our simulation keeps the 53-bit double (Q3-11); retail's control word is 24-bit
// on JO after the device, 64-bit on JO:CA after its boot effect and 53-bit on a dedicated host,
// and the 24-bit word differs from the 53 / 64-bit one at a one-word edge (spring_comp 0 / 20,
// flip 50 / 100) or where 24 bits cannot hold a product's integer part (the half of 201228300).
// So every pin here sits clear of those: each value is the same at 24, 53 and 64 bits. The
// 53-bit words at the edges are recorded in the D-NET-515 entry, not pinned. The rest fit's
// geometry is pinned the same way: the corner quad's separately rounded row products and the
// forward sum's twice-normalized edge.
// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - dword_815184 = ftol(fild((100 -
//  spring_comp) * 0xFFFF) * flt_7C56A8 (0.01f)) @0x47C51F..0x47C544; the landing impulse
//  `fild; fmul flt_7C3B94` (0.5f) @0x47EA86..0x47EA90 and `fild; fmul flt_7C6F18` (1.25f)
//  @0x47EA9F..0x47EAA7; the settle term @0x47EB12..0x47EB1C; the flip bound
//  ftol(fild(flip) * 0.01f * 65536.0f) @0x47D727..0x47D745]

#include <runtime/world/ground_conform.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
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

// The fit's corner quad: corner k is the origin plus the forward row (the Q22 row >> 6) times
// +/- half the length and the side row times +/- half the width, each product rounded on its own
// (integer words, the same at every length; the halves ftol(span * +/-0.5f) are exact).
// Corner 0's x: forward 3632377 >> 6 = 56755, side -2097130 >> 6 = -32768; the halves 163840 and
// 98304 (196609 * 0.5 = 98304.5 truncated); (163840 * 56755 + 0x8000) >> 16 = 141888 and
// (98304 * -32768 + 0x8000) >> 16 = -49152; so 6553600 + 141888 - 49152 = 6646336. The Q22 matrix
// applied to the corner point, ours before, gave 6646338.
// [orig: Entity_ComputeBoundingQuad @0x45B6E0, the non-square arm @0x45B8E4..0x45BE9F]
void test_the_bounding_quad() {
	CollisionMatrix m;
	const int32_t words[12] = { 3632377, -2097130, 41, 0, 2097185, 3632311, -73373, 0, -41, 73400,
		4193665, 0 };
	for (int i = 0; i < 12; ++i) m.m[i] = words[i];
	int32_t quad[4][3];
	detail::vehicle_bounding_quad(m, 196609, 327680, 100 << 16, 200 << 16, 10 << 16, quad);
	const int32_t expected[4][3] = { { 6646336, 13274251, 657077 },
		{ 6744640, 13103989, 653639 }, { 6460865, 12940149, 653644 },
		{ 6362561, 13110411, 657082 } };
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i < 3; ++i)
			CHECK(quad[k][i] == expected[k][i], "the quad corner is the two rounded row products");
}

// The square quad (the parked aircraft's, the bike's): the lower pair along the axis at half the
// larger |width| / |length| (|-327683| over 262144: 163841), lifted along the euler matrix's
// row 2 by half |z_span| (|-131075|: 65537), each product rounded on its own (integer words,
// the same at every length; the halves are exact below 2^24). Row 0 >> 6 is (56755, 32768, -1),
// row 2 >> 6 is (0, -1147, 65526): along = (141888, 81921, -3) ((163841 * -1 + 0x8000) >> 16
// floors to -3), the lift (0, -1147, 65527). The aircraft's parked arm built a flat box quad
// through the Q22 matrix instead, its lower corners at +/- half each span with no lift.
// [orig: Entity_ComputeBoundingQuad @0x45B6E0, the square arm @0x45B6F5..0x45B8DC; the calls
//  @0x481653 (Entity_ProcessAircraftContactPhysics @0x47EF10) and @0x47AB1C / @0x47BE7A
//  (Entity_ProcessLightVehiclePhysics @0x479600)]
void test_the_square_quad() {
	CollisionMatrix m;
	const int32_t words[12] = { 3632377, -2097130, 41, 0, 2097185, 3632311, -73373, 0, -41, 73400,
		4193665, 0 };
	for (int i = 0; i < 12; ++i) m.m[i] = words[i];
	const int32_t axis[3] = { m.m[0] >> 6, m.m[4] >> 6, m.m[8] >> 6 };
	int32_t quad[4][3];
	detail::vehicle_square_quad(m, -131075, -327683, 262144, axis, 100 << 16, 200 << 16, 10 << 16,
			quad);
	const int32_t expected[4][3] = { { 6695488, 13189121, 655357 },
		{ 6411712, 13025279, 655363 }, { 6411712, 13024132, 720890 },
		{ 6695488, 13187974, 720884 } };
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i < 3; ++i)
			CHECK(quad[k][i] == expected[k][i], "the square quad corner");
}

// The plat fit's forward sum reads set 1's edge normalized twice beside set 2's once-normalized
// copy. A one-word change of an even sum never moves its quarter unless the sum's sign differs
// from the edge's, so the pin is a crossed quad whose edges c0 - c3 and c1 - c2 oppose: set 1's
// edge (55595, 21992, 26839) normalizes again to (55596, 21992, 26839), c1 - c2 to
// (-55597, -21991, -26838); the sum 2 * e3 + a + a2 = (-3, 2, 2) quarters to (0, 0, 0), where
// 2 * e3 + 2 * a = (-4, 2, 2) quartered to (-1, 0, 0) and normalized to (-65536, 0, 0). The side
// axis (17090, 63201, 2915) is untouched. Every word is the same at 24, 53 and 64 bits (exact
// rational x87 emulation of the normalize, the quarter and _ftol2_sse).
// [orig: Entity_ProcessWheeledVehicleSuspension @0x46B140 - the second normalize
//  @0x46BB96..0x46BBE6, the forward sum @0x46C29D..0x46C3C6, the quarter flt_7C333C @0x46C3C0]
void test_the_fit_reads_the_edge_normalized_twice() {
	const int32_t c[4][3] = { { 112911, 44666, 54510 }, { -130568, -206659, -48375 },
		{ -17654, -161995, 6132 }, { 0, 0, 0 } };
	detail::PlatFit fit;
	CollisionMatrix m;
	detail::plat_fit_corners(c, fit, &m);
	CHECK(m.m[0] == 0 && m.m[4] == 0 && m.m[8] == 0, "the forward axis quarters to zero");
	CHECK(m.m[1] == 17090 * 64 && m.m[5] == 63201 * 64 && m.m[9] == 2915 * 64,
			"the side axis is untouched");
}

// The normalize divides first: k = 65536 over the length once, then each component times k,
// truncated. An axis vector of length 131019 or -425 then reads 65535 / -65535 at 24, 53 and 64
// bits alike, where a multiply-first `v * 65536 / length` reads 65536 / -65536 at every length;
// a vector clear of the edge, (3000, 4000, 0), reads (39321, 52428, 0) either way, and a zero
// vector all 0.
// [orig: Vehicle_ComputeOrientationFrom4Wheels @0x459310 (`fdivr st, st(5)` @0x4593A6 over
//  flt_7C32BC); Entity_ComputeSuspensionOrientation @0x46C8E0 (`fdivrp st(3), st` @0x46C9C1);
//  Entity_ApplyBoneAttachmentTransform @0x45A750 (@0x45A8A8)]
void test_the_normalize_divides_first() {
	int32_t out[3];
	const int64_t axis[3] = { 131019, 0, 0 };
	detail::q16_normalize(axis, out);
	CHECK(out[0] == 65535 && out[1] == 0 && out[2] == 0, "131019 along x reads 65535");
	const int64_t short_axis[3] = { 0, -425, 0 };
	detail::q16_normalize(short_axis, out);
	CHECK(out[0] == 0 && out[1] == -65535 && out[2] == 0, "-425 along y reads -65535");
	const int64_t clear[3] = { 3000, 4000, 0 };
	detail::q16_normalize(clear, out);
	CHECK(out[0] == 39321 && out[1] == 52428 && out[2] == 0,
			"(3000, 4000, 0) reads (39321, 52428, 0)");
	const int64_t zero[3] = { 0, 0, 0 };
	detail::q16_normalize(zero, out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0, "a zero vector reads 0");
}

} // namespace

int main() {
	test_the_locked_share();
	test_half_energy();
	test_the_flip_bound();
	test_the_bounding_quad();
	test_the_square_quad();
	test_the_fit_reads_the_edge_normalized_twice();
	test_the_normalize_divides_first();
	if (failures == 0) std::printf("suspension_precision: all passed\n");
	return failures == 0 ? 0 : 1;
}
