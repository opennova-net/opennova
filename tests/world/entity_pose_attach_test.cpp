/* engine/runtime/world/entity_pose attach frames — the bone frames and the rigid attachments a
   person carries, built from the engine's own bone matrices (the body's composed pose through the
   rig's FK), moved from the binding's Skeleton3D read-backs (ADR 0077 R2). Over an in-memory rig
   (no retail data), these pin what the GUT suite pinned on a hand-posed Skeleton3D:

     1. the frames: at rest the model->world matrix is the body's world, the joint the rest pivot;
        a posed bone turns both; the body's world places them; a missing bone answers false;
     2. the held gun [orig: matrix build @0x4b2180..0x4b22f8]: the entity frame sends the model's
        barrel (+Z) down the attach pitch and yaw whatever the hand's rotation; the position is the
        joint plus the nudge carried by the bone's model->world rotation (unrotated at rest); the
        hand frame ignores the attach angles and tracks the bone;
     3. the overlays: the goggles and the binoculars at their nudged joints, the canopy at bone 0's
        model origin with the heading turn alone, the carried object at the binocular point with
        the carrier's own orientation after the fixed render-Z turn;
     4. posed_part_to_world is the bone's model->world frame (an action point's part). */

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"

#include <formats/bad/bad.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/person_overlays.h>

using namespace opennova;
using anim::PoseBone;
using anim::Quat;
using anim::Vec3;
using world::PoseTransform;
using world::PosedBoneFrames;

namespace {

constexpr size_t kBones = 19;
constexpr double kPi = 3.14159265358979323846;

// A 19-bone chain (each bone under the one before), one clip key each, identity binds; every bone
// a tenth up its parent.
struct ChainClip {
	std::vector<bad::BadBone> bones;
	std::vector<bad::BadChannel> channels;
	std::vector<bad::BadQuaternion> rotations;
	std::vector<uint16_t> lengths;
	bad::BadFile file = {};

	ChainClip() : bones(kBones), channels(kBones), rotations(kBones), lengths(kBones, 1) {
		for (size_t i = 0; i < kBones; ++i) {
			bones[i].parent_index = i == 0 ? -1 : static_cast<int32_t>(i - 1);
			bones[i].rotation[0] = bones[i].rotation[4] = bones[i].rotation[8] = 1.0f;
			bones[i].position[1] = i == 0 ? 0.0f : 0.1f;
			rotations[i] = {0.0f, 0.0f, 0.0f, 1.0f};
			channels[i].frame_count = 1;
			channels[i].frame_lengths = &lengths[i];
			channels[i].rotations = &rotations[i];
		}
		file.fps = 10;
		file.frame_count = 1;
		file.bone_count = static_cast<uint32_t>(kBones);
		file.bones = bones.data();
		file.num_bones = kBones;
		file.channels = channels.data();
		file.num_channels = kBones;
	}
};

class MemoryFiles : public anim::RigFiles {
public:
	std::map<std::string, const bad::BadFile *> bads;
	std::shared_ptr<const adm::AdmFile> animation_map(const std::string &) const override { return nullptr; }
	std::shared_ptr<const bad::BadFile> bone_animation(const std::string &name) const override {
		const auto it = bads.find(name);
		return it == bads.end() ? nullptr : std::shared_ptr<const bad::BadFile>(it->second, [](const bad::BadFile *) {});
	}
};

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near(const Vec3 &a, const Vec3 &b, float eps = 1e-4f) {
	return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bool same_turn(const PoseTransform &a, const PoseTransform &b, float eps = 1e-4f) {
	for (int i = 0; i < 9; ++i)
		if (!near(a.rows[i], b.rows[i], eps)) return false;
	return true;
}
Vec3 add(const Vec3 &a, const Vec3 &b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }

// A turn of `deg` degrees about the render frame's Y (up).
PoseTransform yaw_turn(double deg) {
	const float c = static_cast<float>(std::cos(deg * kPi / 180.0));
	const float s = static_cast<float>(std::sin(deg * kPi / 180.0));
	PoseTransform t;
	t.rows[0] = c; t.rows[2] = s;
	t.rows[6] = -s; t.rows[8] = c;
	return t;
}

// The rig's rest pose as parent-local bones (bind rotation, bind origin), the pose a channel at
// rest would hand the device.
std::vector<PoseBone> rest_pose(const anim::SkeletalClips &rig) {
	std::vector<PoseBone> pose(rig.bone_count());
	for (size_t i = 0; i < pose.size(); ++i) {
		pose[i].rotation = anim::mat3_to_quat(rig.bind_local()[i].rows);
		pose[i].origin = rig.bind_local()[i].origin;
	}
	return pose;
}

} // namespace

int main() {
	ChainClip chain;
	MemoryFiles files;
	files.bads["rest.bad"] = &chain.file;
	anim::SkeletalClips rig;
	TEST_EXPECT(rig.load_from_files(&files, "rest.bad", {{"anim_idle", "rest.bad"}}));
	TEST_EXPECT(rig.bone_count() == kBones && rig.fk_valid());
	const int hand_bone = world::kHeldWeaponBoneIndex;
	const PoseTransform identity;
	const Vec3 gun_nudge{world::kHeldWeaponAttachNudgeX, world::kHeldWeaponAttachNudgeY,
			world::kHeldWeaponAttachNudgeZ};

	// ---- 1. the frames ----
	PosedBoneFrames rest;
	TEST_EXPECT(world::posed_bone_frames(rig, {}, identity, hand_bone, rest));
	TEST_EXPECT(same_turn(rest.model_to_world, identity) && near(rest.model_to_world.origin, Vec3{}));
	TEST_EXPECT(near(rest.joint_world.origin, rig.rest_global()[hand_bone].origin));
	PosedBoneFrames none;
	TEST_EXPECT(!world::posed_bone_frames(rig, {}, identity, static_cast<int>(kBones), none));
	TEST_EXPECT(!world::posed_bone_frames(rig, {}, identity, -1, none));
	// The rest pose written as a pose gives the same frames.
	std::vector<PoseBone> pose = rest_pose(rig);
	PosedBoneFrames posed_rest;
	TEST_EXPECT(world::posed_bone_frames(rig, pose, identity, hand_bone, posed_rest));
	TEST_EXPECT(same_turn(posed_rest.model_to_world, identity) &&
			near(posed_rest.joint_world.origin, rest.joint_world.origin));

	// ---- 2. the held gun ----
	const float zero[3] = {0.0f, 0.0f, 0.0f};
	{
		// At rest the nudge lands unrotated on the joint.
		const PoseTransform gun = world::held_weapon_attach(rest, false, zero);
		TEST_EXPECT(near(gun.origin, add(rest.joint_world.origin, gun_nudge)));
	}
	// Turn the hand 90 degrees about up in its parent's frame: the nudge follows it.
	std::vector<PoseBone> turned = pose;
	const PoseTransform turn = yaw_turn(90.0);
	turned[hand_bone].rotation = anim::quat_mul(anim::mat3_to_quat(turn.rows), pose[hand_bone].rotation);
	PosedBoneFrames hand;
	TEST_EXPECT(world::posed_bone_frames(rig, turned, identity, hand_bone, hand));
	TEST_EXPECT(same_turn(hand.model_to_world, turn));
	{
		const PoseTransform gun = world::held_weapon_attach(hand, false, zero);
		TEST_EXPECT(near(gun.origin, add(hand.joint_world.origin, anim::rest_transform_direction(turn, gun_nudge))));
	}
	// The entity frame sends the barrel (+Z) down the attach pitch and yaw, whatever the hand.
	const float cases[][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 90.0f, 0.0f}, {17.95f, 13.57f, 0.0f}, {30.0f, 0.0f, 0.0f},
			{-20.0f, -45.0f, 0.0f}};
	for (const auto &angles : cases) {
		const PoseTransform a = world::held_weapon_attach(rest, false, angles);
		const PoseTransform b = world::held_weapon_attach(hand, false, angles);
		TEST_EXPECT(same_turn(a, b));
		const Vec3 dir = anim::rest_transform_direction(a, Vec3{0.0f, 0.0f, 1.0f});
		const double elevation = std::asin(std::fmax(-1.0, std::fmin(1.0, static_cast<double>(dir.y)))) * 180.0 / kPi;
		const double bearing = std::atan2(static_cast<double>(dir.x), -static_cast<double>(dir.z)) * 180.0 / kPi;
		TEST_EXPECT(std::fabs(elevation - angles[0]) < 0.01);
		TEST_EXPECT(std::fabs(bearing - angles[1]) < 0.01);
	}
	// The hand frame ignores the attach angles and tracks the bone.
	{
		const float a1[3] = {11.0f, 22.0f, 33.0f};
		const float a2[3] = {-44.0f, 5.0f, 0.0f};
		const PoseTransform x1 = world::held_weapon_attach(rest, true, a1);
		const PoseTransform x2 = world::held_weapon_attach(rest, true, a2);
		TEST_EXPECT(same_turn(x1, x2) && near(x1.origin, x2.origin));
		TEST_EXPECT(!same_turn(x1, world::held_weapon_attach(rest, false, a1)));
		const PoseTransform after = world::held_weapon_attach(hand, true, a1);
		TEST_EXPECT(same_turn(after, anim::rest_mul(turn, x1)));
	}
	// The body's world places it.
	{
		PoseTransform body = yaw_turn(90.0);
		body.origin = Vec3{10.0f, 0.0f, -5.0f};
		PosedBoneFrames placed;
		TEST_EXPECT(world::posed_bone_frames(rig, {}, body, hand_bone, placed));
		const PoseTransform gun = world::held_weapon_attach(placed, false, zero);
		TEST_EXPECT(near(gun.origin, anim::rest_transform_point(body, add(rest.joint_world.origin, gun_nudge))));
	}

	// ---- 3. the overlays ----
	{
		PosedBoneFrames head;
		TEST_EXPECT(world::posed_bone_frames(rig, {}, identity, world::kNightVisionBoneIndex, head));
		const PoseTransform goggles = world::night_vision_attach(head);
		TEST_EXPECT(same_turn(goggles, identity));
		TEST_EXPECT(near(goggles.origin, add(head.joint_world.origin,
				Vec3{world::kNightVisionNudgeX, world::kNightVisionNudgeY, world::kNightVisionNudgeZ})));
		PosedBoneFrames left;
		TEST_EXPECT(world::posed_bone_frames(rig, {}, identity, world::kBinocularsBoneIndex, left));
		const PoseTransform binoculars = world::binoculars_attach(left);
		TEST_EXPECT(near(binoculars.origin, add(left.joint_world.origin,
				Vec3{world::kBinocularsNudgeX, world::kBinocularsNudgeY, world::kBinocularsNudgeZ})));
		TEST_EXPECT(!same_turn(binoculars, identity));
		// The carried object: the binocular point, the carrier's turn after the fixed one.
		const float carrier[3] = {0.0f, -40.0f, 0.0f};
		const PoseTransform carried = world::carried_attach(left, carrier);
		TEST_EXPECT(near(carried.origin, binoculars.origin));
		const mission::PlacementBasis pb = mission::bms_to_presentation_basis(0.0f, -40.0f, 0.0f);
		PoseTransform placement;
		placement.rows[0] = pb.x.x; placement.rows[1] = pb.y.x; placement.rows[2] = pb.z.x;
		placement.rows[3] = pb.x.y; placement.rows[4] = pb.y.y; placement.rows[5] = pb.z.y;
		placement.rows[6] = pb.x.z; placement.rows[7] = pb.y.z; placement.rows[8] = pb.z.z;
		// The fixed turn is small (about a third of a degree), about render X.
		TEST_EXPECT(same_turn(carried, placement, 1e-2f) && !same_turn(carried, placement, 1e-5f));
		// The canopy: the heading turn alone, at bone 0's model origin.
		PosedBoneFrames root;
		PoseTransform body = yaw_turn(25.0);
		body.origin = Vec3{1.0f, 2.0f, 3.0f};
		TEST_EXPECT(world::posed_bone_frames(rig, turned, body, world::kCanopyBoneIndex, root));
		const PoseTransform canopy = world::canopy_attach(root, 30.0f);
		const mission::PlacementBasis cb = mission::bms_to_presentation_basis(0.0f, 30.0f, 0.0f);
		TEST_EXPECT(near(canopy.rows[0], cb.x.x) && near(canopy.rows[3], cb.x.y) && near(canopy.rows[6], cb.x.z));
		TEST_EXPECT(near(canopy.origin, root.model_to_world.origin) && near(canopy.origin, body.origin));
	}

	// ---- 4. the part frame ----
	{
		PoseTransform part;
		TEST_EXPECT(world::posed_part_to_world(rig, turned, identity, hand_bone, part));
		TEST_EXPECT(same_turn(part, hand.model_to_world) && near(part.origin, hand.model_to_world.origin));
		TEST_EXPECT(!world::posed_part_to_world(rig, turned, identity, 99, part));
	}
	return 0;
}
