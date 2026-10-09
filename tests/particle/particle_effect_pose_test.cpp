// The spawn poses every effect producer hands the scene (runtime/particle/effect_scene.h):
// forward_pose (the forward normalized, the +Y up hint turned to +X for a vertical forward, a zero
// forward keeping the identity basis), descriptor_pose (a zero orientation aims at +Y [orig:
// CEffectWorld_SpawnEmitterAtPosition @ 0x5F6E52..0x5F6E5C]) and compose_pose (a local pose under
// its owner's).
#include <runtime/particle/effect_scene.h>

#include <cmath>
#include <cstdio>

#include "common/test_expect.h"

namespace {

namespace particle = opennova::particle;

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= 0.00001f;
}

bool near_vec(const particle::Vec3 &actual, float x, float y, float z) {
	return near(actual.x, x) && near(actual.y, y) && near(actual.z, z);
}

bool same_vec(const particle::Vec3 &a, const particle::Vec3 &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_pose(const particle::EffectPose &a, const particle::EffectPose &b) {
	return same_vec(a.position, b.position) && same_vec(a.right, b.right) && same_vec(a.up, b.up) &&
	       same_vec(a.forward, b.forward);
}

int test_forward_pose() {
	// Along +X: right = +Y x forward = -Z, up = forward x right = +Y, at the point.
	const particle::EffectPose pose = particle::forward_pose({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
	TEST_EXPECT(near_vec(pose.position, 0.0f, 1.0f, 0.0f));
	TEST_EXPECT(near_vec(pose.forward, 1.0f, 0.0f, 0.0f) && near_vec(pose.right, 0.0f, 0.0f, -1.0f) &&
	            near_vec(pose.up, 0.0f, 1.0f, 0.0f));
	// A forward of any length is normalized; the basis stays orthonormal.
	const particle::EffectPose tilted = particle::forward_pose({}, {0.0f, -3.0f, 4.0f});
	TEST_EXPECT(near_vec(tilted.forward, 0.0f, -0.6f, 0.8f));
	const auto dot = [](const particle::Vec3 &a, const particle::Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
	TEST_EXPECT(near(dot(tilted.right, tilted.forward), 0.0f) && near(dot(tilted.up, tilted.forward), 0.0f) &&
	            near(dot(tilted.right, tilted.up), 0.0f) && near(dot(tilted.right, tilted.right), 1.0f) &&
	            near(dot(tilted.up, tilted.up), 1.0f));
	// A vertical forward (within 0.999 of +-Y) takes the +X hint: right = +X x +Y = +Z, up = +Y x +Z = +X.
	const particle::EffectPose up = particle::forward_pose({}, {0.0f, 2.0f, 0.0f});
	TEST_EXPECT(near_vec(up.forward, 0.0f, 1.0f, 0.0f) && near_vec(up.right, 0.0f, 0.0f, 1.0f) &&
	            near_vec(up.up, 1.0f, 0.0f, 0.0f));
	const particle::EffectPose down = particle::forward_pose({}, {0.0f, -1.0f, 0.0f});
	TEST_EXPECT(near_vec(down.right, 0.0f, 0.0f, -1.0f) && near_vec(down.up, 1.0f, 0.0f, 0.0f));
	// A zero forward (squared length at most 1e-6) keeps the identity basis at the point.
	const particle::EffectPose none = particle::forward_pose({1.0f, 2.0f, 3.0f}, {0.0005f, 0.0f, 0.0f});
	TEST_EXPECT(near_vec(none.position, 1.0f, 2.0f, 3.0f) && near_vec(none.right, 1.0f, 0.0f, 0.0f) &&
	            near_vec(none.up, 0.0f, 1.0f, 0.0f) && near_vec(none.forward, 0.0f, 0.0f, 1.0f));
	std::printf("forward_pose: +X, a tilt, the vertical hint, a zero forward\n");
	return 0;
}

int test_descriptor_pose() {
	// No orientation aims at +Y, the frame an effect played alone emits around.
	const particle::EffectPose none = particle::descriptor_pose({0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f});
	TEST_EXPECT(near_vec(none.position, 0.0f, 0.5f, 0.0f) && near_vec(none.forward, 0.0f, 1.0f, 0.0f));
	TEST_EXPECT(same_pose(none, particle::forward_pose({0.0f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f})));
	// Any other orientation is forward_pose's.
	const particle::Vec3 aimed{0.0f, -0.6f, 0.8f};
	TEST_EXPECT(same_pose(particle::descriptor_pose({}, aimed), particle::forward_pose({}, aimed)));
	std::printf("descriptor_pose: +Y for none, forward_pose for an orientation\n");
	return 0;
}

int test_compose_pose() {
	// An owner turned a quarter about +Y (its forward +X) at (10, 0, 0): a local point one unit ahead
	// lands at (11, 0, 0), and the local basis turns with it.
	const particle::EffectPose owner = particle::forward_pose({10.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
	particle::EffectPose local;
	local.position = {0.0f, 0.0f, 1.0f};
	const particle::EffectPose placed = particle::compose_pose(owner, local);
	TEST_EXPECT(near_vec(placed.position, 11.0f, 0.0f, 0.0f));
	TEST_EXPECT(near_vec(placed.forward, 1.0f, 0.0f, 0.0f) && near_vec(placed.right, 0.0f, 0.0f, -1.0f) &&
	            near_vec(placed.up, 0.0f, 1.0f, 0.0f));
	// Under the identity owner a pose is itself.
	const particle::EffectPose aimed = particle::forward_pose({1.0f, 2.0f, 3.0f}, {0.0f, -3.0f, 4.0f});
	TEST_EXPECT(same_pose(particle::compose_pose(particle::EffectPose(), aimed), aimed));
	std::printf("compose_pose: the owner's axes over the local pose, then its position\n");
	return 0;
}

} // namespace

int main() {
	if (test_forward_pose() != 0) return 1;
	if (test_descriptor_pose() != 0) return 1;
	if (test_compose_pose() != 0) return 1;
	return 0;
}
