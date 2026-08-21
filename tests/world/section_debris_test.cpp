// Section-debris sampling: the 8.8 stride, the centroid order, the integer
// distance approximation and the foliage material gate.
// [orig: Entity_SpawnSectionDebris @0x43F580]

#include <world/section_debris.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The stride spreads the model's faces over ~150 samples, so cost is roughly
// flat regardless of model size — that is the whole point of sampling.
void test_stride_targets_a_fixed_budget() {
	// A big model strides several faces per sample.
	const int64_t big = debris_stride_q8(1500);
	CHECK(big == (1500LL << 8) / 150, "the stride is faces/150 in 8.8");
	CHECK((big >> 8) == 10, "1500 faces stride 10 faces per sample");

	// Walking a 1500-face section yields about the sample budget, not 1500.
	int n = 0;
	for (int64_t i = 0; (i >> 8) < 1500; i += big) ++n;
	CHECK(n >= 140 && n <= 160, "a big model still emits about 150 samples");

	// THE 8.8 FIXED MATTERS: a model with FEWER faces than the budget gets a
	// sub-one stride and still advances, rather than stalling on face 0.
	const int64_t small = debris_stride_q8(40);
	CHECK(small > 0, "a small model has a non-zero stride");
	CHECK((small >> 8) == 0, "which is LESS than one face per step");
	int m = 0;
	for (int64_t i = 0; (i >> 8) < 40; i += small) { ++m; if (m > 1000) break; }
	CHECK(m > 40 && m <= 1000, "and the walk terminates rather than stalling");

	// A degenerate section yields no stride at all.
	CHECK(debris_stride_q8(0) == 0, "no faces, no stride");
	CHECK(debris_stride_q8(-5) == 0, "a negative count is inert");
}

// THE DIVIDE COMES FIRST. Averaging in Q8 then shifting is not the same as
// shifting then averaging — the former truncates, and that truncation is what
// retail's output carries.
void test_centroid_divides_before_shifting() {
	// 1+1+1 = 3, /3 = 1, << 8 = 256.
	CHECK(debris_centroid_axis(1, 1, 1) == 256, "an exact average");

	// 1+1+2 = 4, /3 = 1 (truncated), << 8 = 256.
	// Shifting first would give (256+256+512)/3 = 341 — a different point.
	CHECK(debris_centroid_axis(1, 1, 2) == 256,
			"the divide truncates BEFORE the shift");
	CHECK(debris_centroid_axis(1, 1, 2) != (((1 << 8) + (1 << 8) + (2 << 8)) / 3),
			"shift-then-average would give a different centroid");

	CHECK(debris_centroid_axis(0, 0, 0) == 0, "a degenerate face centres at 0");
}

// The pitch uses an integer approximation, NOT a hypotenuse. Replacing it with
// a real distance changes the throw angle of every off-axis fragment.
void test_distance_approximation() {
	// On-axis: the approximation is exact.
	CHECK(debris_distance_approx(100, 0) == 100, "a pure-x delta is exact");
	CHECK(debris_distance_approx(0, 100) == 100, "and a pure-y delta");

	// Off-axis: max + (min >> 4) * 5. For (100,100): 100 + (6 * 5) = 130.
	CHECK(debris_distance_approx(100, 100) == 130, "the 45-degree case");
	// A true hypotenuse would be ~141 — the approximation is deliberately low.
	CHECK(debris_distance_approx(100, 100) < 141,
			"the approximation is NOT a hypotenuse");

	// Sign-independent.
	CHECK(debris_distance_approx(-100, 100) == debris_distance_approx(100, 100),
			"signs do not matter");
	CHECK(debris_distance_approx(100, -100) == debris_distance_approx(100, 100),
			"in either axis");

	// Symmetric in its arguments.
	CHECK(debris_distance_approx(300, 40) == debris_distance_approx(40, 300),
			"the larger always leads");
}

// Material 17 is foliage, and the difference is OWNERSHIP, not just texture:
// an owned effect dies with its entity, an unowned one outlives it.
void test_foliage_gate() {
	CHECK(debris_is_foliage(17), "material 17 is foliage");
	CHECK(!debris_is_foliage(16), "16 is not");
	CHECK(!debris_is_foliage(18), "nor 18 — it is one exact value");
	CHECK(!debris_is_foliage(0), "nor the default material");

	CHECK(debris_is_owned(17), "foliage debris is OWNED by the entity");
	CHECK(!debris_is_owned(3), "wood debris is not");
}

// The no-blast throw uses a FIXED rise rather than a computed one.
void test_default_pitch_is_constant() {
	CHECK(kDebrisDefaultPitchBam == 0x2CFFFFD3,
			"the witnessed constant pitch");
	CHECK(kDebrisDefaultPitchBam > 0, "which throws upward");
}

} // namespace

int main() {
	test_stride_targets_a_fixed_budget();
	test_centroid_divides_before_shifting();
	test_distance_approximation();
	test_foliage_gate();
	test_default_pitch_is_constant();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("section_debris_test OK\n");
	return 0;
}
