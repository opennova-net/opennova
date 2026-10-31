/* engine/runtime/anim BodyChannel — a clip-posed model's body channel over an in-memory rig (no
   retail data): the verbs ObjectModel's script surface and the present walks drive, moved from the
   binding (ADR 0077 R2), and the frame clock they run free on:

     1. the clip selection: a missing key resolves to the RESET binding; play_at pins the playhead at
        the tick, its repeat is a no-op (the stamp) and a new tick re-poses; a free clip moves on the
        frame clock's seconds, a pinned one does not; a loop wraps, a one-shot clamps;
     2. the blend: weight 1 or no source is the target's play_at, no target the source's, neither the
        rest request; a live blend reads back its source, playhead and weight;
     3. the remote arbitration: a first request commits and seeds (no blend); a later commit blends
        in from the playing clip at 0.1 a tick to weight 1, then runs the target seeded; a queue-class
        current state defers the next request until the playhead crosses the clip end
        [orig: NetPacket_SerializePlayerState @0x4C1153..0x4C11A6; AnimMap_UpdateEntity @0x40B5F0];
     4. the layers: the weapon channel and the aim overlay latch only a change; the override draws
        whole at weight 1 and hands back to the clip on clear;
     5. compose: the composed pose is the rig's own composition in the witnessed order (primary ->
        weapon splice -> overlay -> a Person's rows past the clip); a view-model rig takes no rows;
     6. the slot: the coarse slot's key and its no-restart free play;
     7. world::FrameTickElapsed: the first read stamps, a read adds its DWORD difference. */

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"

#include <formats/bad/bad.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/body_channel.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/world/infantry.h>
#include <runtime/world/tick_count_clock.h>

using namespace opennova;
using anim::BodyChannel;
using anim::PoseBone;
using anim::Quat;
using anim::SkeletalClips;

namespace {

constexpr size_t kBones = 19;

// A clip of kBones bones, `frames` frames at 10 fps (frames / 10 seconds long), one key per channel
// held over the whole clip: the root turned `turn` radians about Y, the rest identity; identity binds.
struct MemoryClip {
	std::vector<bad::BadBone> bones;
	std::vector<bad::BadChannel> channels;
	std::vector<bad::BadQuaternion> rotations;
	std::vector<uint16_t> lengths;
	bad::BadFile file = {};

	MemoryClip(uint32_t frames, bool loop, float turn)
			: bones(kBones), channels(kBones), rotations(kBones), lengths(kBones, static_cast<uint16_t>(frames)) {
		for (size_t i = 0; i < kBones; ++i) {
			bones[i].parent_index = i == 0 ? -1 : static_cast<int32_t>(i - 1);
			bones[i].rotation[0] = bones[i].rotation[4] = bones[i].rotation[8] = 1.0f;
			rotations[i] = {0.0f, 0.0f, 0.0f, 1.0f};
			channels[i].frame_count = 1;
			channels[i].frame_lengths = &lengths[i];
			channels[i].rotations = &rotations[i];
		}
		rotations[0] = {0.0f, std::sin(turn * 0.5f), 0.0f, std::cos(turn * 0.5f)};
		file.fps = 10;
		file.frame_count = frames;
		file.flags = loop ? 1u : 0u;
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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

bool same_pose(const std::vector<PoseBone> &a, const std::vector<PoseBone> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		const Quat &p = a[i].rotation;
		const Quat &q = b[i].rotation;
		if (p.w != q.w || p.x != q.x || p.y != q.y || p.z != q.z) return false;
		if (a[i].origin.x != b[i].origin.x || a[i].origin.y != b[i].origin.y || a[i].origin.z != b[i].origin.z)
			return false;
	}
	return true;
}

} // namespace

int main() {
	const std::string &reset = world::infantry_anim_key_ref(world::anim_state::kReset);
	const std::string &idle = world::infantry_anim_key_ref(world::anim_state::kIdle);
	const std::string &walk = world::infantry_anim_key_ref(world::anim_state::kWalkForward);
	TEST_EXPECT(reset == "anim_reset" && !idle.empty() && !walk.empty());
	TEST_EXPECT(world::infantry_anim_key_ref(-1).empty());
	TEST_EXPECT(world::infantry_anim_key_ref(world::anim_state::kIdle) == world::infantry_anim_key(world::anim_state::kIdle));

	MemoryClip rest_clip(10, true, 0.0f);     // 1.0 s loop
	MemoryClip idle_clip(10, true, 0.5f);     // 1.0 s loop
	MemoryClip walk_clip(20, false, -0.5f);   // 2.0 s one-shot
	MemoryFiles files;
	files.bads["rest.bad"] = &rest_clip.file;
	files.bads["idle.bad"] = &idle_clip.file;
	files.bads["walk.bad"] = &walk_clip.file;
	auto rig = std::make_shared<SkeletalClips>();
	TEST_EXPECT(rig->load_from_files(&files, "rest.bad",
			{{reset, "rest.bad"}, {idle, "idle.bad"}, {walk, "walk.bad"}}));
	TEST_EXPECT(rig->has_clip(idle) && rig->has_clip(walk) && rig->has_clip(reset));
	TEST_EXPECT(near(rig->clip_length(idle), 1.0, 1e-6) && near(rig->clip_length(walk), 2.0, 1e-6));

	// ---- 1. the clip selection ----
	BodyChannel ch;
	TEST_EXPECT(!ch.active());
	TEST_EXPECT(!ch.play(idle, 0)); // no rig: nothing resolves
	ch.bind(rig);
	TEST_EXPECT(ch.dirty() && !ch.active());
	TEST_EXPECT(ch.play("anim_absent", 0)); // the RESET binding
	TEST_EXPECT(ch.key() == reset);
	ch.play_at(walk, 31, 0, false);
	TEST_EXPECT(ch.key() == walk && ch.take_pose_request() == BodyChannel::PoseRequest::kPose);
	const double at31 = ch.playhead();
	TEST_EXPECT(near(at31, rig->clip_seconds_at_tick(walk, 31, 0, -1), 1e-9));
	TEST_EXPECT(ch.compose() && !ch.dirty());
	ch.play_at(walk, 31, 0, false); // the stamp: the same tick posed last is a no-op
	TEST_EXPECT(ch.take_pose_request() == BodyChannel::PoseRequest::kNone && !ch.dirty());
	ch.play_at(walk, 40, 0, false);
	TEST_EXPECT(ch.take_pose_request() == BodyChannel::PoseRequest::kPose && ch.dirty());
	// A pinned playhead holds on the clock; a one-shot clamps at its end.
	ch.advance(5.0);
	TEST_EXPECT(near(ch.playhead(), rig->clip_seconds_at_tick(walk, 40, 0, -1), 1e-9));
	ch.play_at_time(walk, 0, 9.0);
	TEST_EXPECT(near(ch.playhead(), 2.0, 1e-6));
	// A free clip runs on the frame clock's seconds and a loop wraps.
	TEST_EXPECT(ch.play(idle, 0));
	TEST_EXPECT(near(ch.playhead(), 0.0) && ch.needs_frame(true));
	ch.advance(0.25);
	TEST_EXPECT(near(ch.playhead(), 0.25, 1e-9));
	ch.advance(1.0);
	TEST_EXPECT(near(ch.playhead(), 0.25, 1e-9)); // 1.25 wraps on the 1 s loop
	// A repeat of the playing free clip keeps its playhead.
	TEST_EXPECT(ch.play(idle, 0));
	TEST_EXPECT(near(ch.playhead(), 0.25, 1e-9));
	TEST_EXPECT(ch.set_time(0.5) && near(ch.playhead(), 0.5, 1e-9));
	TEST_EXPECT(ch.take_pose_request() == BodyChannel::PoseRequest::kPose);

	// ---- 2. the blend ----
	ch.play_blend_at(idle, 5, walk, 7, 0.25, 0, 0);
	TEST_EXPECT(ch.has_blend() && ch.blend_source_key() == idle && ch.key() == walk);
	TEST_EXPECT(near(ch.blend_weight(), 0.25, 1e-7));
	TEST_EXPECT(near(ch.blend_source_time(), rig->clip_seconds_at_tick(idle, 5, 0, -1), 1e-9));
	ch.play_blend_at(idle, 5, walk, 7, 1.0, 0, 0); // weight 1: the target alone
	TEST_EXPECT(!ch.has_blend() && ch.key() == walk);
	ch.play_blend_at(idle, 5, "", 7, 0.5, 0, 0); // no target key: RESET stands in
	TEST_EXPECT(ch.has_blend() && ch.key() == reset);
	{
		// A rig with no RESET: an unresolved target plays the source; neither is the rest.
		auto bare = std::make_shared<SkeletalClips>();
		TEST_EXPECT(bare->load_from_files(&files, "rest.bad", {{idle, "idle.bad"}}));
		BodyChannel b;
		b.bind(bare);
		b.play_blend_at(idle, 3, "anim_absent", 7, 0.5, 0, 0);
		TEST_EXPECT(b.key() == idle && !b.has_blend());
		(void)b.take_pose_request();
		b.play_blend_at("anim_absent", 3, "anim_absent", 7, 0.5, 0, 0);
		TEST_EXPECT(b.take_pose_request() == BodyChannel::PoseRequest::kRest && !b.active());
		TEST_EXPECT(b.pose().empty());
	}

	// ---- 3. the remote arbitration ----
	{
		BodyChannel r;
		r.bind(rig);
		r.remote_request(world::anim_state::kIdle, idle, 0u, 4); // first: commit + seed
		TEST_EXPECT(r.key() == idle && !r.has_blend() && !r.remote_needs_tick());
		TEST_EXPECT(near(r.playhead(), rig->clip_seconds_at_tick(idle, 4, 0, -1), 1e-9));
		r.remote_request(world::anim_state::kIdle, idle, 0u, 9); // repeat: keep
		TEST_EXPECT(near(r.playhead(), rig->clip_seconds_at_tick(idle, 4, 0, -1), 1e-9));
		r.remote_request(world::anim_state::kWalkForward, walk, 0u, 0); // commit: blend in
		TEST_EXPECT(r.key() == walk && r.has_blend() && r.blend_source_key() == idle);
		TEST_EXPECT(r.blend_weight() == 0.0f && r.remote_needs_tick());
		TEST_EXPECT(!r.remote_tick(world::anim_state::kReset)); // an unrelated state ticks nothing
		int ticks = 0;
		while (r.remote_needs_tick() && ticks < 20) {
			TEST_EXPECT(r.remote_tick(world::anim_state::kWalkForward));
			++ticks;
		}
		TEST_EXPECT(ticks == 10); // 0.1 a tick in float
		TEST_EXPECT(!r.has_blend() && r.key() == walk);
		TEST_EXPECT(near(r.playhead(), rig->clip_seconds_at_tick(walk, 10, 0, -1), 1e-9));
		// A queue-class current (flags 0x4) defers the next request to the clip's end.
		BodyChannel q;
		q.bind(rig);
		q.remote_request(world::anim_state::kIdle, idle, 0x4u, 0);
		q.remote_request(world::anim_state::kWalkForward, walk, 0u, 0);
		TEST_EXPECT(q.key() == idle && q.remote_pending() && q.remote_needs_tick());
		q.advance(0.5);
		TEST_EXPECT(q.key() == idle && q.remote_pending());
		q.advance(0.6); // past the loop's 1 s boundary: the pending state promotes, blending in
		TEST_EXPECT(!q.remote_pending() && q.key() == walk && q.has_blend());
		q.reset_remote();
		TEST_EXPECT(!q.has_blend() && !q.remote_needs_tick());
	}

	// ---- 3b. the arbitration cases the GUT suite pinned on ObjectModel (moved here, ADR 0077 R2),
	// over the retail flags table [orig: g_AnimStateFlagsTable @0x8139E8] ----
	{
		const uint32_t f1 = world::infantry_anim_flags(1);
		const uint32_t f2 = world::infantry_anim_flags(2);
		const uint32_t f41 = world::infantry_anim_flags(41);
		const uint32_t f43 = world::infantry_anim_flags(43);
		const uint32_t f48 = world::infantry_anim_flags(48);
		const uint32_t f115 = world::infantry_anim_flags(115);
		TEST_EXPECT((f41 & 0x4u) != 0 && (f115 & 0x20u) != 0 && (f43 & 0x1u) == 0 && (f1 & 0x1u) != 0);
		// A steady state never re-scrubs a free-running channel.
		{
			BodyChannel c;
			c.bind(rig);
			c.remote_request(1, walk, f1, 10);
			c.advance(0.05);
			const double t = c.playhead();
			c.remote_request(1, walk, f1, 22);
			TEST_EXPECT(near(c.playhead(), t, 1e-9));
		}
		// A to B retargeted to C mid-blend still blends from A, at A's own playhead.
		{
			BodyChannel c;
			c.bind(rig);
			c.remote_request(43, idle, f43, 4);
			c.remote_request(1, walk, f1, 0);
			TEST_EXPECT(c.remote_tick(1));
			const double source_time = c.blend_source_time();
			c.remote_request(2, reset, f2, 0);
			TEST_EXPECT(c.key() == reset && c.blend_source_key() == idle && c.blend_weight() == 0.0f);
			TEST_EXPECT(near(c.blend_source_time(), source_time, 1e-9));
			TEST_EXPECT(near(source_time, rig->clip_seconds_at_tick(idle, 5, 0, -1), 1e-9));
		}
		// A target with flag 0x400 blends over fifteen ticks.
		{
			BodyChannel c;
			c.bind(rig);
			anim::BodyChannelVerbs verbs(c);
			c.remote_request(43, idle, 0u, 0);
			c.remote_request(2, walk, 0x400u, 0);
			for (int tick = 0; tick < 14; ++tick) TEST_EXPECT(verbs.remote_tick(2));
			TEST_EXPECT(!verbs.remote_tick(2));
		}
		// The pulse accepts before the folded final state queues behind it; the queued state still
		// ticks the transition, and a reset clears it.
		{
			BodyChannel c;
			c.bind(rig);
			c.remote_request(48, idle, f48, 10);
			c.remote_request(41, walk, f41, 0);
			c.remote_request(48, idle, f48, 11);
			TEST_EXPECT(c.key() == walk && c.remote_needs_tick() && c.remote_pending());
			TEST_EXPECT(c.remote_tick(48));
			c.reset_remote();
			TEST_EXPECT(!c.remote_needs_tick());
		}
		// A locked current state defers the request; its repeat keeps the pending one armed; the
		// promotion at the clip's end starts the new state at tick zero.
		{
			BodyChannel c;
			c.bind(rig);
			c.remote_request(41, idle, f41, 0);
			c.remote_request(1, walk, f1, 200);
			TEST_EXPECT(c.key() == idle);
			c.remote_request(41, idle, f41, 90);
			TEST_EXPECT(c.remote_needs_tick());
			c.advance(rig->clip_length(idle) + 0.01);
			TEST_EXPECT(c.key() == walk && near(c.playhead(), 0.0, 1e-6));
		}
		// The exit gate (0x20) queues an incoming state without flag 0x1; one with it interrupts and
		// takes its own phase seed.
		{
			BodyChannel c;
			c.bind(rig);
			c.remote_request(115, idle, f115, 0);
			c.remote_request(43, walk, f43, 90);
			TEST_EXPECT(c.key() == idle);
			c.remote_request(1, reset, f1, 6);
			TEST_EXPECT(c.key() == reset);
			TEST_EXPECT(near(c.playhead(), rig->clip_seconds_at_tick(reset, 6, 0, -1), 1e-9));
		}
		// A seeded clip takes its tick, then runs free.
		{
			BodyChannel c;
			c.bind(rig);
			c.play_seeded(idle, 31);
			TEST_EXPECT(c.take_pose_request() == BodyChannel::PoseRequest::kPose);
			const double seeded = rig->clip_seconds_at_tick(idle, 31, 0, -1);
			TEST_EXPECT(near(c.playhead(), seeded, 1e-9));
			c.advance(0.1);
			TEST_EXPECT(near(c.playhead(), std::fmod(seeded + 0.1, 1.0), 1e-9));
		}
	}

	// ---- 4. the layers ----
	anim::WeaponChannel weapon;
	weapon.key = walk;
	weapon.phase_ticks = 12;
	(void)ch.compose();
	TEST_EXPECT(ch.set_weapon_channel(weapon) && ch.dirty());
	(void)ch.compose();
	TEST_EXPECT(!ch.set_weapon_channel(weapon) && !ch.dirty());
	TEST_EXPECT(ch.has_weapon_channel() && ch.weapon_channel().phase_ticks == 12);
	Quat deltas[anim::kOverlayClassCount];
	deltas[anim::kOverlayHead] = Quat{std::cos(0.2f), std::sin(0.2f), 0.0f, 0.0f};
	TEST_EXPECT(ch.set_aim_overlay(deltas) && ch.aim_overlay_valid());
	(void)ch.compose();
	TEST_EXPECT(!ch.set_aim_overlay(deltas) && !ch.dirty());
	TEST_EXPECT(ch.clear_aim_overlay() && !ch.clear_aim_overlay());
	TEST_EXPECT(ch.set_aim_overlay(deltas));

	// ---- 5. compose ----
	ch.play_at(walk, 7, 0, false);
	TEST_EXPECT(ch.compose());
	{
		std::vector<PoseBone> expected;
		rig->eval_pose(walk, rig->clip_seconds_at_tick(walk, 7, 0, -1), 0, expected);
		rig->apply_pose_overlay(expected, deltas, rig->overlay_classes(), walk,
				rig->clip_seconds_at_tick(walk, 12, 0, -1));
		rig->pose_person_rows_past_clip(expected, rig->clip_file_bones(walk, 0), deltas, rig->overlay_classes());
		TEST_EXPECT(same_pose(ch.pose(), expected));
	}
	// The overlay off: the primary channel bare, the weapon channel not spliced.
	ch.clear_aim_overlay();
	TEST_EXPECT(ch.compose());
	{
		std::vector<PoseBone> expected;
		rig->eval_pose(walk, rig->clip_seconds_at_tick(walk, 7, 0, -1), 0, expected);
		rig->pose_person_rows_past_clip(expected, rig->clip_file_bones(walk, 0), nullptr, {});
		TEST_EXPECT(same_pose(ch.pose(), expected));
		// A view-model rig takes no Person rows.
		ch.set_person(false);
		TEST_EXPECT(ch.dirty() && ch.compose());
		std::vector<PoseBone> plain;
		rig->eval_pose(walk, rig->clip_seconds_at_tick(walk, 7, 0, -1), 0, plain);
		TEST_EXPECT(same_pose(ch.pose(), plain));
		ch.set_person(true);
	}
	// The blend composes over the channel window.
	ch.play_blend_at(idle, 3, walk, 7, 0.4, 0, 0);
	TEST_EXPECT(ch.compose());
	{
		std::vector<PoseBone> expected;
		rig->eval_pose_blended(idle, rig->clip_seconds_at_tick(idle, 3, 0, -1), walk,
				rig->clip_seconds_at_tick(walk, 7, 0, -1), 0.4f, expected, 0, 0);
		rig->pose_person_rows_past_clip(expected, rig->clip_file_bones(walk, 0), nullptr, {});
		TEST_EXPECT(same_pose(ch.pose(), expected));
	}
	// The override draws whole at weight 1, and clear hands back to the clip.
	std::vector<PoseBone> ragdoll(kBones);
	ragdoll[3].rotation = Quat{std::cos(0.4f), 0.0f, 0.0f, std::sin(0.4f)};
	ch.set_override(ragdoll, 1.0f);
	TEST_EXPECT(ch.has_override() && ch.take_pose_request() == BodyChannel::PoseRequest::kPose);
	TEST_EXPECT(ch.compose() && same_pose(ch.pose(), ragdoll));
	TEST_EXPECT(ch.clear_override() && !ch.has_override() && !ch.clear_override());
	TEST_EXPECT(ch.compose() && !same_pose(ch.pose(), ragdoll));
	{
		std::vector<PoseBone> clip;
		TEST_EXPECT(ch.eval_clip_pose(0.0, clip) && clip.size() == rig->bone_count());
	}

	// ---- 6. the slot ----
	{
		BodyChannel s;
		s.bind(rig);
		TEST_EXPECT(s.slot_key(-1).empty());
		const std::string key = s.slot_key(0);
		TEST_EXPECT(key == rig->slot_to_key(0));
		std::string play_key;
		if (!key.empty()) {
			TEST_EXPECT(s.slot_play_key(0, play_key) && play_key == key);
			TEST_EXPECT(s.play(play_key, 0));
			TEST_EXPECT(!s.slot_play_key(0, play_key)); // the free loop is not restarted
		}
		s.stop();
		TEST_EXPECT(!s.needs_frame(true) || s.dirty()); // a stopped clip runs no clock
		(void)s.compose();
		TEST_EXPECT(!s.needs_frame(true));
	}

	// ---- 7. the frame clock ----
	world::FrameTickElapsed clock;
	TEST_EXPECT(clock.advance(1000u) == 0u);
	TEST_EXPECT(clock.advance(1015u) == 15u);
	TEST_EXPECT(clock.advance(1031u) == 16u);
	TEST_EXPECT(clock.advance(1031u) == 0u);
	world::FrameTickElapsed wrap;
	(void)wrap.advance(0xFFFFFFF0u);
	TEST_EXPECT(wrap.advance(0x0000000Fu) == 0x1Fu);
	return 0;
}
