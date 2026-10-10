/* engine/runtime/anim SkeletalClips — a Person's model rows past the clip it plays, and the rig a
   per-vertex skinned model with no clip draws through, over in-memory rigs (no retail data):

     1. the rows past the clip: a 21-row model table (BHD's Delta01 shape: row 19 the jaw under the
        head row 14, row 20 under the root) over a 19-bone clip. The plain clip pose carries row 0's
        turn into both (BoneAnim's padding); the Person's bone builder gives row 19 the head row's
        finished matrix (the clip has more than 14 bones) and row 20 the model frame's
        [orig: Entity_BuildBoneTransformMatrices @0x4B1290, identity @0x4B1EC6..0x4B1EE8, row 14's
        copy @0x4B2002..0x4B2018];
     2. the threshold: a clip of 14 bones gives every row past it the model frame's, row 14 included;
     3. the overlay: a row past the clip takes its class's turn on top of the model frame's;
     4. eval_composed_pose takes the rule (the collision pose is the same builder's);
     5. load_rest: the model table at rest with no clip, every row's deformation identity. */

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"

#include <formats/bad/bad.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>

using namespace opennova;
using anim::PoseBone;
using anim::Quat;
using anim::SkeletalClips;

namespace {

constexpr size_t kClipBones = 19;
constexpr size_t kModelRows = 21;
constexpr size_t kHead = 14;
constexpr size_t kJaw = 19;
constexpr size_t kGround = 20;

// One clip of `bones` bones, one key each: the root turned +90 about Y, the head +40 about X, the
// rest identity; identity binds.
struct MemoryClip {
	std::vector<bad::BadBone> bones;
	std::vector<bad::BadChannel> channels;
	std::vector<bad::BadQuaternion> rotations;
	std::vector<uint16_t> lengths;
	bad::BadFile file = {};

	explicit MemoryClip(size_t count) : bones(count), channels(count), rotations(count), lengths(count, 1) {
		const float s45 = std::sin(0.785398f), c45 = std::cos(0.785398f);
		const float s20 = std::sin(0.349066f), c20 = std::cos(0.349066f);
		for (size_t i = 0; i < count; ++i) {
			bones[i].parent_index = i == 0 ? -1 : static_cast<int32_t>(i - 1);
			bones[i].rotation[0] = bones[i].rotation[4] = bones[i].rotation[8] = 1.0f;
			rotations[i] = {0.0f, 0.0f, 0.0f, 1.0f};
			channels[i].frame_count = 1;
			channels[i].frame_lengths = &lengths[i];
			channels[i].rotations = &rotations[i];
		}
		rotations[0] = {0.0f, s45, 0.0f, c45};
		if (count > kHead) rotations[kHead] = {s20, 0.0f, 0.0f, c20};
		file.fps = 30;
		file.frame_count = 1;
		file.bone_count = static_cast<uint32_t>(count);
		file.bones = bones.data();
		file.num_bones = count;
		file.channels = channels.data();
		file.num_channels = count;
	}
};

class MemoryFiles : public anim::RigFiles {
public:
	std::map<std::string, const bad::BadFile *> bads;
	std::shared_ptr<const adm::AdmFile> animation_map(const std::string &) const override { return nullptr; }
	std::shared_ptr<const bad::BadFile> bone_animation(const std::string &name) const override {
		const auto it = bads.find(name);
		// The clips outlive the rig's load; the store's copy is borrowed, never freed here.
		return it == bads.end() ? nullptr : std::shared_ptr<const bad::BadFile>(it->second, [](const bad::BadFile *) {});
	}
};

// The Delta01 table: rows 1..18 a chain from the root, the jaw under the head, the ground row under
// the root; every row a tenth above its parent.
void delta_table(size_t rows, std::vector<anim::Vec3> &origins, std::vector<int> &parents) {
	origins.assign(rows, anim::Vec3{0.0f, 0.1f, 0.0f});
	parents.resize(rows);
	for (size_t i = 0; i < rows; ++i) parents[i] = i == 0 ? 0 : static_cast<int>(i - 1);
	if (rows > kJaw) parents[kJaw] = static_cast<int>(kHead);
	if (rows > kGround) parents[kGround] = 0;
}

// Each row's deformation (rest to pose), as the skin and the collision poses use it.
std::vector<SkeletalClips::RestTransform> deformations(const SkeletalClips &rig, const std::vector<PoseBone> &pose) {
	std::vector<SkeletalClips::RestTransform> globals;
	anim::pose_globals(pose, rig.parents(), pose.size(), -1, globals);
	std::vector<SkeletalClips::RestTransform> out(globals.size());
	for (size_t i = 0; i < globals.size(); ++i) out[i] = anim::rest_mul(globals[i], rig.rest_global_inverse()[i]);
	return out;
}

bool same(const SkeletalClips::RestTransform &a, const SkeletalClips::RestTransform &b, float eps = 1e-4f) {
	for (int i = 0; i < 9; ++i)
		if (std::fabs(a.rows[i] - b.rows[i]) > eps) return false;
	return std::fabs(a.origin.x - b.origin.x) <= eps && std::fabs(a.origin.y - b.origin.y) <= eps &&
	       std::fabs(a.origin.z - b.origin.z) <= eps;
}

bool turns_like(const SkeletalClips::RestTransform &a, const float rows[9], float eps = 1e-4f) {
	for (int i = 0; i < 9; ++i)
		if (std::fabs(a.rows[i] - rows[i]) > eps) return false;
	return true;
}

constexpr float kIdentityRows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

} // namespace

int main() {
	MemoryClip clip(kClipBones);
	MemoryFiles files;
	files.bads["rest.bad"] = &clip.file;
	files.bads["idle.bad"] = &clip.file;
	std::vector<anim::Vec3> origins;
	std::vector<int> parents;
	delta_table(kModelRows, origins, parents);

	SkeletalClips rig;
	TEST_EXPECT(rig.load_from_files(&files, "rest.bad", {{"anim_idle", "idle.bad"}}, origins, parents));
	TEST_EXPECT(rig.bone_count() == kModelRows);
	TEST_EXPECT(rig.fk_valid());
	TEST_EXPECT(rig.clip_file_bones("anim_idle") == kClipBones);
	// A key with no clip poses the bind, the skeleton .bad's.
	TEST_EXPECT(rig.clip_file_bones("anim_absent") == kClipBones);

	// ---- 1. the rows past the clip ----
	std::vector<PoseBone> pose;
	rig.eval_pose("anim_idle", 0.0, 0, pose);
	TEST_EXPECT(pose.size() == kModelRows);
	{
		const auto plain = deformations(rig, pose);
		// The plain clip pose (BoneAnim's padding): the rows past the clip turn with row 0.
		TEST_EXPECT(turns_like(plain[kJaw], plain[0].rows));
		TEST_EXPECT(!turns_like(plain[kHead], plain[0].rows));
	}
	std::vector<PoseBone> person = pose;
	rig.pose_person_rows_past_clip(person, rig.clip_file_bones("anim_idle"), nullptr, {});
	{
		const auto plain = deformations(rig, pose);
		const auto built = deformations(rig, person);
		// The clip's own rows are untouched.
		for (size_t i = 0; i < kClipBones; ++i) TEST_EXPECT(same(built[i], plain[i]));
		// The jaw takes the head's finished matrix: the same deformation, pivot and all (its parent is
		// the head, so the head's turn about the jaw's pivot carried by the head is the head's matrix).
		TEST_EXPECT(same(built[kJaw], built[kHead]));
		// The ground row takes the model frame's turn, its pivot carried by the root.
		TEST_EXPECT(turns_like(built[kGround], kIdentityRows));
		const anim::Vec3 pivot = anim::rest_transform_point(rig.rest_global()[kGround], anim::Vec3{0.0f, 0.0f, 0.0f});
		const anim::Vec3 carried = anim::rest_transform_point(built[0], pivot);
		const anim::Vec3 placed = anim::rest_transform_point(built[kGround], pivot);
		TEST_EXPECT(std::fabs(carried.x - placed.x) < 1e-4f && std::fabs(carried.y - placed.y) < 1e-4f &&
		            std::fabs(carried.z - placed.z) < 1e-4f);
	}

	// ---- 2. the threshold: a clip of 14 bones has no head row past it ----
	{
		std::vector<PoseBone> short_clip = pose;
		rig.pose_person_rows_past_clip(short_clip, kHead, nullptr, {});
		const auto built = deformations(rig, short_clip);
		for (size_t i = kHead; i < kModelRows; ++i) TEST_EXPECT(turns_like(built[i], kIdentityRows));
	}

	// ---- 3. the overlay: a row past the clip takes its class's turn on the model frame's ----
	const float s30 = std::sin(0.261799f), c30 = std::cos(0.261799f);
	Quat deltas[anim::kOverlayClassCount];
	for (auto &d : deltas) d = Quat{1.0f, 0.0f, 0.0f, 0.0f};
	deltas[anim::kOverlayBody] = Quat{c30, 0.0f, 0.0f, s30}; // +30 about Z (w, x, y, z)
	{
		std::vector<PoseBone> overlaid = pose;
		rig.apply_pose_overlay(overlaid, deltas, rig.overlay_classes(), std::string(), 0.0);
		rig.pose_person_rows_past_clip(overlaid, kClipBones, deltas, rig.overlay_classes());
		const auto built = deformations(rig, overlaid);
		std::vector<PoseBone> turned_pose(1);
		turned_pose[0].rotation = deltas[anim::kOverlayBody];
		std::vector<SkeletalClips::RestTransform> turned;
		anim::pose_globals(turned_pose, std::vector<int>{-1}, 1, -1, turned);
		TEST_EXPECT(rig.overlay_classes()[kGround] == anim::kOverlayBody);
		TEST_EXPECT(turns_like(built[kGround], turned[0].rows));
		TEST_EXPECT(same(built[kJaw], built[kHead]));

		// ---- 4. eval_composed_pose: the same builder ----
		std::vector<PoseBone> composed;
		TEST_EXPECT(rig.eval_composed_pose("anim_idle", 0.0, false, std::string(), 0.0, 1.0f, deltas, std::string(),
		                                   0.0, composed));
		const auto via_composed = deformations(rig, composed);
		for (size_t i = 0; i < kModelRows; ++i) TEST_EXPECT(same(via_composed[i], built[i]));
	}

	// ---- 5. load_rest: the model table at rest, no clip ----
	{
		SkeletalClips rest;
		TEST_EXPECT(rest.load_rest(origins, parents));
		TEST_EXPECT(rest.loaded());
		TEST_EXPECT(rest.bone_count() == kModelRows);
		TEST_EXPECT(rest.clips().empty());
		TEST_EXPECT(rest.clip_file_bones("anim_idle") == 0);
		std::vector<PoseBone> at_rest;
		rest.eval_pose("anim_idle", 0.0, 0, at_rest);
		TEST_EXPECT(at_rest.size() == kModelRows);
		const auto built = deformations(rest, at_rest);
		for (size_t i = 0; i < kModelRows; ++i) TEST_EXPECT(turns_like(built[i], kIdentityRows));
		// Each row's rest pivot is the model's: its parent's plus its tenth (the root's a tenth up, the
		// head the fifteenth row, the jaw one more above it).
		const anim::Vec3 jaw = anim::rest_transform_point(rest.rest_global()[kJaw], anim::Vec3{0.0f, 0.0f, 0.0f});
		TEST_EXPECT(std::fabs(jaw.y - 0.1f * static_cast<float>(kHead + 2)) < 1e-4f);
		// A table whose halves differ in length is not a rig.
		TEST_EXPECT(!rest.load_rest(origins, std::vector<int>(3, 0)));
		TEST_EXPECT(!rest.loaded());
	}
	return 0;
}
