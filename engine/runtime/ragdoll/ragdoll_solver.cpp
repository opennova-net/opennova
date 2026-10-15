#include "ragdoll_solver.h"

#include <algorithm>
#include <cmath>

namespace opennova::ragdoll {

namespace {

// The infantry gravity a falling body takes, so a ragdoll drops like the
// living soldier it was: 208/65536 units per tick squared at 62.5 ticks a
// second (world/infantry.cpp's player step; the NPC step is 416 every second
// tick, the same rate).
constexpr float kGravity = 208.0f / 65536.0f * 62.5f * 62.5f;
constexpr int kSubsteps = 8;
// Velocity damping per second, linear and angular, and the joints' own
// relative-spin damping (the muscle tone that keeps a ragdoll from
// flailing).
constexpr float kLinearDamping = 0.1f;
constexpr float kAngularDamping = 1.0f;
constexpr float kJointDamping = 8.0f;
constexpr float kFriction = 0.9f;
// A limb lying on the ground does not roll like a cylinder: its spin decays
// this much faster per second while it touches.
constexpr float kRollingDamping = 12.0f;
// The fastest start motion taken from the death clip (a clip switch can jump
// a whole pose in one tick), per stature per second and radians per second.
constexpr float kMaxStartSpeed = 3.0f;
constexpr float kMaxStartSpin = 8.0f;
constexpr float kMaxSpeed = 20.0f;
constexpr float kMaxSpin = 30.0f;
// The hips' slack from the simulation's corpse, the jump past which the
// whole ragdoll follows it at once, and the drift that wakes a sleeping one
// (all per stature).
constexpr float kTetherSlack = 0.3f;
// The tether is a stiff spring past its slack, never a rod: it closes this
// share of the overshoot per second.
constexpr float kTetherStiffness = 30.0f;
constexpr float kSnapDistance = 1.5f;
constexpr float kWakeDrift = 0.05f;
// A corpse this far under the ground is indoors below it: no ground contact.
constexpr float kIndoorsDepth = 0.25f;
// Sleep: calm for kCalmTicks (speeds under these, per stature and radians),
// or forced once old enough.
constexpr float kCalmSpeed = 0.04f;
constexpr float kCalmSpin = 0.25f;
constexpr int kCalmTicks = 30;
constexpr int kTiredTicks = 500; // 8 s: sleep once merely slow
constexpr float kTiredSpeed = 0.3f;
constexpr int kExhaustedTicks = 940; // 15 s: sleep regardless
// The kick, per stature per second.
constexpr float kBulletKick = 1.2f;
constexpr float kBulletPelvisShare = 0.25f;
constexpr float kBlastKick = 2.5f;
constexpr float kBlastLift = 1.8f;
constexpr float kBlastTumble = 4.0f;
constexpr float kCollapseJitter = 1.0f;

constexpr Vec3 kUpWorld{ 0.0f, 1.0f, 0.0f };

bool finite(const Vec3 &v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 clamp_length(const Vec3 &v, float max_len) {
	const float l = length(v);
	return l > max_len && l > 0.0f ? v * (max_len / l) : v;
}

// The model-frame push of a hit from `quadrant` (0 front .. 3 left): a shot
// from the front drives the body back, from the right drives it left. The
// model faces +Z and its left is +X (ragdoll_rig.cpp's frame).
Vec3 push_direction(int quadrant) {
	switch (quadrant & 3) {
		case 0:
			return { 0.0f, 0.0f, -1.0f };
		case 1:
			return { 1.0f, 0.0f, 0.0f };
		case 2:
			return { 0.0f, 0.0f, 1.0f };
		default:
			return { -1.0f, 0.0f, 0.0f };
	}
}

// A deterministic value in [-1, 1] from a seed and a lane.
float jitter(uint32_t seed, uint32_t lane) {
	uint32_t h = seed * 0x9E3779B1u + lane * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return static_cast<float>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

} // namespace

Vec3 Ragdoll::world_offset(int body, const Vec3 &local) const {
	return quat_rotate(bodies_[body].q, local * scale_);
}

Vec3 Ragdoll::body_pivot(int body) const {
	return bodies_[body].x + world_offset(body, rig_->bodies[body].anchor_child);
}

float Ragdoll::joint_gap(int body) const {
	const BodyDef &def = rig_->bodies[body];
	if (def.parent < 0)
		return 0.0f;
	const Vec3 held = bodies_[def.parent].x + world_offset(def.parent, def.anchor_parent);
	return length(body_pivot(body) - held);
}

bool Ragdoll::start(
		std::shared_ptr<const Rig> rig, const StartInput &in, const Collider &collider) {
	rig_.reset();
	bodies_.clear();
	if (rig == nullptr || !rig->valid() || in.pose == nullptr ||
			in.pose->size() < rig->parents.size() || !(in.node.scale > 0.0f)) {
		return false;
	}
	start_pose_.assign(in.pose->begin(), in.pose->begin() + static_cast<long>(rig->parents.size()));
	pose_globals(rig->parents, start_pose_, start_rotation_, start_pivot_);
	std::vector<Quat> prev_rotation;
	std::vector<Vec3> prev_pivot;
	const bool have_prev = in.pose_prev != nullptr && in.pose_prev->size() >= rig->parents.size();
	if (have_prev)
		pose_globals(rig->parents, *in.pose_prev, prev_rotation, prev_pivot);

	rig_ = std::move(rig);
	scale_ = in.node.scale;
	asleep_ = false;
	failed_ = false;
	calm_ = 0;
	age_ = 0;
	const float stature = rig_->height * scale_;
	const float dt = 1.0f / 62.5f;
	bodies_.resize(rig_->bodies.size());
	for (size_t b = 0; b < bodies_.size(); ++b) {
		const BodyDef &def = rig_->bodies[b];
		Body &body = bodies_[b];
		const int bone = def.bone;
		body.q = quat_normalized(quat_mul(in.node.rotation, start_rotation_[bone]));
		body.x = xform_point(
				in.node, start_pivot_[bone] + quat_rotate(start_rotation_[bone], def.com));
		body.v = Vec3{};
		body.w = Vec3{};
		if (have_prev) {
			const Quat q0 = quat_normalized(quat_mul(in.node_prev.rotation, prev_rotation[bone]));
			const Vec3 x0 = xform_point(
					in.node_prev, prev_pivot[bone] + quat_rotate(prev_rotation[bone], def.com));
			body.v = clamp_length((body.x - x0) * (1.0f / dt), kMaxStartSpeed * stature);
			body.w = clamp_length(
					quat_log(quat_mul(body.q, quat_conj(q0))) * (1.0f / dt), kMaxStartSpin);
		}
		body.x_prev = body.x;
		body.q_prev = body.q;
	}

	// The kick, in the model's frame turned into the world.
	const Vec3 push = quat_rotate(in.node.rotation, push_direction(in.kick.quadrant));
	switch (in.kick.kind) {
		case Kick::Kind::Bullet: {
			int struck = (in.kick.bone >= 0 &&
								 static_cast<size_t>(in.kick.bone) < rig_->bone_body.size())
					? rig_->bone_body[in.kick.bone]
					: 0;
			if (struck < 0)
				struck = 0;
			bodies_[struck].v += push * (kBulletKick * stature);
			bodies_[0].v += push * (kBulletKick * kBulletPelvisShare * stature);
			break;
		}
		case Kick::Kind::Blast: {
			for (size_t b = 0; b < bodies_.size(); ++b) {
				const float vary = 1.0f + 0.2f * jitter(in.kick.seed, static_cast<uint32_t>(b));
				bodies_[b].v += (push * kBlastKick + kUpWorld * kBlastLift) * (vary * stature);
			}
			bodies_[0].w +=
					normalized(cross(kUpWorld, push), Vec3{ 1.0f, 0.0f, 0.0f }) * kBlastTumble;
			break;
		}
		case Kick::Kind::Collapse: {
			const Vec3 axis =
					normalized(Vec3{ jitter(in.kick.seed, 1), 0.0f, jitter(in.kick.seed, 2) },
							Vec3{ 1.0f, 0.0f, 0.0f });
			bodies_[0].w += axis * kCollapseJitter;
			break;
		}
	}

	float ground = 0.0f;
	Vec3 normal;
	terrain_ = !(collider.ground(in.anchor.x, in.anchor.z, ground, normal) &&
			in.anchor.y < ground - kIndoorsDepth * stature);
	last_anchor_ = in.anchor;
	sleep_anchor_ = in.anchor;
	return true;
}

void Ragdoll::sleep() {
	asleep_ = true;
	sleep_anchor_ = last_anchor_;
	for (Body &body : bodies_) {
		body.v = Vec3{};
		body.w = Vec3{};
	}
}

void Ragdoll::apply_point(int body, const Vec3 &r, const Vec3 &impulse) {
	Body &b = bodies_[body];
	const BodyDef &def = rig_->bodies[body];
	const float inv_inertia = def.inv_inertia / (scale_ * scale_);
	b.x += impulse * def.inv_mass;
	b.q = quat_integrate(b.q, cross(r, impulse) * inv_inertia);
}

// Move the world points (a's at offset ra, b's at offset rb) together by
// `delta` (b's point minus a's), split by their generalized inverse masses.
// `a` may be -1 (the world, immovable).
void Ragdoll::positional(int a, const Vec3 &ra, int b, const Vec3 &rb, const Vec3 &delta) {
	const float c = length(delta);
	if (c < 1e-7f)
		return;
	const Vec3 n = delta * (1.0f / c);
	const float s2 = scale_ * scale_;
	float wa = 0.0f;
	if (a >= 0) {
		const BodyDef &da = rig_->bodies[a];
		const Vec3 k = cross(ra, n);
		wa = da.inv_mass + dot(k, k) * da.inv_inertia / s2;
	}
	const BodyDef &db = rig_->bodies[b];
	const Vec3 kb = cross(rb, n);
	const float wb = db.inv_mass + dot(kb, kb) * db.inv_inertia / s2;
	const float lambda = c / (wa + wb);
	const Vec3 p = n * lambda;
	if (a >= 0)
		apply_point(a, ra, p);
	apply_point(b, rb, -p);
}

// Turn body b by `angle` about `unit_axis` relative to body a (a turns back
// by its share).
void Ragdoll::angular(int a, int b, const Vec3 &unit_axis, float angle) {
	const float s2 = scale_ * scale_;
	const float wa = rig_->bodies[a].inv_inertia / s2;
	const float wb = rig_->bodies[b].inv_inertia / s2;
	const float lambda = angle / (wa + wb);
	bodies_[a].q = quat_integrate(bodies_[a].q, unit_axis * (-lambda * wa));
	bodies_[b].q = quat_integrate(bodies_[b].q, unit_axis * (lambda * wb));
}

void Ragdoll::solve_joint(int child) {
	const BodyDef &def = rig_->bodies[child];
	const int parent = def.parent;
	if (parent < 0)
		return;

	// The pivots meet.
	{
		const Vec3 ra = world_offset(parent, def.anchor_parent);
		const Vec3 rb = world_offset(child, def.anchor_child);
		positional(parent, ra, child, rb, (bodies_[child].x + rb) - (bodies_[parent].x + ra));
	}

	const Quat &qp = bodies_[parent].q;
	if (def.joint == JointKind::Hinge) {
		// The two bodies' hinge axes stay one.
		const Vec3 hinge_parent = quat_rotate(qp, quat_rotate(def.rest_rel, def.hinge));
		const Vec3 hinge_child = quat_rotate(bodies_[child].q, def.hinge);
		const Vec3 align = cross(hinge_child, hinge_parent);
		const float s = length(align);
		if (s > 1e-6f)
			angular(parent, child, align * (1.0f / s), std::asin(std::fmin(s, 1.0f)));
		// The bend stays in its range.
		const Vec3 axis = quat_rotate(bodies_[parent].q, quat_rotate(def.rest_rel, def.hinge));
		const Vec3 rest_dir = quat_rotate(bodies_[parent].q, quat_rotate(def.rest_rel, def.axis));
		const Vec3 dir = quat_rotate(bodies_[child].q, def.axis);
		const float bend = std::atan2(dot(cross(rest_dir, dir), axis), dot(rest_dir, dir));
		const float clamped = std::fmin(std::fmax(bend, def.twist_min), def.twist_max);
		if (bend != clamped)
			angular(parent, child, axis, clamped - bend);
		return;
	}

	// Ball: the limb stays inside its cone...
	{
		const Quat rel = quat_mul(quat_conj(bodies_[parent].q), bodies_[child].q);
		const Vec3 dir = quat_rotate(rel, def.axis);
		const float cosine = std::fmin(std::fmax(dot(dir, def.cone), -1.0f), 1.0f);
		const float swing = std::acos(cosine);
		if (swing > def.swing_max) {
			const Vec3 k = cross(dir, def.cone);
			const float kl = length(k);
			const Vec3 axis_local = kl > 1e-6f ? k * (1.0f / kl) : quat_rotate(rel, def.hinge);
			const Vec3 axis = quat_rotate(
					bodies_[parent].q, normalized(axis_local, Vec3{ 1.0f, 0.0f, 0.0f }));
			angular(parent, child, axis, swing - def.swing_max);
		}
	}
	// ...and its twist about itself in range.
	{
		const Quat rel = quat_mul(quat_conj(bodies_[parent].q), bodies_[child].q);
		Quat d = quat_mul(quat_conj(def.rest_rel), rel);
		if (d.w < 0.0f)
			d = Quat{ -d.w, -d.x, -d.y, -d.z };
		const float along = d.x * def.axis.x + d.y * def.axis.y + d.z * def.axis.z;
		const float twist = 2.0f * std::atan2(along, d.w);
		const float clamped = std::fmin(std::fmax(twist, def.twist_min), def.twist_max);
		if (twist != clamped) {
			const Vec3 axis = quat_rotate(bodies_[child].q, def.axis);
			angular(parent, child, axis, clamped - twist);
		}
	}
}

// One sphere against one plane: pushed out along the normal, its contact
// point held against sliding up to the friction cone.
void Ragdoll::contact(int body, const Vec3 &center, const Vec3 &center_prev, float radius,
		const Vec3 &point, const Vec3 &normal) {
	const float depth = radius - dot(center - point, normal);
	if (depth <= 0.0f)
		return;
	Body &b = bodies_[body];
	b.touching = true;
	const Vec3 r = (center - normal * radius) - b.x;
	positional(-1, Vec3{}, body, r, normal * -depth);
	// Static friction: undo the contact point's slide this substep, as far
	// as the push-out allows.
	const Vec3 slide = (center - center_prev) - normal * dot(center - center_prev, normal);
	const float slide_len = length(slide);
	if (slide_len > 1e-7f) {
		const float take = std::fmin(slide_len, kFriction * depth);
		positional(-1, Vec3{}, body, r, slide * (take / slide_len));
	}
}

void Ragdoll::solve_contacts(int body, const Collider &collider) {
	const BodyDef &def = rig_->bodies[body];
	const Body &b = bodies_[body];
	const float radius = def.radius * scale_;
	const Vec3 ends[3] = { def.seg_a, def.seg_b, (def.seg_a + def.seg_b) * 0.5f };
	const int count = length(def.seg_b - def.seg_a) * scale_ > 2.0f * radius ? 3 : 2;
	for (int i = 0; i < count; ++i) {
		const Vec3 center = bodies_[body].x + world_offset(body, ends[i]);
		const Vec3 center_prev = b.x_prev + quat_rotate(b.q_prev, ends[i] * scale_);
		float height = 0.0f;
		Vec3 normal;
		if (terrain_ && collider.ground(center.x, center.z, height, normal)) {
			contact(body, center, center_prev, radius, Vec3{ center.x, height, center.z }, normal);
		}
		if (b.has_plane) {
			const Vec3 moved = bodies_[body].x + world_offset(body, ends[i]);
			contact(body, moved, center_prev, radius, b.plane_point, b.plane_normal);
		}
	}
}

void Ragdoll::substep(const Collider &collider, const Vec3 &anchor, float h) {
	const float stature = rig_->height * scale_;
	for (Body &b : bodies_) {
		b.v.y -= kGravity * h;
		b.x_prev = b.x;
		b.q_prev = b.q;
		b.x += b.v * h;
		b.q = quat_integrate(b.q, b.w * h);
		b.touching = false;
	}
	for (size_t b = 1; b < bodies_.size(); ++b)
		solve_joint(static_cast<int>(b));
	for (size_t b = 0; b < bodies_.size(); ++b)
		solve_contacts(static_cast<int>(b), collider);
	// The joints again, so a contact's push never leaves a limb past its range.
	for (size_t b = 1; b < bodies_.size(); ++b)
		solve_joint(static_cast<int>(b));
	// The tether: the hips stay within reach of the simulation's corpse.
	{
		const Vec3 r = world_offset(0, rig_->bodies[0].anchor_child);
		const Vec3 off = (bodies_[0].x + r) - anchor;
		const float d = length(off);
		const float slack = kTetherSlack * stature;
		if (d > slack) {
			const float share = std::fmin(1.0f, kTetherStiffness * h);
			positional(-1, Vec3{}, 0, r, off * ((d - slack) * share / d));
		}
	}
	const float inv_h = 1.0f / h;
	for (Body &b : bodies_) {
		b.v = (b.x - b.x_prev) * inv_h;
		Quat dq = quat_mul(b.q, quat_conj(b.q_prev));
		if (dq.w < 0.0f)
			dq = Quat{ -dq.w, -dq.x, -dq.y, -dq.z };
		b.w = Vec3{ dq.x, dq.y, dq.z } * (2.0f * inv_h);
		b.v = b.v * std::fmax(0.0f, 1.0f - kLinearDamping * h);
		b.w = b.w * std::fmax(0.0f, 1.0f - kAngularDamping * h);
		if (b.touching)
			b.w = b.w * std::fmax(0.0f, 1.0f - kRollingDamping * h);
	}
	const float joint_share = std::fmin(1.0f, kJointDamping * h);
	for (size_t c = 1; c < bodies_.size(); ++c) {
		const BodyDef &def = rig_->bodies[c];
		Body &child = bodies_[c];
		Body &parent = bodies_[def.parent];
		const Vec3 rel = (child.w - parent.w) * joint_share;
		const float wp = rig_->bodies[def.parent].inv_inertia;
		const float wc = def.inv_inertia;
		child.w -= rel * (wc / (wp + wc));
		parent.w += rel * (wp / (wp + wc));
	}
}

void Ragdoll::step(const Collider &collider, const Vec3 &anchor, float dt) {
	if (rig_ == nullptr || failed_)
		return;
	const float stature = rig_->height * scale_;
	const Vec3 jump = anchor - last_anchor_;
	last_anchor_ = anchor;
	if (length(jump) > kSnapDistance * stature) {
		for (Body &b : bodies_) {
			b.x += jump;
			b.x_prev += jump;
		}
	}
	if (asleep_) {
		if (length(anchor - sleep_anchor_) <= kWakeDrift * stature)
			return;
		asleep_ = false;
		calm_ = 0;
	}
	++age_;

	// Each body's static geometry this tick: the first solid along its
	// motion (gravity-biased, so a body at rest still finds its floor),
	// reached out past its own extent.
	for (size_t i = 0; i < bodies_.size(); ++i) {
		Body &b = bodies_[i];
		const BodyDef &def = rig_->bodies[i];
		const Vec3 motion = b.v * dt + Vec3{ 0.0f, -kGravity * dt * dt, 0.0f };
		const float reach = (std::fmax(length(def.seg_a), length(def.seg_b)) + def.radius) * scale_;
		const Vec3 dir = normalized(motion, Vec3{ 0.0f, -1.0f, 0.0f });
		b.has_plane = collider.sweep(
				b.x, b.x + dir * (length(motion) + reach), b.plane_point, b.plane_normal);
	}

	const float h = dt / static_cast<float>(kSubsteps);
	for (int s = 0; s < kSubsteps; ++s)
		substep(collider, anchor, h);

	float fastest = 0.0f;
	float spin = 0.0f;
	for (Body &b : bodies_) {
		b.v = clamp_length(b.v, kMaxSpeed * stature);
		b.w = clamp_length(b.w, kMaxSpin);
		if (!finite(b.x) || !finite(b.v) || !finite(b.w) || !std::isfinite(b.q.w)) {
			failed_ = true;
			return;
		}
		fastest = std::fmax(fastest, length(b.v));
		spin = std::fmax(spin, length(b.w));
	}
	calm_ = (fastest < kCalmSpeed * stature && spin < kCalmSpin) ? calm_ + 1 : 0;
	if (calm_ >= kCalmTicks || (age_ >= kTiredTicks && fastest < kTiredSpeed * stature) ||
			age_ >= kExhaustedTicks) {
		sleep();
	}
}

void Ragdoll::pose(const Xform &node, std::vector<anim::PoseBone> &r_pose) const {
	r_pose = start_pose_;
	if (rig_ == nullptr)
		return;
	const size_t n = rig_->parents.size();
	const Quat node_inv = quat_conj(node.rotation);
	std::vector<Quat> rot(n);
	std::vector<Vec3> pivot(n);
	for (size_t i = 0; i < n; ++i) {
		const int p = rig_->parents[i];
		const int b = rig_->bone_body[i];
		const bool simulated = b >= 0 && rig_->bodies[b].bone == static_cast<int>(i);
		if (simulated) {
			rot[i] = quat_normalized(quat_mul(node_inv, bodies_[b].q));
		} else if (p >= 0) {
			rot[i] = quat_normalized(quat_mul(rot[p], start_pose_[i].rotation));
		} else {
			rot[i] = start_rotation_[i];
		}
		if (p >= 0) {
			pivot[i] = pivot[p] + quat_rotate(rot[p], start_pose_[i].origin);
			r_pose[i].rotation = quat_normalized(quat_mul(quat_conj(rot[p]), rot[i]));
		} else {
			pivot[i] = simulated ? xform_point_inverse(node, body_pivot(b)) : start_pivot_[i];
			r_pose[i].rotation = rot[i];
			r_pose[i].origin = pivot[i];
		}
	}
}

float Ragdoll::lowest_clearance(const Collider &collider) const {
	float lowest = 1e30f;
	if (rig_ == nullptr)
		return lowest;
	for (size_t i = 0; i < bodies_.size(); ++i) {
		const BodyDef &def = rig_->bodies[i];
		for (const Vec3 &end : { def.seg_a, def.seg_b }) {
			const Vec3 c = bodies_[i].x + world_offset(static_cast<int>(i), end);
			float height = 0.0f;
			Vec3 normal;
			if (!collider.ground(c.x, c.z, height, normal))
				continue;
			lowest = std::fmin(lowest, c.y - def.radius * scale_ - height);
		}
	}
	return lowest;
}

} // namespace opennova::ragdoll
