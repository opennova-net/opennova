// The views the editor's effect schedule tests a NOVISNOUPDATE emitter against (editor/preview/effect_schedule):
// the orbit camera's own planes, and the world pass's (renderer::kScenePassNearZ, renderer::scene_far_plane of the
// fog distance) a mission's view takes, so an emitter past the fog stands outside it as the game's does, whatever
// the orbit camera's far plane.

#include <cstdio>

#include <editor/preview/effect_schedule.h>
#include <runtime/renderer/render_order.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// Whether a sphere at `at` of `radius` stands inside every plane (particle::emitter_bounds_visible's test).
bool inside(const particle::ParticleViewFrustum &frustum, const PreviewVec3 &at, float radius) {
	for (const auto &plane : frustum.planes)
		if (plane[0] * at.x + plane[1] * at.y + plane[2] * at.z + plane[3] < -radius) return false;
	return true;
}

// A point `depth` ahead of the camera's eye, on its axis.
PreviewVec3 ahead(const OrbitCamera &camera, float depth) {
	PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	const PreviewVec3 eye = camera.eye();
	return PreviewVec3{ eye.x - back.x * depth, eye.y - back.y * depth, eye.z - back.z * depth };
}

int test_world_pass_planes() {
	OrbitCamera camera;
	camera.distance = 20.0f;
	// A fog 325 m deep (CP10's): the far plane at floor(325) + 1, not the orbit camera's 500.
	const float fog = 325.0f;
	const particle::ParticleViewFrustum world = scene_pass_view_frustum(camera, 1280, 720, fog);
	TEST_EXPECT(world.valid);
	const float far_z = renderer::scene_far_plane(fog);
	TEST_EXPECT(inside(world, ahead(camera, far_z - 5.0f), 1.0f));
	TEST_EXPECT(!inside(world, ahead(camera, far_z + 5.0f), 1.0f));
	// Its radius reaches back across the far plane.
	TEST_EXPECT(inside(world, ahead(camera, far_z + 0.5f), 1.0f));
	// The orbit camera's own view keeps the emitter past the fog inside, and one past 500 m outside.
	const particle::ParticleViewFrustum orbit = preview_view_frustum(camera, 1280, 720);
	TEST_EXPECT(inside(orbit, ahead(camera, far_z + 5.0f), 1.0f));
	TEST_EXPECT(!inside(orbit, ahead(camera, camera.far_plane + 5.0f), 1.0f));
	// A fog deeper than 500 m: the world pass sees past the orbit camera's far plane.
	const particle::ParticleViewFrustum deep = scene_pass_view_frustum(camera, 1280, 720, 1000.0f);
	TEST_EXPECT(inside(deep, ahead(camera, 700.0f), 1.0f));
	// The near plane the world pass's: a point 0.1 ahead with no radius is behind it.
	TEST_EXPECT(!inside(world, ahead(camera, renderer::kScenePassNearZ * 0.5f), 0.0f));
	TEST_EXPECT(inside(world, ahead(camera, renderer::kScenePassNearZ * 2.0f), 0.0f));
	// No fog distance known, or no device size: no planes (every emitter advances).
	TEST_EXPECT(!scene_pass_view_frustum(camera, 1280, 720, 0.0f).valid);
	TEST_EXPECT(!scene_pass_view_frustum(camera, 0, 720, fog).valid);
	return 0;
}

} // namespace

int main() {
	const int failed = test_world_pass_planes();
	if (failed == 0) std::printf("effect schedule: the world pass's view, its far plane the fog's\n");
	return failed == 0 ? 0 : 1;
}
