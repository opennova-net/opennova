// The particle group's child render gate: which children draw on the near
// pass, the far pass, and always.
// [orig: CParticleGroup_RenderChildren @0x5E5890]

#include <particle/group_render_gate.h>

#include <cstdio>
#include <initializer_list>

using namespace opennova::particle;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// A plain child: any def class other than the ALWAYS-only class 7.
const int32_t PLAIN_CLASS = 3;
const int32_t ALWAYS_CLASS = kGroupChildClassId;

// THE TWO PASSES ARE COMPLEMENTARY — a plain child draws on exactly one of
// them, not both. If the comparison is reversed or the boundary shared, an
// effect doubles at some distances and vanishes at others.
void test_passes_are_complementary() {
	const float thr = 100.0f;
	for (float d : {0.0f, 50.0f, 99.9f, 100.0f, 100.1f, 500.0f}) {
		const bool near_pass =
				group_child_draws(PLAIN_CLASS, kRenderFlagNear, d, thr, true);
		const bool far_pass =
				group_child_draws(PLAIN_CLASS, kRenderFlagFar, d, thr, true);
		CHECK(near_pass != far_pass,
				"every distance belongs to exactly one of the two passes");
	}
}

// The boundary belongs to the NEAR pass: the far test is strictly greater.
void test_boundary_belongs_to_near() {
	const float thr = 100.0f;
	CHECK(group_child_draws(PLAIN_CLASS, kRenderFlagNear, 100.0f, thr, true),
			"exactly on the threshold draws on the near pass");
	CHECK(!group_child_draws(PLAIN_CLASS, kRenderFlagFar, 100.0f, thr, true),
			"and NOT on the far pass");
	CHECK(group_child_draws(PLAIN_CLASS, kRenderFlagFar, 100.1f, thr, true),
			"just beyond it flips to far");
}

// A child with NO cached distance is treated as at zero — inside every near
// threshold, rather than being pushed out to the far pass.
void test_missing_distance_is_zero() {
	CHECK(group_child_draws(PLAIN_CLASS, kRenderFlagNear, 9999.0f, 100.0f, false),
			"no cached distance reads as 0, so the near pass takes it");
	CHECK(!group_child_draws(PLAIN_CLASS, kRenderFlagFar, 9999.0f, 100.0f, false),
			"and the far pass does not");
}

// CLASS 7 IS THE ALWAYS-ONLY CLASS [orig: @0x5E58D2..0x5E58EE]: it draws on
// a pass carrying flag 4 regardless of distance, and on no other pass — not
// even a flagless one. A plain child never sees flag 4: ALWAYS alone is a
// word with neither distance bit, which skips it.
void test_always_class() {
	CHECK(group_child_draws(ALWAYS_CLASS, kRenderFlagAlways, 9999.0f, 1.0f, true),
			"class 7 draws on the ALWAYS pass regardless of distance");
	CHECK(group_child_draws(ALWAYS_CLASS, kRenderFlagAlways | kRenderFlagFar, 0.0f, 1.0f, true),
			"and the distance bits beside it do not matter");
	CHECK(!group_child_draws(ALWAYS_CLASS, kRenderFlagNear, 0.0f, 100.0f, true),
			"class 7 does not draw on the near pass");
	CHECK(!group_child_draws(ALWAYS_CLASS, kRenderFlagFar, 999.0f, 100.0f, true),
			"nor on the far pass");
	CHECK(!group_child_draws(ALWAYS_CLASS, 0u, 0.0f, 100.0f, true),
			"nor on a flagless pass");
	CHECK(!group_child_draws(PLAIN_CLASS, kRenderFlagAlways, 0.0f, 100.0f, true),
			"a plain child is skipped by ALWAYS alone (neither distance bit)");
}

// A zero flag word means no distance test at all for a plain child; a word
// with only unrelated bits set skips it.
void test_flag_edge_cases() {
	CHECK(group_child_draws(PLAIN_CLASS, 0u, 9999.0f, 1.0f, true),
			"no flags means no distance test");
	CHECK(!group_child_draws(PLAIN_CLASS, 0x8u, 9999.0f, 1.0f, true),
			"an unrelated flag skips the child");
	CHECK(group_child_draws(PLAIN_CLASS, kRenderFlagFar | 0x8u, 500.0f, 1.0f, true),
			"an unrelated bit beside a distance bit changes nothing");
}

} // namespace

int main() {
	test_passes_are_complementary();
	test_boundary_belongs_to_near();
	test_missing_distance_is_zero();
	test_always_class();
	test_flag_edge_cases();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("group_render_gate_test OK\n");
	return 0;
}
