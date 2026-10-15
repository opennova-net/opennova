#pragma once

// A ragdoll's body plan over one skeletal rig (ADR 0060): which bones the
// solver simulates as capsule bodies, which ride along welded to them, and the
// joint between each body and its parent. OpenNova's own presentation
// feature, not a port: retail moves a corpse as one body and never ragdolls
// (Entity_UpdateInfantryAI's corpse legs), so the plan is authored here.
//
// The plan is the JO person rig's: the 19 BN## bones in model-table order
// (world-wac-ai-re §14.2: 0 Hips .. 18 L Foot), the order the BN tags name.
// Any other rig gets no plan and keeps its death clip.

#include <runtime/anim/skeletal_pose.h>
#include <runtime/ragdoll/ragdoll_math.h>

#include <cstdint>
#include <vector>

namespace opennova::ragdoll {

// The JO person rig's BN## bones by model index.
namespace bone {
enum : int {
	kHips = 0,
	kLowerSpine = 1,
	kUpperSpine = 2,
	kRClavicle = 3,
	kLClavicle = 4,
	kRUpperArm = 5,
	kLUpperArm = 6,
	kRThigh = 7,
	kLThigh = 8,
	kRForearm = 9,
	kLForearm = 10,
	kRCalf = 11,
	kLCalf = 12,
	kNeck = 13,
	kHead = 14,
	kLHand = 15,
	kRHand = 16,
	kRFoot = 17,
	kLFoot = 18,
	kCount = 19,
};
} // namespace bone

enum class JointKind : uint8_t { Ball, Hinge };

// One simulated body: a capsule riding its bone's frame (the bone's model
// global at rest), in model units. The body's own origin is its center of
// mass, so every offset below is relative to it.
struct BodyDef {
	int bone = -1;
	int parent = -1; // parent body (-1: the root, the pelvis)
	float inv_mass = 0.0f;
	float inv_inertia = 0.0f; // one isotropic moment (the capsule's transverse one)
	float radius = 0.0f;
	Vec3 com; // the center of mass in the bone frame
	Vec3 seg_a, seg_b; // the capsule segment, relative to the COM
	// The joint to `parent`, at this bone's pivot.
	JointKind joint = JointKind::Ball;
	Vec3 anchor_child; // the pivot relative to this body's COM (bone frame)
	Vec3 anchor_parent; // the pivot relative to the parent's COM (parent bone frame)
	Quat rest_rel; // this bone's rest rotation in its parent body's bone frame
	Vec3 axis; // the limb direction at rest (bone frame): the twist axis
	Vec3 hinge; // the hinge axis (bone frame), hinge joints only
	Vec3 cone; // the swing cone's center in the parent bone frame
	float swing_max = 0.0f; // ball: the cone's half angle
	float twist_min = 0.0f; // ball: twist about `axis`; hinge: the bend about `hinge`
	float twist_max = 0.0f;
};

struct Rig {
	std::vector<BodyDef> bodies; // [0] the pelvis; every parent ahead of its children
	std::vector<int> bone_body; // per bone: the body that carries it (-1: none)
	std::vector<int> parents; // per bone: its parent bone (-1: a root)
	float height = 0.0f; // the rest stature the tunables scale by
	bool valid() const { return !bodies.empty(); }
};

// What a rig gives the plan: its bones' parents (each ahead of its children),
// each bone's BN## index (anim::model_bone_index_from_name, -1 untagged) and
// its rest pose, parent-local.
struct RigSource {
	std::vector<int> parents;
	std::vector<int> bn;
	std::vector<anim::PoseBone> rest;
};

// The plan, or an empty Rig when the source is not the JO person rig.
Rig build_rig(const RigSource &source);

// A pose's model-space bone globals (rotation, pivot) by parent-local FK.
void pose_globals(const std::vector<int> &parents, const std::vector<anim::PoseBone> &pose,
		std::vector<Quat> &r_rotation, std::vector<Vec3> &r_pivot);

} // namespace opennova::ragdoll
