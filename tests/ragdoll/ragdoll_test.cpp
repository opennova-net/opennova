// The opt-in ragdoll corpses (ADR 0060) on a synthetic JO person rig: the
// NoBlood switch, the body plan, the solver (settling, joint limits, ground and
// wall contact, the tether, the kick's direction, determinism), the set's
// lifecycle, and the death clip reading every machine decodes the same way.
#include <runtime/ragdoll/ragdoll_option.h>
#include <runtime/ragdoll/ragdoll_rig.h>
#include <runtime/ragdoll/ragdoll_set.h>
#include <runtime/ragdoll/ragdoll_solver.h>
#include <runtime/world/corpse_ragdoll.h>
#include <runtime/world/infantry.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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

constexpr float kTick = 1.0f / 62.5f;
constexpr float kPi = 3.14159265358979323846f;

// A standing soldier, model frame (Y up, facing +Z, its left +X), arms down:
// the 19 BN bones plus the meshless root row a JO person model carries.
struct BoneRow {
	int parent;
	Vec3 pivot;
};
const BoneRow kRows[20] = {
	{ -1, { 0.0f, 1.0f, 0.0f } }, // 0 hips
	{ 0, { 0.0f, 1.1f, 0.0f } }, // 1 lower spine
	{ 1, { 0.0f, 1.3f, 0.0f } }, // 2 upper spine
	{ 2, { -0.05f, 1.5f, 0.0f } }, // 3 R clavicle
	{ 2, { 0.05f, 1.5f, 0.0f } }, // 4 L clavicle
	{ 3, { -0.2f, 1.5f, 0.0f } }, // 5 R upper arm
	{ 4, { 0.2f, 1.5f, 0.0f } }, // 6 L upper arm
	{ 0, { -0.1f, 0.95f, 0.0f } }, // 7 R thigh
	{ 0, { 0.1f, 0.95f, 0.0f } }, // 8 L thigh
	{ 5, { -0.2f, 1.2f, 0.0f } }, // 9 R forearm
	{ 6, { 0.2f, 1.2f, 0.0f } }, // 10 L forearm
	{ 7, { -0.1f, 0.5f, 0.0f } }, // 11 R calf
	{ 8, { 0.1f, 0.5f, 0.0f } }, // 12 L calf
	{ 2, { 0.0f, 1.55f, 0.0f } }, // 13 neck
	{ 13, { 0.0f, 1.65f, 0.0f } }, // 14 head
	{ 10, { 0.2f, 0.95f, 0.0f } }, // 15 L hand
	{ 9, { -0.2f, 0.95f, 0.0f } }, // 16 R hand
	{ 11, { -0.1f, 0.08f, 0.0f } }, // 17 R foot
	{ 12, { 0.1f, 0.08f, 0.0f } }, // 18 L foot
	{ -1, { 0.0f, 0.0f, 0.0f } }, // 19 the meshless root row
};

RigSource make_source() {
	RigSource s;
	for (int i = 0; i < 20; ++i) {
		s.parents.push_back(kRows[i].parent);
		s.bn.push_back(i < 19 ? i : -1);
		anim::PoseBone b;
		b.origin = kRows[i].parent >= 0 ? kRows[i].pivot - kRows[kRows[i].parent].pivot
										: kRows[i].pivot;
		s.rest.push_back(b);
	}
	return s;
}

// Flat ground at `level`, optionally tilted to `slope_deg` about Z, and an
// optional wall plane z = wall_z facing +Z.
struct StubCollider final : Collider {
	float level = 0.0f;
	float slope = 0.0f; // tangent
	bool wall = false;
	float wall_z = 0.0f;
	bool ground(float x, float, float &h, Vec3 &n) const override {
		h = level + slope * x;
		n = normalized(Vec3{ -slope, 1.0f, 0.0f });
		return true;
	}
	bool sweep(const Vec3 &a, const Vec3 &b, Vec3 &p, Vec3 &n) const override {
		if (!wall || !(a.z > wall_z && b.z <= wall_z))
			return false;
		const float t = (a.z - wall_z) / (a.z - b.z);
		p = a + (b - a) * t;
		n = Vec3{ 0.0f, 0.0f, 1.0f };
		return true;
	}
};

Xform node_at(const Vec3 &origin, float yaw_deg) {
	Xform x;
	x.rotation = quat_axis_angle(Vec3{ 0.0f, 1.0f, 0.0f }, yaw_deg * kPi / 180.0f);
	x.origin = origin;
	return x;
}

std::shared_ptr<const Rig> make_rig() {
	return std::make_shared<const Rig>(build_rig(make_source()));
}

bool start_on(Ragdoll &r, const std::shared_ptr<const Rig> &rig, const Collider &c,
		const Xform &node, const Kick &kick, const Vec3 &anchor) {
	static std::vector<anim::PoseBone> pose;
	pose = make_source().rest;
	StartInput in;
	in.node = node;
	in.node_prev = node;
	in.pose = &pose;
	in.kick = kick;
	in.anchor = anchor;
	return r.start(rig, in, c);
}

// The hips a death clip carries down: standing at 1.0, lying at 0.15 after 40 ticks.
Vec3 clip_hips(int tick) {
	const float t = tick >= 40 ? 1.0f : static_cast<float>(tick) / 40.0f;
	return Vec3{ 0.0f, 1.0f - 0.85f * t, 0.0f };
}

float max_joint_gap(const Ragdoll &r) {
	float worst = 0.0f;
	for (int b = 1; b < r.body_count(); ++b)
		worst = std::fmax(worst, r.joint_gap(b));
	return worst;
}

void test_option() {
	expect(!ragdolls_enabled(0), "NoBlood 0: no ragdolls");
	expect(!ragdolls_enabled(1), "NoBlood 1 (retail's bit): no ragdolls");
	expect(ragdolls_enabled(2), "NoBlood 2: ragdolls");
	expect(ragdolls_enabled(3), "NoBlood 3: ragdolls and retail's bit");
	expect(ragdolls_enabled(-1), "NoBlood -1: every bit, ragdolls");
	expect(ragdolls_enabled(6), "NoBlood 6: bit 1 set");
	expect(!ragdolls_enabled(4), "NoBlood 4: bit 1 clear");
}

void test_rig() {
	const Rig rig = build_rig(make_source());
	if (!expect(rig.valid(), "the JO person rig gets a plan"))
		return;
	expect(rig.bodies.size() == 12, "twelve bodies");
	expect(rig.bodies[0].bone == bone::kHips && rig.bodies[0].parent == -1,
			"the pelvis is the root");
	expect(std::fabs(rig.height - 1.57f) < 1e-4f, "the stature runs head pivot to feet");
	const int chest = rig.bone_body[bone::kUpperSpine];
	expect(rig.bone_body[bone::kRClavicle] == chest && rig.bone_body[bone::kNeck] == chest,
			"clavicles and neck weld to the chest");
	expect(rig.bone_body[bone::kLHand] == rig.bone_body[bone::kLForearm],
			"a hand welds to its forearm");
	expect(rig.bone_body[bone::kRFoot] == rig.bone_body[bone::kRCalf], "a foot welds to its calf");
	expect(rig.bone_body[19] == -1, "the meshless root row rides no body");
	const int knee = rig.bone_body[bone::kRCalf];
	expect(rig.bodies[knee].joint == JointKind::Hinge, "the knee is a hinge");
	expect(rig.bodies[knee].parent == rig.bone_body[bone::kRThigh], "the calf hangs off the thigh");
	// The knee's hinge bends the calf backward (-Z): its bend direction is
	// cross(hinge, limb) at rest.
	const Vec3 bend = cross(rig.bodies[knee].hinge, rig.bodies[knee].axis);
	expect(bend.z < -0.9f, "the knee bends back");
	const int elbow = rig.bone_body[bone::kLForearm];
	expect(cross(rig.bodies[elbow].hinge, rig.bodies[elbow].axis).z > 0.9f,
			"the elbow bends forward");

	RigSource short_rig = make_source();
	short_rig.parents.resize(18);
	expect(!build_rig(short_rig).valid(), "fewer than 19 bones: no plan");
	RigSource untagged = make_source();
	untagged.bn[5] = -1;
	expect(!build_rig(untagged).valid(), "a bone out of BN order: no plan");
	RigSource forward = make_source();
	forward.parents[3] = 7;
	expect(!build_rig(forward).valid(), "a parent after its child: no plan");
}

void test_pose_round_trip() {
	const auto rig = make_rig();
	StubCollider ground;
	ground.level = -10.0f;
	Ragdoll r;
	const Xform node = node_at(Vec3{ 5.0f, 2.0f, -3.0f }, 30.0f);
	if (!expect(start_on(r, rig, ground, node, Kick{}, Vec3{ 5.0f, 3.0f, -3.0f }),
				"a ragdoll starts"))
		return;
	std::vector<anim::PoseBone> pose;
	r.pose(node, pose);
	const std::vector<anim::PoseBone> rest = make_source().rest;
	float worst = 0.0f;
	for (size_t i = 0; i < rest.size(); ++i) {
		worst = std::fmax(worst, length(pose[i].origin - rest[i].origin));
		const Quat d = quat_mul(quat_conj(rest[i].rotation), pose[i].rotation);
		worst = std::fmax(worst, length(quat_log(d)));
	}
	expect(worst < 1e-4f, "an unstepped ragdoll draws its start pose");
}

// Hinge bend of a body against its parent, as the solver measures it.
float hinge_bend(const Ragdoll &r, int body) {
	const Rig &rig = *r.rig();
	const BodyDef &def = rig.bodies[body];
	std::vector<anim::PoseBone> pose;
	r.pose(Xform{}, pose);
	std::vector<Quat> rot;
	std::vector<Vec3> pivot;
	pose_globals(rig.parents, pose, rot, pivot);
	const Quat qp = rot[rig.bodies[def.parent].bone];
	const Quat qc = rot[def.bone];
	const Vec3 axis = quat_rotate(qp, quat_rotate(def.rest_rel, def.hinge));
	const Vec3 rest_dir = quat_rotate(qp, quat_rotate(def.rest_rel, def.axis));
	const Vec3 dir = quat_rotate(qc, def.axis);
	return std::atan2(dot(cross(rest_dir, dir), axis), dot(rest_dir, dir));
}

void test_drop_settles() {
	const auto rig = make_rig();
	StubCollider ground;
	Ragdoll r;
	Kick kick;
	kick.kind = Kick::Kind::Collapse;
	kick.seed = 7;
	if (!expect(start_on(r, rig, ground, Xform{}, kick, clip_hips(0)), "the drop starts"))
		return;
	int ticks = 0;
	float worst_clearance = 0.0f;
	float worst_bend_over = 0.0f;
	while (!r.asleep() && ticks < 1000 && !r.failed()) {
		r.step(ground, clip_hips(ticks + 1), kTick);
		++ticks;
		worst_clearance = std::fmin(worst_clearance, r.lowest_clearance(ground));
		for (int b = 0; b < r.body_count(); ++b) {
			const BodyDef &def = rig->bodies[b];
			if (def.joint != JointKind::Hinge || def.parent < 0)
				continue;
			const float bend = hinge_bend(r, b);
			worst_bend_over = std::fmax(
					worst_bend_over, std::fmax(def.twist_min - bend, bend - def.twist_max));
		}
	}
	std::printf("drop: slept after %d ticks, deepest %.4f, worst bend overshoot %.2f deg\n", ticks,
			worst_clearance, worst_bend_over * 180.0f / kPi);
	expect(!r.failed(), "the drop stays finite");
	expect(r.asleep(), "the drop comes to rest");
	expect(ticks < 500, "the drop rests within 8 s");
	expect(worst_clearance > -0.03f, "no body sinks into the ground");
	expect(worst_bend_over < 8.0f * kPi / 180.0f, "knees and elbows stay in their range");
	expect(r.body_pivot(0).y < 0.45f, "the body lies down");
	expect(max_joint_gap(r) < 0.01f * rig->height, "the joints hold together");
}

void test_slope() {
	const auto rig = make_rig();
	StubCollider ground;
	ground.slope = std::tan(30.0f * kPi / 180.0f);
	Ragdoll r;
	Kick kick;
	kick.kind = Kick::Kind::Bullet;
	kick.bone = bone::kHead;
	if (!expect(start_on(r, rig, ground, Xform{}, kick, clip_hips(0)), "the slope drop starts"))
		return;
	float worst = 0.0f;
	for (int t = 0; t < 500 && !r.asleep(); ++t) {
		r.step(ground, clip_hips(t + 1), kTick);
		worst = std::fmin(worst, r.lowest_clearance(ground));
	}
	expect(!r.failed(), "a 30 degree slope stays finite");
	expect(worst > -0.03f, "nothing sinks into a 30 degree slope");
}

// The quadrant pushes the struck chest the way the shot travelled.
void test_kick_direction() {
	const auto rig = make_rig();
	StubCollider ground;
	const float yaw = 90.0f;
	const Xform node = node_at(Vec3{}, yaw);
	// Model frame pushes for a shot from the front, right, back, left.
	const Vec3 model_push[4] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
	for (int q = 0; q < 4; ++q) {
		Ragdoll r;
		Kick kick;
		kick.kind = Kick::Kind::Bullet;
		kick.quadrant = q;
		kick.bone = bone::kUpperSpine;
		const Vec3 anchor{ 0.0f, 1.0f, 0.0f };
		if (!start_on(r, rig, ground, node, kick, anchor))
			continue;
		const int chest = rig->bone_body[bone::kUpperSpine];
		const Vec3 before = r.body_pivot(chest);
		for (int t = 0; t < 12; ++t)
			r.step(ground, anchor, kTick);
		const Vec3 moved = r.body_pivot(chest) - before;
		const Vec3 want = quat_rotate(node.rotation, model_push[q]);
		char msg[96];
		std::snprintf(msg, sizeof(msg), "quadrant %d pushes the chest the shot's way", q);
		expect(dot(Vec3{ moved.x, 0.0f, moved.z }, want) > 0.01f, msg);
	}
}

void test_determinism() {
	const auto rig = make_rig();
	StubCollider ground;
	Kick kick;
	kick.kind = Kick::Kind::Blast;
	kick.quadrant = 1;
	kick.seed = 99;
	const Vec3 anchor{ 0.0f, 0.5f, 0.0f };
	Ragdoll a, b;
	start_on(a, rig, ground, Xform{}, kick, anchor);
	start_on(b, rig, ground, Xform{}, kick, anchor);
	for (int t = 0; t < 120; ++t) {
		a.step(ground, anchor, kTick);
		b.step(ground, anchor, kTick);
	}
	bool same = true;
	for (int i = 0; i < a.body_count(); ++i) {
		const Vec3 pa = a.body_pivot(i), pb = b.body_pivot(i);
		same = same && std::memcmp(&pa, &pb, sizeof(Vec3)) == 0;
	}
	expect(same, "the same start steps to the same bits");
}

void test_wall() {
	const auto rig = make_rig();
	StubCollider c;
	c.wall = true;
	c.wall_z = -0.35f;
	Ragdoll r;
	Kick kick;
	kick.kind = Kick::Kind::Blast; // from the front: thrown back toward -Z
	if (!expect(start_on(r, rig, c, Xform{}, kick, clip_hips(0)), "the wall drop starts"))
		return;
	float deepest = 0.0f;
	for (int t = 0; t < 300 && !r.asleep(); ++t) {
		r.step(c, clip_hips(t + 1), kTick);
		for (int b = 0; b < r.body_count(); ++b) {
			deepest = std::fmin(deepest, r.body_pivot(b).z - c.wall_z);
		}
	}
	std::printf("wall: deepest pivot past the wall %.3f\n", deepest);
	expect(deepest > -0.1f, "a flung body stops at the wall");
}

void test_tether() {
	const auto rig = make_rig();
	StubCollider ground;
	Ragdoll r;
	start_on(r, rig, ground, Xform{}, Kick{}, clip_hips(0));
	for (int t = 0; t < 200; ++t)
		r.step(ground, clip_hips(t + 1), kTick);
	const Vec3 anchor = clip_hips(200);
	const float slack = 0.3f * rig->height;
	expect(length(r.body_pivot(0) - anchor) <= slack + 0.02f, "the hips stay within the tether");
	const Vec3 far{ 20.0f, 0.2f, 0.0f };
	r.step(ground, far, kTick);
	expect(length(r.body_pivot(0) - far) <= slack + 0.3f,
			"a corpse that jumps takes its ragdoll along");
}

SubjectHead head_for(uint32_t key, int anim_state, int life = 1) {
	SubjectHead h;
	h.key = key;
	h.life = life;
	const world::CorpseDeath d = world::corpse_death(anim_state, key);
	h.dying = d.dying;
	h.ragdolls = d.ragdolls;
	h.kick = d.kick;
	return h;
}

void test_set() {
	const auto rig = make_rig();
	StubCollider ground;
	const std::vector<anim::PoseBone> pose = make_source().rest;
	const Vec3 anchor{ 0.0f, 0.5f, 0.0f };
	constexpr int kAlive = 1;
	constexpr int kShot = world::anim_state::kDeathBulletBase + 4; // torso, from the front

	RagdollSet set;
	expect(set.observe(head_for(1, kShot)) == Need::None, "a disabled set starts nothing");
	set.set_enabled(true);

	// First seen already dead: keep the clip.
	expect(set.observe(head_for(2, kShot)) == Need::None,
			"a corpse first seen dead keeps its clip");
	set.step(1, ground);
	expect(set.observe(head_for(2, kShot)) == Need::None, "...and keeps it after");

	// Alive, then shot.
	expect(set.observe(head_for(1, kAlive)) == Need::None, "the living draw their clips");
	set.step(1, ground);
	SubjectHead dead = head_for(1, kShot);
	if (!expect(set.observe(dead) == Need::Start, "a death starts a ragdoll"))
		return;
	expect(set.start(dead, rig, pose, nullptr, anchor, ground), "the start takes");
	const RagdollOutput *out = set.output(1);
	expect(out != nullptr && out->weight == 1.0f, "a ragdoll draws over the clip");
	set.step(1, ground);
	expect(set.observe(dead) == Need::Run, "a running ragdoll is fed");
	set.feed(1, Xform{}, anchor);
	set.step(2, ground);

	// Revived: blends back out.
	expect(set.observe(head_for(1, kAlive)) == Need::None, "a revive hands back to the clip");
	set.step(4, ground);
	out = set.output(1);
	expect(out != nullptr && out->weight > 0.0f && out->weight < 1.0f,
			"the clip takes the body back gradually");
	set.observe(head_for(1, kAlive));
	set.step(RagdollSet::kBlendOutTicks, ground);
	expect(set.output(1) == nullptr, "the blend ends on the clip");

	// Fire and drowning keep their clips.
	set.observe(head_for(3, kAlive));
	set.step(1, ground);
	expect(set.observe(head_for(3, world::anim_state::kDeathFire)) == Need::None,
			"fire keeps its clip");
	set.observe(head_for(4, kAlive));
	set.step(1, ground);
	expect(set.observe(head_for(4, world::anim_state::kDeathDrown)) == Need::None,
			"drowning keeps its clip");

	// Mounted the tick before death (detached at the edge): keeps its clip.
	SubjectHead mounted = head_for(5, kAlive);
	mounted.eligible = false;
	set.observe(mounted);
	set.step(1, ground);
	expect(set.observe(head_for(5, kShot)) == Need::None,
			"a body mounted when it died keeps its clip");

	// A new life ends the ragdoll at once; hidden too.
	set.observe(head_for(6, kAlive));
	set.step(1, ground);
	SubjectHead six = head_for(6, kShot);
	set.observe(six);
	set.start(six, rig, pose, nullptr, anchor, ground);
	set.step(1, ground);
	set.observe(head_for(6, kAlive, 2));
	expect(set.output(6) == nullptr, "a respawn ends the ragdoll at once");
	set.observe(head_for(7, kAlive));
	set.step(1, ground);
	SubjectHead seven = head_for(7, kShot);
	set.observe(seven);
	set.start(seven, rig, pose, nullptr, anchor, ground);
	set.step(1, ground);
	SubjectHead hidden = seven;
	hidden.hidden = true;
	set.observe(hidden);
	expect(set.output(7) == nullptr, "a hidden corpse ends its ragdoll");

	// Out of view for a while: the sleeper keeps its pose for the grace period.
	set.observe(head_for(8, kAlive));
	set.step(1, ground);
	SubjectHead eight = head_for(8, kShot);
	set.observe(eight);
	set.start(eight, rig, pose, nullptr, anchor, ground);
	set.step(100, ground);
	expect(set.output(8) != nullptr, "an unseen corpse keeps its ragdoll");
	set.step(RagdollSet::kGraceTicks + 1, ground);
	expect(set.output(8) == nullptr && set.ragdoll(8) == nullptr,
			"past the grace the ragdoll ends");

	// The awake budget.
	RagdollSet many;
	many.set_enabled(true);
	for (uint32_t k = 100; k < 100 + RagdollSet::kMaxAwake + 4; ++k)
		many.observe(head_for(k, kAlive));
	many.step(1, ground);
	for (uint32_t k = 100; k < 100 + RagdollSet::kMaxAwake + 4; ++k) {
		const SubjectHead h = head_for(k, kShot);
		if (many.observe(h) == Need::Start)
			many.start(h, rig, pose, nullptr, anchor, ground);
	}
	const RagdollStats st = many.stats();
	std::printf("set: awake %d sleeping %d started %d\n", st.awake, st.sleeping, st.started);
	expect(st.awake <= RagdollSet::kMaxAwake, "no more than the budget step at once");
	expect(st.started == RagdollSet::kMaxAwake + 4, "every death still gets its ragdoll");
	// Every corpse's hips drop a tenth of a stature, past the drift that wakes a sleeper:
	// the sleepers the burst put down wake only into the budget.
	const Vec3 lower{ anchor.x, anchor.y - 0.1f * rig->height, anchor.z };
	for (uint32_t k = 100; k < 100 + RagdollSet::kMaxAwake + 4; ++k) {
		many.observe(head_for(k, kShot));
		many.feed(k, Xform{}, lower);
	}
	many.step(1, ground);
	const RagdollStats drifted = many.stats();
	std::printf("set after the drift: awake %d sleeping %d\n", drifted.awake, drifted.sleeping);
	expect(drifted.awake <= RagdollSet::kMaxAwake, "a drifting sleeper wakes only into the budget");
	expect(drifted.awake + drifted.sleeping == RagdollSet::kMaxAwake + 4, "...and every ragdoll is kept");

	set.set_enabled(false);
	const RagdollStats off = set.stats();
	expect(set.output(1) == nullptr && off.awake + off.sleeping + off.blending == 0,
			"turning ragdolls off clears them");
}

void test_corpse_death() {
	using namespace opennova::world;
	expect(!corpse_death(172, 0).dying, "172 is no death clip");
	expect(!corpse_death(240, 0).dying, "240 is past the death clips");
	const CorpseDeath fire = corpse_death(anim_state::kDeathFire, 0);
	expect(fire.dying && !fire.ragdolls, "fire is a death that keeps its clip");
	const CorpseDeath drown = corpse_death(anim_state::kDeathDrown, 0);
	expect(drown.dying && !drown.ragdolls, "drowning is a death that keeps its clip");
	const CorpseDeath pungi = corpse_death(anim_state::kDeathPungi, 0);
	expect(pungi.ragdolls && pungi.kick.kind == Kick::Kind::Collapse, "174 collapses");
	for (int q = 0; q < 4; ++q) {
		const CorpseDeath blast = corpse_death(anim_state::kDeathGrenadeBase + q, 0);
		expect(blast.ragdolls && blast.kick.kind == Kick::Kind::Blast && blast.kick.quadrant == q,
				"a grenade death carries its quadrant");
	}
	bool round_trip = true;
	for (int b = 0; b < 19; ++b) {
		for (int q = 0; q < 4; ++q) {
			const int id = compute_death_anim_state(b, q, death_cause::kBullet);
			const CorpseDeath d = corpse_death(id, 0);
			round_trip = round_trip && d.ragdolls && d.kick.kind == Kick::Kind::Bullet &&
					d.kick.quadrant == q &&
					compute_death_anim_state(d.kick.bone, q, death_cause::kBullet) == id;
		}
	}
	expect(round_trip, "every bullet death decodes back to its group and quadrant");
	// The selector's quadrant: a round travelling the victim's way came from behind.
	const int32_t heading = 0x40000000; // facing mission +y (atan2 bearing 90 degrees)
	expect(death_quadrant_from_round(heading, 0.0f, -1.0f) == 0,
			"a round at the face: quadrant 0 (front)");
	expect(death_quadrant_from_round(heading, 0.0f, 1.0f) == 2, "a round at the back: quadrant 2");
	expect(death_quadrant_from_round(heading, -1.0f, 0.0f) == 1,
			"a round travelling west at a north-facing victim: from the right");
}

// The rules every machine must agree on, the world's (ADR 0060 d2, d6, d7): the kick's
// seed from the wire handle and the clip id alone; the water start gate; the local
// avatar's life, one more at every dead -> alive edge of the local player.
void test_corpse_rules() {
	using namespace opennova::world;
	const int shot = anim_state::kDeathBulletBase + 4;
	// Two machines that saw different respawn counts seed the same corpse alike.
	expect(corpse_death(shot, 0x1234).kick.seed == corpse_death(shot, 0x1234).kick.seed &&
	               corpse_death(shot, 0x1234).kick.seed == corpse_kick_seed(0x1234, shot),
			"the seed is the wire handle's and the clip's");
	expect(corpse_kick_seed(0x1234, shot) != corpse_kick_seed(0x1234, shot + 1) &&
	               corpse_kick_seed(0x1234, shot) != corpse_kick_seed(0x1235, shot),
			"another clip or another corpse seeds otherwise");

	auto world = std::make_unique<World>();
	expect(corpse_ragdoll_may_start(*world, -1000.0f), "a world with no water starts anywhere");
	world->env.water_z = 10 << 16;
	expect(!corpse_ragdoll_may_start(*world, 10.0f) && !corpse_ragdoll_may_start(*world, 9.5f),
			"hips at or below the water keep retail's float");
	expect(corpse_ragdoll_may_start(*world, 10.5f), "hips above the water start a ragdoll");

	world->registry.configure_pool(0, 4);
	Entity seed;
	seed.kind = EntityKind::Organic;
	world->cached.local_player = world->registry.spawn(0, seed);
	LocalAvatarLife life;
	Entity &self = *world->registry.get(world->cached.local_player);
	expect(life.observe(*world) == 0, "the first life is 0");
	self.flags |= kEntityFlagDead;
	expect(life.observe(*world) == 0, "a death keeps the life");
	self.flags &= ~kEntityFlagDead;
	expect(life.observe(*world) == 1, "a respawn is a new life");
	expect(life.observe(*world) == 1, "...once");
}

} // namespace

int main() {
	test_option();
	test_rig();
	test_pose_round_trip();
	test_drop_settles();
	test_slope();
	test_kick_direction();
	test_determinism();
	test_wall();
	test_tether();
	test_set();
	test_corpse_death();
	test_corpse_rules();
	if (failures == 0)
		std::printf("ragdoll: all passed\n");
	return failures == 0 ? 0 : 1;
}
