#pragma once

// Every ragdoll a machine draws (ADR 0060): which corpse starts one, when it
// ends, and the pose each one draws. Its embedder observes every skeletal
// subject it draws once a frame, feeds the running ones their placement and
// the simulation corpse's hips, steps the set by the frame's logic ticks, and
// draws each subject's output over its clip pose. A subject is keyed by an
// identity that is the same on every machine (its wire handle), and its kick
// comes from its death clip, so every machine ragdolls the same corpse the
// same way without a byte on the wire.

#include <runtime/ragdoll/ragdoll_solver.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace opennova::ragdoll {

// What the embedder knows of a subject this frame.
struct SubjectHead {
	uint32_t key = 0;
	int32_t life = 0; // the respawn epoch: a new life ends any ragdoll
	bool dying = false; // playing a death clip
	bool ragdolls = false; // a death a ragdoll replaces (kick holds its push)
	// Free to fall: false for a severed dismemberment piece, which keeps retail's motion.
	bool eligible = true;
	bool hidden = false;
	Kick kick;
	Xform node;
};

enum class Need : uint8_t {
	None, // draw the clip (or a ragdoll blending back out: see output())
	Start, // a death this frame: call start()
	Run, // a running ragdoll: call feed()
};

struct RagdollOutput {
	std::vector<anim::PoseBone> pose; // parent-local, model space
	float weight = 0.0f; // 1: the ragdoll alone; below: blending back to the clip
};

struct RagdollStats {
	int awake = 0;
	int sleeping = 0;
	int blending = 0;
	int started = 0; // since the set was made
};

class RagdollSet {
public:
	// At most this many ragdolls step at once; a new death puts the oldest to sleep.
	static constexpr int kMaxAwake = 16;
	// At most this many are kept; past it the oldest sleeper ends.
	static constexpr int kMaxKept = 64;
	// Ticks a ragdoll takes to hand its body back to the clip.
	static constexpr int kBlendOutTicks = 10;
	// Ticks a sleeper outlives its subject (a corpse out of a joiner's view and back).
	static constexpr int kGraceTicks = 1875;

	void set_enabled(bool enabled);
	bool enabled() const { return enabled_; }

	Need observe(const SubjectHead &head);
	// `pose` and `pose_prev` are the death clip's pose now and one tick
	// before; `anchor` the simulation corpse's hips pivot (world).
	bool start(const SubjectHead &head, std::shared_ptr<const Rig> rig,
			const std::vector<anim::PoseBone> &pose, const std::vector<anim::PoseBone> *pose_prev,
			const Vec3 &anchor, const Collider &collider);
	void feed(uint32_t key, const Xform &node, const Vec3 &anchor);
	void step(int ticks, const Collider &collider);
	// The pose to draw over the subject's clip, or null for the clip alone.
	const RagdollOutput *output(uint32_t key) const;
	const Ragdoll *ragdoll(uint32_t key) const;
	void clear();
	RagdollStats stats() const;

private:
	struct Entry {
		int32_t life = 0;
		bool seen = false;
		bool was_dying = false;
		bool was_eligible = false;
		bool observed = false; // this frame
		int missing = 0; // ticks since last observed
		Vec3 node_origin;
		int node_ticks = 0; // ticks since node_origin
		std::unique_ptr<Ragdoll> ragdoll;
		Xform node;
		Vec3 anchor;
		int blend_left = 0; // > 0: blending out
		uint64_t order = 0; // start order (eviction)
		RagdollOutput out;
	};

	void end(Entry &entry);
	void make_room();

	std::unordered_map<uint32_t, Entry> entries_;
	bool enabled_ = false;
	uint64_t next_order_ = 0;
	int started_ = 0;
};

} // namespace opennova::ragdoll
