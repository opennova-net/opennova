#pragma once

// One ragdoll: a rig's capsule bodies under gravity, held together by their
// joints, kept off the ground and static geometry, and tethered to the
// simulation's corpse (ADR 0060). Rigid bodies stepped by extended
// position-based dynamics (Mueller et al., "Detailed Rigid Body Simulation
// with Extended Position Based Dynamics", 2020) at the logic tick with fixed
// substeps, in floats: it is presentation, and nothing it computes reaches
// the simulation.

#include <runtime/ragdoll/ragdoll_collider.h>
#include <runtime/ragdoll/ragdoll_rig.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace opennova::ragdoll {

// The push a death gives the body, decoded from its death clip.
struct Kick {
	enum class Kind : uint8_t { Collapse, Bullet, Blast };
	Kind kind = Kind::Collapse;
	// Where the hit came from, around the victim: 0 front, 1 right, 2 back, 3 left.
	int quadrant = 0;
	int bone = 0; // the struck bone (Bullet)
	uint32_t seed = 0; // the deterministic variation
};

struct StartInput {
	Xform node; // the model's placement now
	Xform node_prev; // and one tick before
	const std::vector<anim::PoseBone> *pose = nullptr; // the drawn pose now (parent-local)
	const std::vector<anim::PoseBone> *pose_prev = nullptr; // one tick before (null: at rest)
	Kick kick;
	Vec3 anchor; // the simulation corpse's hips pivot, world
};

class Ragdoll {
public:
	// Seat the bodies on the start pose with its motion and the kick; false
	// when the rig has no plan or the pose does not fit it.
	bool start(std::shared_ptr<const Rig> rig, const StartInput &in, const Collider &collider);
	// One logic tick of `dt` seconds, the hips held within reach of `anchor`.
	void step(const Collider &collider, const Vec3 &anchor, float dt);
	// The drawn pose (parent-local, model space) under `node`.
	void pose(const Xform &node, std::vector<anim::PoseBone> &r_pose) const;

	bool started() const { return rig_ != nullptr; }
	bool asleep() const { return asleep_; }
	// Put to sleep at the anchor it last followed.
	void sleep();
	// Whether a sleeper's anchor has drifted past the distance that wakes it.
	// The embedder decides whether it does (RagdollSet keeps its awake budget):
	// step() never wakes a sleeper itself.
	bool drifted(const Vec3 &anchor) const;
	void wake();
	bool failed() const { return failed_; }
	int age_ticks() const { return age_; }
	// The world pivot of a simulated bone's body (tests and probes).
	Vec3 body_pivot(int body) const;
	Vec3 body_velocity(int body) const { return bodies_[body].v; }
	Vec3 body_spin(int body) const { return bodies_[body].w; }
	// How far a body's pivot has drifted from its parent's hold on it (tests).
	float joint_gap(int body) const;
	int body_count() const { return static_cast<int>(bodies_.size()); }
	const Rig *rig() const { return rig_.get(); }
	// The lowest point of any body's capsule above the ground (probes).
	float lowest_clearance(const Collider &collider) const;

private:
	struct Body {
		Vec3 x, v, x_prev;
		Quat q, q_prev;
		Vec3 w;
		bool touching = false; // in contact this substep
		bool has_plane = false;
		Vec3 plane_point, plane_normal;
	};

	Vec3 world_offset(int body, const Vec3 &local) const;
	void apply_point(int body, const Vec3 &r, const Vec3 &impulse);
	void positional(int a, const Vec3 &ra, int b, const Vec3 &rb, const Vec3 &delta);
	void angular(int a, int b, const Vec3 &unit_axis, float angle);
	void solve_joint(int body);
	void contact(int body, const Vec3 &center, const Vec3 &center_prev, float radius,
			const Vec3 &point, const Vec3 &normal);
	void solve_contacts(int body, const Collider &collider);
	void substep(const Collider &collider, const Vec3 &anchor, float h);

	std::shared_ptr<const Rig> rig_;
	std::vector<Body> bodies_;
	std::vector<anim::PoseBone> start_pose_;
	std::vector<Quat> start_rotation_;
	std::vector<Vec3> start_pivot_;
	float scale_ = 1.0f;
	bool terrain_ = true;
	bool asleep_ = false;
	bool failed_ = false;
	int calm_ = 0;
	int age_ = 0;
	Vec3 last_anchor_;
	Vec3 sleep_anchor_;
};

} // namespace opennova::ragdoll
