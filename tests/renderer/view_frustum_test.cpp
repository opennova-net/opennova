#include <runtime/renderer/view_frustum.h>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using opennova::renderer::view_frustum;
using opennova::renderer::view_frustum_sphere_visible;
using opennova::renderer::ViewFrustum;

// An eye at (10, 0, 0) looking down -z (Godot's camera axes), 90 degrees
// across on both axes (tangent 1).
const float kEye[3] = { 10.0f, 0.0f, 0.0f };
const float kForward[3] = { 0.0f, 0.0f, -1.0f };
const float kRight[3] = { 1.0f, 0.0f, 0.0f };
const float kUp[3] = { 0.0f, 1.0f, 0.0f };

bool visible(const ViewFrustum &frustum, float x, float y, float z, float radius) {
	const float center[3] = { kEye[0] + x, kEye[1] + y, kEye[2] + z };
	return view_frustum_sphere_visible(frustum, center, radius);
}

void check_planes() {
	const ViewFrustum none = view_frustum(kEye, kForward, kRight, kUp, 0.0f, 1.0f, 0.0f, 0.0f);
	CHECK(none.count == 0);
	CHECK(visible(none, 0.0f, 0.0f, 1000.0f, 0.0f));
	const ViewFrustum open = view_frustum(kEye, kForward, kRight, kUp, 1.0f, 1.0f, 0.0f, 0.0f);
	CHECK(open.count == 5);
	const ViewFrustum closed = view_frustum(kEye, kForward, kRight, kUp, 1.0f, 0.5f, 0.5f, 100.0f);
	CHECK(closed.count == 6);
}

void check_sphere_rejects_below_minus_radius() {
	// Each half-plane rejects a sphere whose signed distance falls below
	// -radius [orig: Viewport_TransformAndClipPoint @ 0x4115e0].
	const ViewFrustum frustum = view_frustum(kEye, kForward, kRight, kUp, 1.0f, 1.0f, 0.0f, 0.0f);
	CHECK(visible(frustum, 0.0f, 0.0f, -10.0f, 0.0f));
	// Behind the eye: the forward plane through the eye.
	CHECK(!visible(frustum, 0.0f, 0.0f, 5.0f, 1.0f));
	CHECK(visible(frustum, 0.0f, 0.0f, 0.5f, 1.0f));
	// Past the right side at depth 10 (the edge at x = 10): the side plane's
	// distance is (x - 10) cos 45.
	CHECK(!visible(frustum, 12.0f, 0.0f, -10.0f, 1.0f));
	CHECK(visible(frustum, 11.0f, 0.0f, -10.0f, 1.0f));
	CHECK(!visible(frustum, -12.0f, 0.0f, -10.0f, 1.0f));
	CHECK(!visible(frustum, 0.0f, 12.0f, -10.0f, 1.0f));
	CHECK(!visible(frustum, 0.0f, -12.0f, -10.0f, 1.0f));
	// No far plane: any depth inside the sides passes.
	CHECK(visible(frustum, 0.0f, 0.0f, -1.0e6f, 0.0f));
}

void check_near_and_far() {
	const ViewFrustum frustum = view_frustum(kEye, kForward, kRight, kUp, 1.0f, 0.5f, 0.5f, 100.0f);
	CHECK(!visible(frustum, 0.0f, 0.0f, -0.25f, 0.1f));
	CHECK(visible(frustum, 0.0f, 0.0f, -0.25f, 0.3f));
	CHECK(!visible(frustum, 0.0f, 0.0f, -102.0f, 1.0f));
	CHECK(visible(frustum, 0.0f, 0.0f, -100.5f, 1.0f));
	// The vertical half-angle's tangent 0.5: the top edge at y = 5 at depth 10.
	CHECK(!visible(frustum, 0.0f, 7.0f, -10.0f, 1.0f));
	CHECK(visible(frustum, 0.0f, 5.5f, -10.0f, 1.0f));
}

} // namespace

int main() {
	check_planes();
	check_sphere_rejects_below_minus_radius();
	check_near_and_far();
	if (failures != 0) {
		std::printf("renderer_view_frustum: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_view_frustum ok\n");
	return 0;
}
