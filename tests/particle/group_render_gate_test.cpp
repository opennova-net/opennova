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

const int32_t OK_CLASS = kGroupChildClassId;

// THE TWO PASSES ARE COMPLEMENTARY — a child draws on exactly one of them, not
// both. If the comparison is reversed or the boundary shared, an effect
// doubles at some distances and vanishes at others.
void test_passes_are_complementary() {
	const float thr = 100.0f;
	for (float d : {0.0f, 50.0f, 99.9f, 100.0f, 100.1f, 500.0f}) {
		const bool near_pass =
				group_child_draws(OK_CLASS, kRenderFlagNear, d, thr, true);
		const bool far_pass =
				group_child_draws(OK_CLASS, kRenderFlagFar, d, thr, true);
		CHECK(near_pass != far_pass,
				"every distance belongs to exactly one of the two passes");
	}
}

// The boundary belongs to the NEAR pass: the far test is strictly greater.
void test_boundary_belongs_to_near() {
	const float thr = 100.0f;
	CHECK(group_child_draws(OK_CLASS, kRenderFlagNear, 100.0f, thr, true),
			"exactly on the threshold draws on the near pass");
	CHECK(!group_child_draws(OK_CLASS, kRenderFlagFar, 100.0f, thr, true),
			"and NOT on the far pass");
	CHECK(group_child_draws(OK_CLASS, kRenderFlagFar, 100.1f, thr, true),
			"just beyond it flips to far");
}

// A child with NO cached distance is treated as at zero — inside every near
// threshold, rather than being pushed out to the far pass.
void test_missing_distance_is_zero() {
	CHECK(group_child_draws(OK_CLASS, kRenderFlagNear, 9999.0f, 100.0f, false),
			"no cached distance reads as 0, so the near pass takes it");
	CHECK(!group_child_draws(OK_CLASS, kRenderFlagFar, 9999.0f, 100.0f, false),
			"and the far pass does not");
}

// The ALWAYS flag bypasses both the distance test AND the class check.
void test_always_flag() {
	CHECK(group_child_draws(OK_CLASS, kRenderFlagAlways, 9999.0f, 1.0f, true),
			"always draws regardless of distance");
	CHECK(group_child_draws(99, kRenderFlagAlways, 0.0f, 1.0f, true),
			"and regardless of the child class");
}

// A child of the wrong class is skipped unless ALWAYS is set.
void test_class_gate() {
	CHECK(!group_child_draws(99, kRenderFlagNear, 0.0f, 100.0f, true),
			"a foreign child class does not draw on the near pass");
	CHECK(!group_child_draws(99, kRenderFlagFar, 999.0f, 100.0f, true),
			"nor the far pass");
	CHECK(group_child_draws(OK_CLASS, kRenderFlagNear, 0.0f, 100.0f, true),
			"the right class does");
}

// A zero flag word means no distance test at all; a word with only unrelated
// bits set skips the child.
void test_flag_edge_cases() {
	CHECK(group_child_draws(OK_CLASS, 0u, 9999.0f, 1.0f, true),
			"no flags means no distance test");
	CHECK(!group_child_draws(OK_CLASS, 0x8u, 9999.0f, 1.0f, true),
			"an unrelated flag skips the child");
}

} // namespace

int main() {
	test_passes_are_complementary();
	test_boundary_belongs_to_near();
	test_missing_distance_is_zero();
	test_always_flag();
	test_class_gate();
	test_flag_edge_cases();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("group_render_gate_test OK\n");
	return 0;
}
