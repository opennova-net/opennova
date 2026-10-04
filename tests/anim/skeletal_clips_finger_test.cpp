/* engine/runtime/anim SkeletalClips: a BN## part past the person rig's 19
   bones follows the part it hangs from (ADR 0047 decision 16). Retail gives
   every such bone the body matrix and the primary channel; OpenNova's own
   bodies carry finger bones there, which must aim and hold with their hand.

   The rig is minted here through the .bad construction seam: the person rig's
   19 bones, then BN20 (a finger below BN17 R Hand) and BN21 (a pouch below
   BN01 Hips). Three clips turn the hand, the finger, the pouch and a thigh by
   different angles, so the composed pose tells which channel each bone took:

     1. the class and the mask: the finger takes the arm class and the weapon
        channel, the pouch the body class and the primary channel;
     2. the splice: the finger's world rotation is the weapon clip's, the
        pouch's and the thigh's the primary clip's;
     3. the overlay: an arm-class delta turns the hand and the finger together
        and leaves the pouch. */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/bad/bad_build.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/assets/asset_store.h>

using namespace opennova::bad;

using opennova::anim::PoseBone;
using opennova::anim::Quat;
using opennova::anim::SkeletalClips;

namespace {

constexpr int kBones = 21;
constexpr int kThigh = 7;   // BN08 R Thigh: unmasked, in the table
constexpr int kHand = 16;   // BN17 R Hand: masked, arm class
constexpr int kFinger = 19; // BN20: past the table, below the hand
constexpr int kPouch = 20;  // BN21: past the table, below the hips

const int kParents[kBones] = {-1, 0, 1, 2, 2, 3, 4, 0, 0, 5, 6, 7, 8, 2, 13, 10, 9, 11, 12, kHand, 0};
const char *const kNames[kBones] = {
		"BN01 Hips", "BN02 Lower Spine", "BN03 Upper Spine", "BN04 R Clavicle", "BN05 L Clavicle",
		"BN06 R UpperArm", "BN07 L UpperArm", "BN08 R Thigh", "BN09 L Thigh", "BN10 R Forearm",
		"BN11 L Forearm", "BN12 R Calf", "BN13 L Calf", "BN14 Neck", "BN15 Head", "BN16 L Hand",
		"BN17 R Hand", "BN18 R Foot", "BN19 L Foot", "BN20 R Index1", "BN21 Pouch"};

BadBuildQuat yaw(double degrees) {
	const double half = degrees * 3.14159265358979323846 / 360.0;
	return BadBuildQuat{0.0, 0.0, std::sin(half), std::cos(half)};
}

// A two-key clip that turns four bones in the model's frame and rests the others.
BadBuildClip clip(const std::string &name, double thigh, double hand, double finger, double pouch) {
	BadBuildClip out;
	out.name = name;
	out.frame_count = 1;
	for (int i = 0; i < kBones; ++i) {
		BadBuildBone bone;
		bone.name = kNames[i];
		bone.parent = kParents[i];
		const double degrees = i == kThigh ? thigh : i == kHand ? hand : i == kFinger ? finger : i == kPouch ? pouch : 0.0;
		bone.keys.assign(2, yaw(degrees));
		out.bones.push_back(bone);
	}
	return out;
}

std::vector<Quat> fk(const std::vector<int> &parents, const std::vector<PoseBone> &pose) {
	std::vector<Quat> world(pose.size());
	for (size_t i = 0; i < pose.size(); ++i) {
		const int p = parents[i];
		world[i] = (p >= 0 && static_cast<size_t>(p) < i)
				? opennova::anim::quat_mul(world[static_cast<size_t>(p)], pose[i].rotation)
				: pose[i].rotation;
	}
	return world;
}

float angle_between(Quat a, Quat b) {
	a = opennova::anim::quat_normalize(a);
	b = opennova::anim::quat_normalize(b);
	float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
	if (d < 0) d = -d;
	if (d > 1.0f) d = 1.0f;
	return 2.0f * std::acos(d);
}

} // namespace

int main() {
	const std::string dir = std::string(test_paths_temp_dir()) + "/opennova_finger_test";
#ifdef _WIN32
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0777);
#endif

	BadBuildSet set;
	set.adm_name = "rig.adm";
	set.rows = {{"anim_reset", {"reset"}}, {"anim_walk_forward", {"walk"}}, {"anim_knife", {"hold"}}};
	set.clips = {clip("reset", 0.0, 0.0, 0.0, 0.0), clip("walk", 35.0, 10.0, 10.0, 50.0),
			clip("hold", 0.0, 40.0, 70.0, 25.0)};
	std::string error;
	for (const BadBuildClip &c : set.clips) {
		std::vector<uint8_t> bytes;
		TEST_EXPECT(bad_build_mint(c, &set.clips[0], bytes, &error));
		TEST_EXPECT(test_io::write_file(dir + "/" + c.name + ".bad", bytes));
	}
	std::string table;
	TEST_EXPECT(bad_build_mint_table(set, table, &error));
	TEST_EXPECT(test_io::write_file(dir + "/rig.adm", std::vector<uint8_t>(table.begin(), table.end())));

	opennova::ResourceIndex index;
	opennova::assets::AssetStore assets{&index};
	TEST_EXPECT(index.scan(dir));
	const std::vector<opennova::anim::Vec3> origins(kBones);
	const std::vector<int> parents(kParents, kParents + kBones);
	SkeletalClips clips;
	TEST_EXPECT(clips.load_from_adm(&assets, "rig.adm", origins, parents));
	TEST_EXPECT(clips.fk_valid());

	// ---- 1. the class ----
	const std::vector<uint8_t> &classes = clips.overlay_classes();
	TEST_EXPECT(classes.size() == static_cast<size_t>(kBones));
	TEST_EXPECT(classes[kHand] == opennova::anim::kOverlayArm);
	TEST_EXPECT(classes[kFinger] == opennova::anim::kOverlayArm);
	TEST_EXPECT(classes[kPouch] == opennova::anim::kOverlayBody);
	// The name alone still reads as retail's default case.
	TEST_EXPECT(opennova::anim::overlay_class_for_bone_name(kNames[kFinger]) == opennova::anim::kOverlayBody);

	// ---- 2. the splice ----
	std::vector<PoseBone> primary, weapon, composed;
	clips.eval_pose("anim_walk_forward", 0.0, 0, primary);
	clips.eval_pose("anim_knife", 0.0, 0, weapon);
	const std::vector<Quat> primary_w = fk(parents, primary);
	const std::vector<Quat> weapon_w = fk(parents, weapon);
	TEST_EXPECT(angle_between(primary_w[kFinger], weapon_w[kFinger]) > 0.5f);
	TEST_EXPECT(angle_between(primary_w[kPouch], weapon_w[kPouch]) > 0.3f);
	TEST_EXPECT(clips.eval_composed_pose("anim_walk_forward", 0.0, false, "", 0.0, 1.0f, nullptr,
			"anim_knife", 0.0, composed));
	const std::vector<Quat> w = fk(parents, composed);
	TEST_EXPECT(angle_between(w[kHand], weapon_w[kHand]) < 1e-3f);
	TEST_EXPECT(angle_between(w[kFinger], weapon_w[kFinger]) < 1e-3f);
	TEST_EXPECT(angle_between(w[kPouch], primary_w[kPouch]) < 1e-3f);
	TEST_EXPECT(angle_between(w[kThigh], primary_w[kThigh]) < 1e-3f);

	// ---- 3. the overlay ----
	Quat deltas[opennova::anim::kOverlayClassCount];
	for (Quat &d : deltas) d = Quat{1.0f, 0.0f, 0.0f, 0.0f};
	const float half = 0.3f;
	deltas[opennova::anim::kOverlayArm] = Quat{std::cos(half), std::sin(half), 0.0f, 0.0f};
	std::vector<PoseBone> aimed;
	TEST_EXPECT(clips.eval_composed_pose("anim_walk_forward", 0.0, false, "", 0.0, 1.0f, deltas,
			"anim_knife", 0.0, aimed));
	const std::vector<Quat> a = fk(parents, aimed);
	const Quat &arm = deltas[opennova::anim::kOverlayArm];
	TEST_EXPECT(angle_between(a[kHand], opennova::anim::quat_mul(arm, w[kHand])) < 1e-3f);
	TEST_EXPECT(angle_between(a[kFinger], opennova::anim::quat_mul(arm, w[kFinger])) < 1e-3f);
	TEST_EXPECT(angle_between(a[kPouch], w[kPouch]) < 1e-3f);

	std::printf("anim skeletal clips finger: OK\n");
	return 0;
}
