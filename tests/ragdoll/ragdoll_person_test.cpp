// The opt-in ragdoll corpses (ADR 0060) on a JO person rig read from files as
// the game reads a soldier's: the minted person model and its clip set
// (common/rig19_set.h: person.3di and rig19.adm on the stock 19-bone body rig,
// the US01 layout). The body plan takes it, the model frame the plan assumes
// holds (a crouch puts the knees toward +Z, the model's front), each
// death_bullet_torso clip's fall is reported, and a ragdoll started on a death
// clip settles on flat ground with no body sunk into it.
#include "common/rig19_set.h"
#include "common/temp_dir.h"

#include <base/resource_index/resource_index.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/assets/asset_store.h>
#include <runtime/ragdoll/ragdoll_rig.h>
#include <runtime/ragdoll/ragdoll_solver.h>
#include <runtime/world/corpse_ragdoll.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/infantry.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::ragdoll;

namespace {

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond)
		return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

struct FlatGround final : Collider {
	bool ground(float, float, float &h, Vec3 &n) const override {
		h = 0.0f;
		n = Vec3{ 0.0f, 1.0f, 0.0f };
		return true;
	}
	bool sweep(const Vec3 &, const Vec3 &, Vec3 &, Vec3 &) const override { return false; }
};

void pivots(const anim::SkeletalClips &clips, const std::string &key, double seconds,
		std::vector<Vec3> &r_pivot) {
	std::vector<anim::PoseBone> pose;
	clips.eval_pose(key, seconds, 0, pose);
	std::vector<Quat> rot;
	pose_globals(clips.parents(), pose, rot, r_pivot);
}

} // namespace

int main() {
	test_temp::TempDir root("opennova_ragdoll_person");
	if (!expect(rig19::stage(root.root()), "the person rig stages"))
		return 1;
	ResourceIndex index;
	if (!expect(index.scan(root.root(), std::string(), VfsMountMode::LooseOnly), "the staged rig mounts"))
		return 1;
	assets::AssetStore models{ &index };
	const threedi::Threedi3di3 *model = models.model("person").get();
	if (!expect(model != nullptr, "person.3di loads"))
		return 1;
	std::vector<anim::Vec3> origins;
	std::vector<int> parents;
	if (!expect(world::model_bone_table(*model, origins, parents), "person carries a bone table"))
		return 1;
	anim::SkeletalClips clips;
	if (!expect(clips.load_from_adm(&models, "rig19.adm", origins, parents), "rig19.adm loads"))
		return 1;

	RigSource source;
	source.parents = clips.parents();
	for (const anim::ClipBone &b : clips.bones())
		source.bn.push_back(anim::model_bone_index_from_name(b.name));
	clips.eval_pose(std::string(), 0.0, 0, source.rest);
	std::vector<Quat> rest_rot;
	std::vector<Vec3> rest_pivot;
	pose_globals(source.parents, source.rest, rest_rot, rest_pivot);
	for (size_t i = 0; i < clips.bone_count(); ++i) {
		std::printf("bone %2zu %-16s parent %2d bn %2d pivot (%6.3f %6.3f %6.3f)\n", i,
				clips.bones()[i].name.c_str(), source.parents[i], source.bn[i], rest_pivot[i].x,
				rest_pivot[i].y, rest_pivot[i].z);
	}
	const auto rig = std::make_shared<const Rig>(build_rig(source));
	if (!expect(rig->valid(), "the person rig gets a ragdoll plan"))
		return 1;
	std::printf("stature %.3f, %zu bodies\n", rig->height, rig->bodies.size());

	// The front: a crouch bends the knees toward the model's +Z.
	std::vector<Vec3> crouch;
	pivots(clips, "anim_idle_crouch", 0.0, crouch);
	for (int side = 0; side < 2; ++side) {
		const int hip = side == 0 ? bone::kRThigh : bone::kLThigh;
		const int knee = side == 0 ? bone::kRCalf : bone::kLCalf;
		const int ankle = side == 0 ? bone::kRFoot : bone::kLFoot;
		const Vec3 mid = (crouch[hip] + crouch[ankle]) * 0.5f;
		const float ahead = crouch[knee].z - mid.z;
		std::printf("crouch knee %d ahead of the hip-ankle line by %.3f (z)\n", side, ahead);
		expect(ahead > 0.0f, "a crouching knee points to the model's +Z");
	}

	// Each torso clip's fall: where the head ends against where it began.
	static const char *const kTorso[4] = { "anim_death_bullet_torso_forward",
		"anim_death_bullet_torso_right", "anim_death_bullet_torso_back",
		"anim_death_bullet_torso_left" };
	for (int q = 0; q < 4; ++q) {
		if (!expect(clips.has_clip(kTorso[q]), "the set carries every torso death"))
			continue;
		std::vector<Vec3> first, last;
		pivots(clips, kTorso[q], 0.0, first);
		pivots(clips, kTorso[q], clips.clip_length(kTorso[q]), last);
		const Vec3 fall = last[bone::kHead] - first[bone::kHead];
		std::printf("%s: head falls (%.2f, %.2f, %.2f) over %.2f s\n", kTorso[q], fall.x, fall.y,
				fall.z, clips.clip_length(kTorso[q]));
	}

	// A ragdoll on each death clip's first frame settles on flat ground.
	FlatGround ground;
	for (int q = 0; q < 4; ++q) {
		if (!clips.has_clip(kTorso[q]))
			continue;
		std::vector<anim::PoseBone> pose, prev;
		clips.eval_pose(kTorso[q], 1.0 / 62.5, 0, pose);
		clips.eval_pose(kTorso[q], 0.0, 0, prev);
		std::vector<Quat> rot;
		std::vector<Vec3> piv;
		pose_globals(source.parents, pose, rot, piv);
		const float foot = std::fmin(piv[bone::kRFoot].y, piv[bone::kLFoot].y);
		StartInput in;
		in.node.origin = Vec3{ 0.0f, -foot + 0.05f, 0.0f };
		in.node_prev = in.node;
		in.pose = &pose;
		in.pose_prev = &prev;
		in.kick = world::corpse_death(world::anim_state::kDeathBulletBase + 4 + q, 1).kick;
		in.anchor = in.node.origin + piv[bone::kHips];
		Ragdoll r;
		if (!expect(r.start(rig, in, ground), "a ragdoll starts on the death clip"))
			continue;
		const double len = clips.clip_length(kTorso[q]);
		float deepest = 0.0f;
		int t = 0;
		for (; t < 600 && !r.asleep() && !r.failed(); ++t) {
			// The anchor follows the clip's hips, as the simulation's corpse does.
			const double s = std::fmin(len, (t + 2) / 62.5);
			std::vector<Vec3> at;
			pivots(clips, kTorso[q], s, at);
			r.step(ground, in.node.origin + at[bone::kHips], 1.0f / 62.5f);
			deepest = std::fmin(deepest, r.lowest_clearance(ground));
		}
		std::printf("%s ragdoll: rest after %d ticks, deepest %.4f, hips %.3f\n", kTorso[q], t,
				deepest, r.body_pivot(0).y);
		expect(!r.failed(), "the death clip ragdoll stays finite");
		expect(r.asleep(), "the death clip ragdoll comes to rest");
		expect(deepest > -0.05f * rig->height, "no body sinks into the ground");
	}
	if (failures == 0)
		std::printf("ragdoll_person: all passed\n");
	return failures == 0 ? 0 : 1;
}
