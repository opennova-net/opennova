#include "ragdoll_rig.h"

#include <cmath>

namespace opennova::ragdoll {

namespace {

constexpr float kDegrees = 3.14159265358979323846f / 180.0f;

// The model frame's up and forward: bones pose in the engine-native Y-up model
// space, and a placed model faces its +Z (the placement basis turns +Z onto
// the entity's heading, runtime/mission/placement_traits.h).
constexpr Vec3 kUp{ 0.0f, 1.0f, 0.0f };
constexpr Vec3 kForward{ 0.0f, 0.0f, 1.0f };

enum class Shape : uint8_t { Limb, Pelvis, Head };

// One body of the plan. Radii scale by the rig's stature; masses are relative
// (the solver only ever divides them into one another). A ball joint's cone
// is centered `cone_forward` degrees ahead of the limb's rest direction; a
// hinge bends toward the front (+1, the elbow) or the back (-1, the knee).
struct BodySpec {
	int bone;
	int tip; // the bone whose pivot ends a limb capsule
	Shape shape;
	float radius;
	float mass;
	JointKind joint;
	float swing_deg;
	float twist_min_deg;
	float twist_max_deg;
	float cone_forward_deg;
	int bend;
};

// Ancestors first: a body's parent is the body of its bone's nearest simulated
// ancestor, which this order always lists earlier.
constexpr BodySpec kBodies[] = {
	{ bone::kHips, -1, Shape::Pelvis, 0.075f, 12.0f, JointKind::Ball, 0, 0, 0, 0, 0 },
	{ bone::kLowerSpine, bone::kUpperSpine, Shape::Limb, 0.08f, 10.0f, JointKind::Ball, 25, -15, 15,
			10, 0 },
	{ bone::kUpperSpine, bone::kNeck, Shape::Limb, 0.095f, 16.0f, JointKind::Ball, 25, -15, 15, 10,
			0 },
	{ bone::kHead, -1, Shape::Head, 0.06f, 5.0f, JointKind::Ball, 45, -50, 50, 10, 0 },
	{ bone::kRUpperArm, bone::kRForearm, Shape::Limb, 0.035f, 2.5f, JointKind::Ball, 100, -70, 70,
			20, 0 },
	{ bone::kLUpperArm, bone::kLForearm, Shape::Limb, 0.035f, 2.5f, JointKind::Ball, 100, -70, 70,
			20, 0 },
	{ bone::kRForearm, bone::kRHand, Shape::Limb, 0.03f, 2.0f, JointKind::Hinge, 0, -5, 140, 0, 1 },
	{ bone::kLForearm, bone::kLHand, Shape::Limb, 0.03f, 2.0f, JointKind::Hinge, 0, -5, 140, 0, 1 },
	{ bone::kRThigh, bone::kRCalf, Shape::Limb, 0.05f, 8.0f, JointKind::Ball, 65, -30, 30, 30, 0 },
	{ bone::kLThigh, bone::kLCalf, Shape::Limb, 0.05f, 8.0f, JointKind::Ball, 65, -30, 30, 30, 0 },
	{ bone::kRCalf, bone::kRFoot, Shape::Limb, 0.04f, 5.0f, JointKind::Hinge, 0, -5, 140, 0, -1 },
	{ bone::kLCalf, bone::kLFoot, Shape::Limb, 0.04f, 5.0f, JointKind::Hinge, 0, -5, 140, 0, -1 },
};
constexpr int kBodyCount = static_cast<int>(sizeof(kBodies) / sizeof(kBodies[0]));

// The forearm capsule runs on past the wrist over the welded hand.
constexpr float kHandReach = 0.05f;
// The head capsule: from just above the skull's pivot along the neck's line.
constexpr float kHeadNear = 0.03f;
constexpr float kHeadFar = 0.09f;

// `v` turned `degrees` toward `toward` (about their common normal); `v` itself
// when the two are parallel.
Vec3 turned_toward(const Vec3 &v, const Vec3 &toward, float degrees) {
	const Vec3 k = cross(v, toward);
	if (length(k) < 1e-6f)
		return v;
	return quat_rotate(quat_axis_angle(normalized(k), degrees * kDegrees), v);
}

} // namespace

void pose_globals(const std::vector<int> &parents, const std::vector<anim::PoseBone> &pose,
		std::vector<Quat> &r_rotation, std::vector<Vec3> &r_pivot) {
	const size_t n = pose.size() < parents.size() ? pose.size() : parents.size();
	r_rotation.assign(n, Quat{});
	r_pivot.assign(n, Vec3{});
	for (size_t i = 0; i < n; ++i) {
		const int p = parents[i];
		if (p >= 0 && static_cast<size_t>(p) < i) {
			r_rotation[i] = quat_normalized(quat_mul(r_rotation[p], pose[i].rotation));
			r_pivot[i] = r_pivot[p] + quat_rotate(r_rotation[p], pose[i].origin);
		} else {
			r_rotation[i] = quat_normalized(pose[i].rotation);
			r_pivot[i] = pose[i].origin;
		}
	}
}

Rig build_rig(const RigSource &source) {
	Rig rig;
	const size_t n = source.parents.size();
	if (n < static_cast<size_t>(bone::kCount) || source.rest.size() < n || source.bn.size() < n)
		return rig;
	for (int i = 0; i < bone::kCount; ++i) {
		if (source.bn[i] != i)
			return rig;
	}
	for (size_t i = 0; i < n; ++i) {
		const int p = source.parents[i];
		if (p >= static_cast<int>(i))
			return rig;
	}

	std::vector<Quat> rot;
	std::vector<Vec3> pivot;
	pose_globals(source.parents, source.rest, rot, pivot);
	const float foot = std::fmin(pivot[bone::kRFoot].y, pivot[bone::kLFoot].y);
	const float height = pivot[bone::kHead].y - foot;
	if (!(height > 1e-3f))
		return rig;

	// Each bone's body: its own when simulated, else its nearest simulated
	// ancestor's (welded), else none.
	std::vector<int> body_of_bone(n, -1);
	for (int b = 0; b < kBodyCount; ++b)
		body_of_bone[kBodies[b].bone] = b;
	for (size_t i = 0; i < n; ++i) {
		if (body_of_bone[i] >= 0)
			continue;
		const int p = source.parents[i];
		if (p >= 0)
			body_of_bone[i] = body_of_bone[p];
	}

	std::vector<BodyDef> bodies(kBodyCount);
	std::vector<Vec3> com_model(kBodyCount);
	for (int b = 0; b < kBodyCount; ++b) {
		const BodySpec &spec = kBodies[b];
		BodyDef &def = bodies[b];
		def.bone = spec.bone;
		const int pb = source.parents[spec.bone];
		def.parent = (b == 0) ? -1 : (pb >= 0 ? body_of_bone[pb] : -1);
		if (b > 0 && (def.parent < 0 || def.parent >= b))
			return Rig{};

		const Vec3 &p0 = pivot[spec.bone];
		Vec3 a, c, dir;
		switch (spec.shape) {
			case Shape::Pelvis:
				a = pivot[bone::kRThigh];
				c = pivot[bone::kLThigh];
				dir = kUp;
				break;
			case Shape::Head:
				dir = normalized(p0 - pivot[bone::kNeck], kUp);
				a = p0 + dir * (kHeadNear * height);
				c = p0 + dir * (kHeadFar * height);
				break;
			case Shape::Limb:
				a = p0;
				c = pivot[spec.tip];
				dir = normalized(c - a, -kUp);
				if (spec.tip == bone::kRHand || spec.tip == bone::kLHand)
					c += dir * (kHandReach * height);
				break;
		}
		const Vec3 com = (a + c) * 0.5f;
		com_model[b] = com;
		const Quat inv = quat_conj(rot[spec.bone]);
		def.radius = spec.radius * height;
		def.com = quat_rotate(inv, com - p0);
		def.seg_a = quat_rotate(inv, a - com);
		def.seg_b = quat_rotate(inv, c - com);
		def.anchor_child = quat_rotate(inv, p0 - com);
		const float len = length(c - a);
		const float r = def.radius;
		float inertia = spec.mass * (3.0f * r * r + len * len) / 12.0f;
		inertia = std::fmax(inertia, 0.4f * spec.mass * r * r);
		def.inv_mass = 1.0f / spec.mass;
		def.inv_inertia = 1.0f / inertia;
		def.axis = quat_rotate(inv, dir);

		def.joint = spec.joint;
		def.swing_max = spec.swing_deg * kDegrees;
		def.twist_min = spec.twist_min_deg * kDegrees;
		def.twist_max = spec.twist_max_deg * kDegrees;
		if (def.parent >= 0) {
			const int parent_bone = kBodies[def.parent].bone;
			const Quat parent_inv = quat_conj(rot[parent_bone]);
			def.anchor_parent = quat_rotate(parent_inv, p0 - com_model[def.parent]);
			def.rest_rel = quat_normalized(quat_mul(parent_inv, rot[spec.bone]));
			def.cone = quat_rotate(parent_inv, turned_toward(dir, kForward, spec.cone_forward_deg));
			if (spec.joint == JointKind::Hinge) {
				Vec3 h = cross(dir, kForward * static_cast<float>(spec.bend));
				h = normalized(h, Vec3{ 1.0f, 0.0f, 0.0f });
				def.hinge = quat_rotate(inv, h);
			}
		}
	}

	rig.bodies = std::move(bodies);
	rig.bone_body = std::move(body_of_bone);
	rig.parents = source.parents;
	rig.height = height;
	return rig;
}

} // namespace opennova::ragdoll
