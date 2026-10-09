// The parent-local FK a skin poses by (runtime/anim/skeletal_clips.h pose_globals): each bone's local
// (its pose rotation as rows, its pose origin) on its parent's global, a root its own; the collapsed
// bone's rows zeroed with its origin kept, standing alone while its children chain on it [orig: special
// row @0x4b1290]; the count clamped to the pose and the parents. And a point or a direction carried by a
// transform (rest_transform_point / rest_transform_direction).
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "common/test_expect.h"

namespace {

namespace anim = opennova::anim;
using Rest = anim::SkeletalClips::RestTransform;

bool near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

bool same_rest(const Rest &a, const Rest &b) {
	for (int i = 0; i < 9; ++i)
		if (a.rows[i] != b.rows[i]) return false;
	return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z;
}

anim::Quat about_z(double angle) {
	return anim::Quat{static_cast<float>(std::cos(angle * 0.5)), 0.0f, 0.0f, static_cast<float>(std::sin(angle * 0.5))};
}

Rest local_of(const anim::PoseBone &bone) {
	Rest local;
	anim::quat_to_mat3_rows(bone.rotation, local.rows);
	local.origin = bone.origin;
	return local;
}

int test_chain() {
	// Bone 0 a root turned a quarter about +Z at (1, 0, 0); bone 1 its child one unit along x; bone 2 a
	// child of 1 turned again; bone 3 a root by -1.
	const std::vector<anim::PoseBone> pose = {
		{about_z(1.5707963267948966), {1.0f, 0.0f, 0.0f}},
		{anim::Quat{}, {1.0f, 0.0f, 0.0f}},
		{about_z(1.5707963267948966), {0.0f, 2.0f, 0.0f}},
		{anim::Quat{}, {5.0f, 5.0f, 5.0f}},
	};
	const std::vector<int> parents = {-1, 0, 1, -1};
	std::vector<Rest> globals;
	anim::pose_globals(pose, parents, pose.size(), -1, globals);
	TEST_EXPECT(globals.size() == 4);
	TEST_EXPECT(same_rest(globals[0], local_of(pose[0])));
	TEST_EXPECT(same_rest(globals[1], anim::rest_mul(globals[0], local_of(pose[1]))));
	TEST_EXPECT(same_rest(globals[2], anim::rest_mul(globals[1], local_of(pose[2]))));
	TEST_EXPECT(same_rest(globals[3], local_of(pose[3])));
	// The child one unit along the root's turned x: (1, 1, 0).
	TEST_EXPECT(near(globals[1].origin.x, 1.0f) && near(globals[1].origin.y, 1.0f) && near(globals[1].origin.z, 0.0f));
	// A count past the pose (or the parents) stops at the shorter.
	anim::pose_globals(pose, parents, 99, -1, globals);
	TEST_EXPECT(globals.size() == 4);
	anim::pose_globals(pose, {-1, 0}, 4, -1, globals);
	TEST_EXPECT(globals.size() == 2);
	// A prefix is the same FK over its bones.
	std::vector<Rest> prefix;
	anim::pose_globals(pose, parents, 2, -1, prefix);
	anim::pose_globals(pose, parents, pose.size(), -1, globals);
	TEST_EXPECT(prefix.size() == 2 && same_rest(prefix[1], globals[1]));
	std::printf("chain: four bones, parents first, a root, a clamped count, a prefix\n");
	return 0;
}

int test_collapse() {
	const std::vector<anim::PoseBone> pose = {
		{about_z(0.5), {0.0f, 0.0f, 1.0f}},
		{about_z(0.25), {0.0f, 3.0f, 0.0f}},
		{about_z(0.75), {2.0f, 0.0f, 0.0f}},
	};
	const std::vector<int> parents = {-1, 0, 1};
	std::vector<Rest> globals;
	anim::pose_globals(pose, parents, pose.size(), 1, globals);
	// The collapsed bone: rows zeroed, its own origin kept, not on its parent.
	for (int i = 0; i < 9; ++i) TEST_EXPECT(globals[1].rows[i] == 0.0f);
	TEST_EXPECT(globals[1].origin.x == 0.0f && globals[1].origin.y == 3.0f && globals[1].origin.z == 0.0f);
	// Its child still chains on it: the zero rows take the child's offset away, leaving the collapsed origin.
	TEST_EXPECT(same_rest(globals[2], anim::rest_mul(globals[1], local_of(pose[2]))));
	TEST_EXPECT(globals[2].origin.x == 0.0f && globals[2].origin.y == 3.0f && globals[2].origin.z == 0.0f);
	// A collapse past the count, or none, changes nothing.
	std::vector<Rest> plain, past;
	anim::pose_globals(pose, parents, pose.size(), -1, plain);
	anim::pose_globals(pose, parents, pose.size(), anim::kCollapsedRightHandBone, past);
	for (size_t i = 0; i < plain.size(); ++i) TEST_EXPECT(same_rest(plain[i], past[i]));
	std::printf("collapse: the bone's rows zeroed, its origin kept, its child chained on it\n");
	return 0;
}

int test_carry() {
	Rest turn;
	anim::quat_to_mat3_rows(about_z(1.5707963267948966), turn.rows);
	turn.origin = {10.0f, 0.0f, 0.0f};
	const anim::Vec3 point = anim::rest_transform_point(turn, {1.0f, 0.0f, 2.0f});
	TEST_EXPECT(near(point.x, 10.0f) && near(point.y, 1.0f) && near(point.z, 2.0f));
	const anim::Vec3 direction = anim::rest_transform_direction(turn, {1.0f, 0.0f, 2.0f});
	TEST_EXPECT(near(direction.x, 0.0f) && near(direction.y, 1.0f) && near(direction.z, 2.0f));
	// A point carried is the composed transform's origin, as rest_mul composes it.
	Rest at;
	at.origin = {1.0f, 0.0f, 2.0f};
	const Rest composed = anim::rest_mul(turn, at);
	TEST_EXPECT(composed.origin.x == point.x && composed.origin.y == point.y && composed.origin.z == point.z);
	std::printf("carry: a point turned and moved, a direction turned\n");
	return 0;
}

} // namespace

int main() {
	if (test_chain() != 0) return 1;
	if (test_collapse() != 0) return 1;
	if (test_carry() != 0) return 1;
	return 0;
}
