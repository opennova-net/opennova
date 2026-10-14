// The mission view's pick math (editor/preview/preview_pick, ADR 0046 S14): the slab test of a segment against a box
// (met, missed, past the limit, along an axis outside the slab), the segment against a triangle (either face, its
// parameter, outside its edges, along its plane), and the nearest face of a shape before a best parameter.

#include <cmath>
#include <cstdio>

#include <editor/preview/preview_pick.h>

#include "common/test_expect.h"

using namespace opennova::editor;

namespace {

bool near(float a, float b) {
	return std::fabs(a - b) < 1e-5f;
}

int test_box() {
	const PreviewVec3 low{ -1.0f, -1.0f, -1.0f }, high{ 1.0f, 1.0f, 1.0f };
	TEST_EXPECT(segment_meets_box(low, high, { -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, 1.0f));
	TEST_EXPECT(!segment_meets_box(low, high, { -5.0f, 2.0f, 0.0f }, { 5.0f, 2.0f, 0.0f }, 1.0f));
	// The box begins at t = 0.4: a limit short of it misses.
	TEST_EXPECT(!segment_meets_box(low, high, { -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, 0.3f));
	TEST_EXPECT(segment_meets_box(low, high, { -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, 0.45f));
	// Along an axis: the start's place on it decides.
	TEST_EXPECT(segment_meets_box(low, high, { 0.0f, 0.0f, -5.0f }, { 0.0f, 0.0f, 5.0f }, 1.0f));
	TEST_EXPECT(!segment_meets_box(low, high, { 3.0f, 0.0f, -5.0f }, { 3.0f, 0.0f, 5.0f }, 1.0f));
	return 0;
}

int test_triangle() {
	const PreviewVec3 v0{ 0.0f, 0.0f, 0.0f }, v1{ 1.0f, 0.0f, 0.0f }, v2{ 0.0f, 1.0f, 0.0f };
	float at = -1.0f;
	TEST_EXPECT(segment_meets_triangle({ 0.25f, 0.25f, 1.0f }, { 0.25f, 0.25f, -1.0f }, v0, v1, v2, at) && near(at, 0.5f));
	// Either face.
	TEST_EXPECT(segment_meets_triangle({ 0.25f, 0.25f, -1.0f }, { 0.25f, 0.25f, 3.0f }, v0, v1, v2, at) && near(at, 0.25f));
	// Outside its edges, short of it, along its plane.
	TEST_EXPECT(!segment_meets_triangle({ 0.8f, 0.8f, 1.0f }, { 0.8f, 0.8f, -1.0f }, v0, v1, v2, at));
	TEST_EXPECT(!segment_meets_triangle({ 0.25f, 0.25f, 3.0f }, { 0.25f, 0.25f, 1.0f }, v0, v1, v2, at));
	TEST_EXPECT(!segment_meets_triangle({ -1.0f, 0.25f, 0.0f }, { 2.0f, 0.25f, 0.0f }, v0, v1, v2, at));
	return 0;
}

int test_nearest_face() {
	// Two faces across the x axis, at x = 1 and x = -1; the segment from x = -5 to 5 meets the far one (x = 1, t = 0.6)
	// and the near one (x = -1, t = 0.4) first.
	const PreviewVec3 faces[] = {
		{ 1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, -1.0f }, { 1.0f, 0.0f, 1.0f },
		{ -1.0f, -1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f }, { -1.0f, 0.0f, 1.0f },
	};
	const PreviewVec3 low{ -1.0f, -1.0f, -1.0f }, high{ 1.0f, 1.0f, 1.0f };
	float best = 1.0f;
	TEST_EXPECT(segment_nearest_face(faces, 6, low, high, { -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, best) && near(best, 0.4f));
	// Something nearer already (the ground at t = 0.3): the shape's faces leave it.
	best = 0.3f;
	TEST_EXPECT(!segment_nearest_face(faces, 6, low, high, { -5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, best) && near(best, 0.3f));
	// A segment missing the box tests no face.
	best = 1.0f;
	TEST_EXPECT(!segment_nearest_face(faces, 6, low, high, { -5.0f, 4.0f, 0.0f }, { 5.0f, 4.0f, 0.0f }, best) && best == 1.0f);
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_box();
	failed += test_triangle();
	failed += test_nearest_face();
	if (failed == 0) std::printf("preview pick: the box, the triangle, the nearest face\n");
	return failed == 0 ? 0 : 1;
}
