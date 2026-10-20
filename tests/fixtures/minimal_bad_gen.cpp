// Generator + guard for the synthetic clip sets fixtures/anim/rig19* and vm40*
// (fixtures/README.md): a 19-bone JO person rig's table and clips, authored
// from small integer-friendly data and minted through the clip construction
// seam (formats/bad/bad_build.h, the one code path that builds a clip and the
// table naming it) and the .bad writer, so no retail bytes live in the tree
// (ADR 0003). The set pairs with fixtures/threedi/synth/person.3di: both take
// the rig from person_rig.h, so a clip bone and a model part pair by BN## index.
//
//   fixtures/anim/rig19.adm        the table (rig19_rst its reset row)
//   fixtures/anim/rig19_rst.bad    the reset: every key the rest pose, three
//                                  keys over two frames (DT1RST's length)
//   fixtures/anim/rig19_idle.bad   the stock 19-bone idle's shape, the shape the
//                                  retired BINOC.bad tests read: version 1,
//                                  header 80, 30 fps, 3 frames (4 keys per bone,
//                                  each a frame long), a loop (flags 1), 4 events
//                                  (bottom 31/32, top 55/32), no translations;
//                                  bone 0 `BN01 Hips` the root, 13/14 the neck
//                                  and head; every key a turn of its own, every
//                                  fourth bone's past 150 degrees (the last key
//                                  repeating the first from the opposite
//                                  hemisphere, a seamless loop), so no bone's
//                                  bind is the identity
//   fixtures/anim/rig19_twist.bad  rig19_idle with bone 5 (BN06 R UpperArm, a
//                                  weapon-channel mask bone) and bone 7 (BN08 R
//                                  Thigh, not one) turned 90 degrees about their
//                                  local Y on every key, the bone table and the
//                                  events untouched: the weapon channel's
//                                  probe pair (the retired BINOC_twist.bad's
//                                  recipe, skeletal_clips_weapon_channel_test)
//   fixtures/anim/rig19_crch.bad   a crouch: the thighs swing the knees forward,
//                                  the calves back
//   fixtures/anim/rig19_rld1.bad,  two reloads: the arms (mask bones 5, 6, 9, 10,
//   fixtures/anim/rig19_rld2.bad   15, 16) turn and come back over 20 frames
//   fixtures/anim/rig19_dthf.bad   the four torso deaths (forward, right, back,
//   fixtures/anim/rig19_dthr.bad   left): the whole body tips 43.6 degrees over
//   fixtures/anim/rig19_dthb.bad   30 frames, one-shot (the back death's odd
//                                  bones keyed from the opposite hemisphere)
//   fixtures/anim/rig19_dthl.bad
//   fixtures/anim/rig19_walk.bad   a walk loop: 30 frames stepping 1/16 forward
//                                  a frame, the legs swinging, footsteps (bits
//                                  0x1 / 0x2) on the plant frames and a foley bit
//                                  (0x20)
// And a 40-bone first-person rig's set, the shape the stock first-person
// tables carry (mp5_1st.adm, 357_1st.adm: the reset and the eight weapon slots
// in the stock order; clips that carry translations; every event standing
// still, bottom = top = 137/128):
//   fixtures/anim/vm40.adm         the table: anim_reset, wpn_idle, wpn_fire,
//                                  wpn_recoil (the fire clip again), wpn_reload,
//                                  wpn_empty, wpn_switchto, wpn_switchfrom,
//                                  wpn_switchrank
//   fixtures/anim/vm40_rst.bad     the reset: one frame, translated (rows 0 and
//                                  1 per bone, and the pad row), the 357_RST.bad
//                                  shape the parser pins
//   fixtures/anim/vm40_i.bad       idle (a loop)
//   fixtures/anim/vm40_f.bad       fire: the bolt slides back and returns, a fire
//                                  marker bit (0x4)
//   fixtures/anim/vm40_r.bad       reload: the magazine drops and returns
//   fixtures/anim/vm40_e.bad       empty (a loop)
//   fixtures/anim/vm40_swt.bad     switch to, flag bit 3 (as the stock draw
//                                  clips carry it)
//   fixtures/anim/vm40_swf.bad     switch from
//   fixtures/anim/vm40_swr.bad     switch rank
//   fixtures/anim/vm40_brk.bad     the reset with every child bone's dead position
//                                  field X-triplicated (x copied into y and z),
//                                  the breakage the stock exporter left in 6720
//                                  of 13517 shipped bones (ak47_1st's rig: its
//                                  reset positions the runtime never reads,
//                                  bad_build.h); minted by hand from vm40_rst,
//                                  not a clip the seam would write
// And one clip per form the shipped clips take that the sets above do not
// (witnessed over the 477 clips of JOTAC's tree, 2026-10-19), each a lone clip
// or built deliberately where the construction seam would write the form
// another way:
//   fixtures/anim/rig19_short.bad  two frames, two keys a bone (one fewer than
//                                  the frame count's fence post): DT1RST.BAD's
//                                  and stgr_RST.bad's channels
//   fixtures/anim/rig19_sprs.bad   eleven frames, every bone keyed sparsely on a
//                                  duration table of its own (1..4 frames a key,
//                                  summing to the 12 fence posts): DVFLEE1E.BAD,
//                                  the one sparse shipped clip
//   fixtures/anim/rig19_v0.bad     a version 0 clip: 20-byte events with no
//                                  trigger word (m60_rst / m60_1d / m60_1r)
//   fixtures/anim/rig19_junk.bad   the exporter's leftovers the reader drops:
//                                  every name's bytes past its NUL filled, the
//                                  three bytes before the index byte set (202
//                                  clips), one index byte not its bone's (12),
//                                  the root's parent address and a leaf's child
//                                  address stale, a parent's child address its
//                                  last child's (every clip carries some), and
//                                  flags 0x9 (a loop with bit 3, 26 clips)
//   fixtures/anim/vm40_long.bad    nine keys a bone over four frames and the
//                                  translation rows to match (M60_1i.bad: 181
//                                  keys on 150 frames, rows to frame 182)
//   fixtures/anim/vm40_nopad.bad   a translated clip with no pad row past row
//                                  frame_count (stgr_RST.bad; 200 of the 202
//                                  translated clips carry one)
//   fixtures/anim/rig19_b3.bad     flags 0x8: bit 3 alone, no loop, no
//                                  translation (Idl_rld.bad, the one such clip)
//   fixtures/anim/vm40_b3lp.bad    flags 0xB: bit 3 on a translated loop
//                                  (74gl_1d.bad, FNC_1i.bad, M4_1e.bad, P90_1d.bad:
//                                  six clips)
// Every turn is a rational rotation (a quaternion (t * axis, 1) normalized: t
// the tangent of half the angle), so the bytes come out the same on every
// platform. Default: rebuild every file in memory and byte-compare the
// committed copies; `--write` (re)writes them. Every run re-reads the committed
// bytes and checks the shape the consumers pin.
#include "fixtures/person_rig.h"
#include "fixtures/vm40_rig.h"

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_write.h>
#include <runtime/anim/adm_clip_index.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::bad;

namespace {

int failures = 0;

bool expect(bool cond, const std::string &msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
		++failures;
	}
	return cond;
}

// The rotation (t * axis, 1) normalized, mission axes: t is the tangent of half
// the angle about `axis` (a unit axis, so every product stays exact before the
// one normalization).
BadBuildQuat turn(int axis, double t) {
	const double n = std::sqrt(1.0 + t * t);
	BadBuildQuat q;
	q.x = axis == 0 ? t / n : 0.0;
	q.y = axis == 1 ? t / n : 0.0;
	q.z = axis == 2 ? t / n : 0.0;
	q.w = 1.0 / n;
	return q;
}

constexpr int kX = 0; // mission x, forward
constexpr int kY = 1; // mission y, left
constexpr int kZ = 2; // mission z, up

// The rig: names, parents and pivots from person_rig.h, no keys yet.
std::vector<BadBuildBone> rig_bones() {
	std::vector<BadBuildBone> bones(person_rig::kBones);
	for (int i = 0; i < person_rig::kBones; ++i) {
		BadBuildBone &b = bones[static_cast<size_t>(i)];
		b.name = person_rig::kNames[i];
		b.parent = i == 0 ? -1 : person_rig::kParents[i];
		b.pivot = BadBuildVec3{person_rig::kPivots[i][0], person_rig::kPivots[i][1], person_rig::kPivots[i][2]};
		// The bone's length: the distance to its first child's pivot (0 for a leaf).
		for (int c = i + 1; c < person_rig::kBones; ++c) {
			if (person_rig::kParents[c] != i) continue;
			const double dx = person_rig::kPivots[c][0] - person_rig::kPivots[i][0];
			const double dy = person_rig::kPivots[c][1] - person_rig::kPivots[i][1];
			const double dz = person_rig::kPivots[c][2] - person_rig::kPivots[i][2];
			b.length = std::sqrt(dx * dx + dy * dy + dz * dz);
			break;
		}
	}
	return bones;
}

// The hips' and the head's height over the ground of the standing rig.
constexpr double kBottom = 31.0 / 32.0;
constexpr double kTop = 55.0 / 32.0;

void standing_events(BadBuildClip &clip) {
	clip.events.assign(clip.frame_count + 1, BadBuildEvent{});
	for (BadBuildEvent &e : clip.events) {
		e.bottom = kBottom;
		e.top = kTop;
	}
}

// Every bone holds `keys` keys of `at(bone, key)`.
template <typename At>
void key_every_bone(BadBuildClip &clip, At at) {
	for (size_t b = 0; b < clip.bones.size(); ++b) {
		clip.bones[b].keys.clear();
		for (uint32_t k = 0; k <= clip.frame_count; ++k) clip.bones[b].keys.push_back(at(static_cast<int>(b), static_cast<int>(k)));
	}
}

BadBuildClip make_rst() {
	BadBuildClip clip;
	clip.name = "rig19_rst";
	clip.flags = BAD_FLAG_LOOP;
	clip.frame_count = 2;
	clip.bones = rig_bones();
	key_every_bone(clip, [](int, int) { return BadBuildQuat{}; });
	standing_events(clip);
	return clip;
}

BadBuildClip make_idle() {
	BadBuildClip clip;
	clip.name = "rig19_idle";
	clip.flags = BAD_FLAG_LOOP;
	clip.frame_count = 3;
	clip.bones = rig_bones();
	// Bone b turns about mission axis b % 3 by tan(half angle) = (8 + 2 (b % 5)
	// + k) / 128 on key k (7 to 14 degrees); every fourth bone (0, 4, 8, 12, 16:
	// about x, y and z) by 4 + k / 16 instead (152 degrees and on), as a body
	// rig's bind turns its limbs past a quarter turn. The last key repeats the
	// first from the opposite hemisphere, (-x, -y, -z, -w): the same turn, which
	// the slerp takes the short way, as the shipped clips store such neighbours
	// (4573 key pairs in 218 of the 477, bad_build.h).
	key_every_bone(clip, [](int b, int k) {
		const int step = k == 3 ? 0 : k;
		const double t = b % 4 == 0 ? 4.0 + step / 16.0 : (8.0 + 2.0 * (b % 5) + step) / 128.0;
		BadBuildQuat q = turn(b % 3, t);
		if (k == 3) q = BadBuildQuat{-q.x, -q.y, -q.z, -q.w};
		return q;
	});
	standing_events(clip);
	return clip;
}

BadBuildClip make_crouch() {
	BadBuildClip clip;
	clip.name = "rig19_crch";
	clip.flags = BAD_FLAG_LOOP;
	clip.frame_count = 2;
	clip.bones = rig_bones();
	// About mission y, a negative turn swings a bone hanging down toward +x
	// (forward): the thighs by -53.13 degrees (t = -1/2), the calves back by
	// +53.13 (t = 1/2); the rest of the body stands.
	key_every_bone(clip, [](int b, int) {
		if (b == 7 || b == 8) return turn(kY, -0.5);
		if (b == 11 || b == 12) return turn(kY, 0.5);
		return BadBuildQuat{};
	});
	standing_events(clip);
	for (BadBuildEvent &e : clip.events) {
		e.bottom = 21.0 / 32.0;
		e.top = 45.0 / 32.0;
	}
	return clip;
}

BadBuildClip make_reload(const char *name, int axis) {
	BadBuildClip clip;
	clip.name = name;
	clip.frame_count = 20;
	clip.bones = rig_bones();
	// The arms turn up to t = 7/20 (38.6 degrees) at frame 10 and back.
	key_every_bone(clip, [axis](int b, int k) {
		const bool arm = b == 5 || b == 6 || b == 9 || b == 10 || b == 15 || b == 16;
		if (!arm) return BadBuildQuat{};
		const int ramp = k <= 10 ? k : 20 - k;
		return turn(axis, 0.035 * ramp);
	});
	standing_events(clip);
	return clip;
}

// Forward tips the body's up toward +x (a positive turn about y), right toward
// -y (a positive turn about x), back and left the opposite ways.
BadBuildClip make_death(const char *name, int axis, double sign, bool flipped = false) {
	BadBuildClip clip;
	clip.name = name;
	clip.frame_count = 30;
	clip.bones = rig_bones();
	// Every bone takes the hips' turn: the body tips whole, to t = 0.4 (43.6
	// degrees) at the last frame. `flipped` stores the odd bones' keys from the
	// opposite hemisphere, (-x, -y, -z, -w): the same turns, as the shipped clips
	// store keys from either one (bad_build.h), so a pose's neighbouring bones can
	// hold opposite signs.
	key_every_bone(clip, [axis, sign, flipped](int b, int k) {
		const BadBuildQuat q = turn(axis, sign * 0.4 * k / 30.0);
		return flipped && b % 2 == 1 ? BadBuildQuat{-q.x, -q.y, -q.z, -q.w} : q;
	});
	standing_events(clip);
	return clip;
}

BadBuildClip make_walk() {
	BadBuildClip clip;
	clip.name = "rig19_walk";
	clip.flags = BAD_FLAG_LOOP;
	clip.frame_count = 30;
	clip.bones = rig_bones();
	// The legs swing about mission y, the right leg ahead on frames 0..15 and the
	// left on 15..30: t ramps -1/4 .. 1/4 and back.
	key_every_bone(clip, [](int b, int k) {
		const int phase = k <= 15 ? k : 30 - k;
		const double swing = -0.25 + phase / 30.0;
		if (b == 7 || b == 11) return turn(kY, swing);
		if (b == 8 || b == 12) return turn(kY, -swing);
		return BadBuildQuat{};
	});
	standing_events(clip);
	// The end pose (frame 30, the loop's seam) carries no footstep of its own: the game plays frames 0 to 29 and
	// never reads its event, frame 0's footstep firing each cycle (the editor's animation.end_pose_trigger).
	for (size_t k = 0; k < clip.events.size(); ++k) {
		BadBuildEvent &e = clip.events[k];
		e.velocity = BadBuildVec3{1.0 / 16.0, 0.0, 0.0};
		e.trigger = k == 0 ? 0x1 : (k == 15 ? 0x2 : (k == 8 ? 0x20 : 0));
	}
	return clip;
}

BadBuildSet make_set() {
	BadBuildSet set;
	set.adm_name = "rig19.adm";
	set.clips = {make_rst(), make_idle(), make_crouch(), make_reload("rig19_rld1", kY), make_reload("rig19_rld2", kZ),
		make_death("rig19_dthf", kY, 1.0), make_death("rig19_dthr", kX, 1.0), make_death("rig19_dthb", kY, -1.0, true),
		make_death("rig19_dthl", kX, -1.0), make_walk()};
	set.rows = {{"anim_reset", {"rig19_rst"}}, {"anim_idle", {"rig19_idle"}}, {"anim_idle_crouch", {"rig19_crch"}},
		{"anim_walk_forward", {"rig19_walk"}}, {"anim_reload", {"rig19_rld1"}}, {"anim_reload2", {"rig19_rld2"}},
		{"anim_death_bullet_torso_forward", {"rig19_dthf"}}, {"anim_death_bullet_torso_right", {"rig19_dthr"}},
		{"anim_death_bullet_torso_back", {"rig19_dthb"}}, {"anim_death_bullet_torso_left", {"rig19_dthl"}}};
	return set;
}

// The 40-bone first-person rig (vm40_rig.h), no keys yet.
constexpr int kVmReceiver = vm40_rig::kReceiver, kVmMagazine = vm40_rig::kMagazine, kVmBolt = vm40_rig::kBolt;

std::vector<BadBuildBone> vm_bones() {
	std::vector<BadBuildBone> bones;
	for (const vm40_rig::Bone &r : vm40_rig::bones()) {
		BadBuildBone b;
		b.name = r.name;
		b.parent = r.parent;
		b.pivot = BadBuildVec3{r.pivot[0], r.pivot[1], r.pivot[2]};
		b.length = r.length;
		bones.push_back(b);
	}
	return bones;
}

constexpr double kVmStill = 137.0 / 128.0;

// A first-person clip: `frames` intervals, every key at `at(bone, key)`, every
// bone's translation row at `slide(bone, key)` (mission axes), every event
// standing still.
template <typename At, typename Slide>
BadBuildClip vm_clip(const char *name, uint32_t frames, uint32_t flags, At at, Slide slide) {
	BadBuildClip clip;
	clip.name = name;
	clip.flags = flags | BAD_FLAG_TRANSLATION;
	clip.frame_count = frames;
	clip.bones = vm_bones();
	key_every_bone(clip, at);
	for (size_t b = 0; b < clip.bones.size(); ++b)
		for (uint32_t k = 0; k <= frames; ++k)
			clip.bones[b].translations.push_back(slide(static_cast<int>(b), static_cast<int>(k)));
	clip.events.assign(frames + 1, BadBuildEvent{});
	for (BadBuildEvent &e : clip.events) e.bottom = e.top = kVmStill;
	return clip;
}

BadBuildQuat still(int, int) { return BadBuildQuat{}; }
BadBuildVec3 no_slide(int, int) { return BadBuildVec3{}; }

// The arms (upper arm, forearm and hand of each side) turn by `t(key)` about `axis`.
template <typename T>
auto arms_turn(int axis, T t) {
	return [axis, t](int b, int k) {
		const bool arm = (b >= 1 && b <= 3) || (b >= 19 && b <= 21);
		return arm ? turn(axis, t(k)) : BadBuildQuat{};
	};
}

BadBuildSet make_vm_set() {
	BadBuildSet set;
	set.adm_name = "vm40.adm";
	BadBuildClip fire = vm_clip("vm40_f", 4, 0,
			arms_turn(kY, [](int k) { return k == 1 ? -0.0625 : (k == 2 ? -0.03125 : 0.0); }),
			[](int b, int k) { return b == kVmBolt && (k == 1 || k == 2) ? BadBuildVec3{-0.0625, 0.0, 0.0} : BadBuildVec3{}; });
	fire.events[1].trigger = 0x4;
	set.clips = {vm_clip("vm40_rst", 1, 0, still, no_slide),
		vm_clip("vm40_i", 8, BAD_FLAG_LOOP, arms_turn(kZ, [](int k) { return (k <= 4 ? k : 8 - k) / 128.0; }), no_slide),
		fire,
		vm_clip("vm40_r", 30, 0, arms_turn(kX, [](int k) { return (k <= 15 ? k : 30 - k) / 60.0; }),
				[](int b, int k) {
					const int drop = k <= 15 ? k : 30 - k;
					return b == kVmMagazine ? BadBuildVec3{0.0, 0.0, -drop / 64.0} : BadBuildVec3{};
				}),
		vm_clip("vm40_e", 2, BAD_FLAG_LOOP, still, no_slide),
		vm_clip("vm40_swt", 10, BAD_FLAG_BIT3, arms_turn(kY, [](int k) { return (10 - k) / 25.0; }), no_slide),
		vm_clip("vm40_swf", 10, 0, arms_turn(kY, [](int k) { return k / 25.0; }), no_slide),
		vm_clip("vm40_swr", 6, 0, arms_turn(kZ, [](int k) { return (k <= 3 ? k : 6 - k) / 24.0; }), no_slide)};
	set.rows = {{"anim_reset", {"vm40_rst"}}, {"anim_wpn_idle", {"vm40_i"}}, {"anim_wpn_fire", {"vm40_f"}},
		{"anim_wpn_recoil", {"vm40_f"}}, {"anim_wpn_reload", {"vm40_r"}}, {"anim_wpn_empty", {"vm40_e"}},
		{"anim_wpn_switchto", {"vm40_swt"}}, {"anim_wpn_switchfrom", {"vm40_swf"}},
		{"anim_wpn_switchrank", {"vm40_swr"}}};
	return set;
}

struct Minted {
	std::string name;
	std::vector<uint8_t> bytes;
};

// The lone clips (a clip minted on its own composes against its own first key).
bool mint_lone(const BadBuildClip &clip, std::vector<Minted> &out) {
	std::vector<uint8_t> bytes;
	std::string why;
	if (!bad_build_mint(clip, nullptr, bad_retail_limits(), bytes, &why)) {
		std::fprintf(stderr, "FAIL: %s: %s\n", clip.name.c_str(), why.c_str());
		return false;
	}
	out.push_back(Minted{clip.name + ".bad", std::move(bytes)});
	return true;
}

BadBuildClip make_short() {
	BadBuildClip clip = make_rst();
	clip.name = "rig19_short";
	for (size_t b = 0; b < clip.bones.size(); ++b) {
		clip.bones[b].keys = {turn(static_cast<int>(b) % 3, 1.0 / 16.0), turn(static_cast<int>(b) % 3, 1.0 / 8.0)};
		clip.bones[b].durations = {1, 1};
	}
	return clip;
}

BadBuildClip make_sparse() {
	BadBuildClip clip;
	clip.name = "rig19_sprs";
	clip.flags = BAD_FLAG_LOOP;
	clip.frame_count = 11;
	clip.bones = rig_bones();
	for (size_t b = 0; b < clip.bones.size(); ++b) {
		// Bone b's keys hold 1, 2, .. 2 + b % 3 frames in turn, the last cut to
		// the 12 fence posts (6 or 8 keys a bone).
		uint32_t left = clip.frame_count + 1;
		const uint32_t longest = 2 + static_cast<uint32_t>(b) % 3;
		for (uint32_t k = 0; left > 0; ++k) {
			const uint32_t hold = std::min<uint32_t>(left, 1 + k % longest);
			clip.bones[b].durations.push_back(hold);
			clip.bones[b].keys.push_back(turn(static_cast<int>(b) % 3, (2.0 + k) / 64.0));
			left -= hold;
		}
	}
	standing_events(clip);
	return clip;
}

BadBuildClip make_v0() {
	BadBuildClip clip = make_rst();
	clip.name = "rig19_v0";
	clip.version = 0;
	key_every_bone(clip, [](int b, int k) { return turn(b % 3, (1.0 + k) / 32.0); });
	return clip;
}

BadBuildClip make_b3() {
	BadBuildClip clip = make_rst();
	clip.name = "rig19_b3";
	clip.flags = BAD_FLAG_BIT3;
	key_every_bone(clip, [](int b, int k) { return turn(b % 3, (3.0 + k) / 64.0); });
	return clip;
}

BadBuildClip make_b3lp() {
	BadBuildClip clip = vm_clip("vm40_b3lp", 6, BAD_FLAG_LOOP | BAD_FLAG_BIT3,
			arms_turn(kX, [](int k) { return (k <= 3 ? k : 6 - k) / 32.0; }), no_slide);
	return clip;
}

BadBuildClip make_long() {
	BadBuildClip clip;
	clip.name = "vm40_long";
	clip.flags = BAD_FLAG_LOOP | BAD_FLAG_TRANSLATION;
	clip.frame_count = 4;
	clip.bones = vm_bones();
	for (BadBuildBone &b : clip.bones) {
		for (int k = 0; k < 9; ++k) {
			b.keys.push_back(turn(kZ, k / 64.0));
			b.durations.push_back(1);
		}
		for (uint32_t k = 0; k <= clip.frame_count; ++k) b.translations.push_back(BadBuildVec3{});
	}
	clip.events.assign(clip.frame_count + 1, BadBuildEvent{});
	for (BadBuildEvent &e : clip.events) e.bottom = e.top = kVmStill;
	return clip;
}

const Minted *find_minted(const std::vector<Minted> &out, const char *name) {
	for (const Minted &m : out)
		if (m.name == name) return &m;
	return nullptr;
}

// The bone table's offset (header word 6) and a bone row's address in `bytes`.
uint32_t bone_table(const std::vector<uint8_t> &bytes) {
	return static_cast<uint32_t>(bytes[0x18]) | static_cast<uint32_t>(bytes[0x19]) << 8 |
			static_cast<uint32_t>(bytes[0x1A]) << 16 | static_cast<uint32_t>(bytes[0x1B]) << 24;
}

void put_u32(std::vector<uint8_t> &bytes, size_t at, uint32_t v) {
	for (int k = 0; k < 4; ++k) bytes[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
}

bool mint_quirks(std::vector<Minted> &out) {
	if (!mint_lone(make_short(), out) || !mint_lone(make_sparse(), out) || !mint_lone(make_v0(), out) ||
			!mint_lone(make_b3(), out) || !mint_lone(make_b3lp(), out))
		return false;

	// rig19_junk: the idle, flags 0x9, then the exporter's leftovers in its bone rows.
	const Minted *idle = find_minted(out, "rig19_idle.bad");
	if (idle == nullptr) return false;
	BadFile bf{};
	if (bad_parse_buffer(idle->bytes.data(), idle->bytes.size(), &bf) != 0) return false;
	bf.flags = BAD_FLAG_LOOP | BAD_FLAG_BIT3;
	Minted junk{"rig19_junk.bad", {}};
	const bool wrote = bad_write_buffer(&bf, junk.bytes) == 0;
	bad_free(&bf);
	if (!wrote) return false;
	const uint32_t bones = bone_table(junk.bytes);
	for (uint32_t b = 0; b < 19; ++b) {
		const size_t row = bones + 100u * b;
		size_t len = 0;
		while (junk.bytes[row + len] != 0) ++len;
		for (size_t k = len + 1; k < 32; ++k) junk.bytes[row + k] = static_cast<uint8_t>(0x40 + (b * 7 + k) % 26);
		junk.bytes[row + 32] = 0x11;
		junk.bytes[row + 33] = 0x22;
		junk.bytes[row + 34] = 0x33;
	}
	junk.bytes[bones + 100u * 3 + 35] = 0x5A;                  // bone 3's index byte
	put_u32(junk.bytes, bones + 44, bones + 100u * 19);         // the root's parent address, past the table
	put_u32(junk.bytes, bones + 100u * 2 + 40, bones + 100u * 13); // bone 2's child: its last child, the neck
	put_u32(junk.bytes, bones + 100u * 18 + 40, 8);             // a leaf's child address
	out.push_back(std::move(junk));

	// vm40_long: nine keys on four frames, and translation rows to key 9 + 1.
	if (!mint_lone(make_long(), out)) return false;
	{
		Minted &m = out.back();
		const size_t row_bytes = 12u * 40u;
		// The writer stores rows 0..4 and a pad row (6); M60_1i's rows run to its
		// last key's fence post (10): four more, each repeating the last.
		const std::vector<uint8_t> last(m.bytes.end() - static_cast<std::ptrdiff_t>(row_bytes), m.bytes.end());
		for (int r = 0; r < 4; ++r) m.bytes.insert(m.bytes.end(), last.begin(), last.end());
	}

	// vm40_nopad: the translated reset without its pad row (the writer's last row).
	const Minted *rst = find_minted(out, "vm40_rst.bad");
	if (rst == nullptr) return false;
	Minted nopad{"vm40_nopad.bad", rst->bytes};
	nopad.bytes.resize(nopad.bytes.size() - 12u * 40u);
	out.push_back(std::move(nopad));
	return true;
}

// rig19_idle with `bone`'s every key turned 90 degrees about its local Y,
// cur * (cos 45, 0, sin 45, 0) in (w, x, y, z), normalized as the runtime
// reads a key (anim::bad_channel_quat): with cos 45 = sin 45 = k each part is
// k times a sum, so the turn rounds the same everywhere.
void twist_channel(BadFile &bf, int bone) {
	const double k = std::sqrt(0.5);
	BadChannel &ch = bf.channels[bone];
	for (uint32_t f = 0; f < ch.frame_count; ++f) {
		BadQuaternion &q = ch.rotations[f];
		double w = q.w, x = q.x, y = q.y, z = q.z;
		const double n = std::sqrt(w * w + x * x + y * y + z * z);
		w /= n;
		x /= n;
		y /= n;
		z /= n;
		double ow = k * (w - y), ox = k * (x - z), oy = k * (w + y), oz = k * (z + x);
		const double on = std::sqrt(ow * ow + ox * ox + oy * oy + oz * oz);
		q.w = static_cast<float>(ow / on);
		q.x = static_cast<float>(ox / on);
		q.y = static_cast<float>(oy / on);
		q.z = static_cast<float>(oz / on);
	}
}

bool mint_set(const BadBuildSet &set, std::vector<Minted> &out) {
	std::vector<BadMintedFile> files;
	std::vector<BadBuildProblem> problems;
	if (!bad_build_mint_set(set, set.adm_name, bad_retail_limits(), opennova::anim::adm_slot_index, files, problems)) {
		for (const BadBuildProblem &p : problems) std::fprintf(stderr, "FAIL: %s: %s\n", set.adm_name.c_str(), p.what.c_str());
		return false;
	}
	for (BadMintedFile &f : files) out.push_back(Minted{f.name, std::move(f.bytes)});
	return true;
}

bool mint_all(std::vector<Minted> &out) {
	if (!mint_set(make_set(), out) || !mint_set(make_vm_set(), out) || !mint_quirks(out)) return false;
	// The broken rig from the minted first-person reset.
	for (const Minted &m : out) {
		if (m.name != "vm40_rst.bad") continue;
		BadFile bf{};
		if (bad_parse_buffer(m.bytes.data(), m.bytes.size(), &bf) != 0) return false;
		for (size_t b = 1; b < bf.num_bones; ++b) bf.bones[b].position[1] = bf.bones[b].position[2] = bf.bones[b].position[0];
		Minted broken{"vm40_brk.bad", {}};
		const bool ok = bad_write_buffer(&bf, broken.bytes) == 0;
		bad_free(&bf);
		if (!ok) return false;
		out.push_back(std::move(broken));
		break;
	}
	// The twist from the minted idle: its bone table, header and events as they are.
	for (const Minted &m : out) {
		if (m.name != "rig19_idle.bad") continue;
		BadFile bf{};
		if (bad_parse_buffer(m.bytes.data(), m.bytes.size(), &bf) != 0) return false;
		twist_channel(bf, 5);
		twist_channel(bf, 7);
		Minted twist{"rig19_twist.bad", {}};
		const bool ok = bad_write_buffer(&bf, twist.bytes) == 0;
		bad_free(&bf);
		if (!ok) return false;
		out.push_back(std::move(twist));
		break;
	}
	return true;
}

// The shape the consumers pin, checked on the committed bytes.
void check_facts(const Minted &m) {
	if (m.name == "rig19.adm") {
		opennova::adm::AdmFile table{};
		const bool ok = opennova::adm::adm_parse_buffer(reinterpret_cast<const char *>(m.bytes.data()), m.bytes.size(), &table) == 0;
		expect(ok && table.count == 10 && std::strcmp(table.entries[0].key, "anim_reset") == 0, m.name + ": ten rows, the reset first");
		opennova::adm::adm_free(&table);
		return;
	}
	if (m.name == "vm40.adm") {
		opennova::adm::AdmFile table{};
		const bool ok = opennova::adm::adm_parse_buffer(reinterpret_cast<const char *>(m.bytes.data()), m.bytes.size(), &table) == 0;
		expect(ok && table.count == 9 && std::strcmp(table.entries[4].key, "anim_wpn_reload") == 0,
				m.name + ": nine rows, the reload fifth");
		opennova::adm::adm_free(&table);
		return;
	}
	BadFile bf{};
	if (!expect(bad_parse_buffer(m.bytes.data(), m.bytes.size(), &bf) == 0, m.name + ": the committed bytes parse")) return;
	if (m.name.rfind("vm40_", 0) == 0) {
		expect(bf.version == 1 && bf.bone_count == 40 && (bf.flags & BAD_FLAG_TRANSLATION) != 0 &&
						bf.num_translations == 40u * (bf.frame_count + 1) && bf.num_events == bf.frame_count + 1 &&
						bf.events[0].bottom == bf.events[0].top,
				m.name + ": the 40-bone first-person rig, translated, standing still");
		if (m.name == "vm40_rst.bad") expect(bf.frame_count == 1 && bf.num_translations == 80, m.name + ": one frame, rows 0..1");
		if (m.name == "vm40_swt.bad") expect((bf.flags & BAD_FLAG_BIT3) != 0, m.name + ": flag bit 3");
		if (m.name == "vm40_b3lp.bad") expect(bf.flags == 0xBu, m.name + ": flags 0xB");
		if (m.name == "vm40_long.bad") expect(bf.frame_count == 4 && bf.channels[0].frame_count == 9, m.name + ": nine keys on four frames");
		if (m.name == "vm40_brk.bad") {
			int triplicated = 0;
			for (size_t b = 1; b < bf.num_bones; ++b)
				triplicated += bf.bones[b].position[0] == bf.bones[b].position[1] &&
						bf.bones[b].position[1] == bf.bones[b].position[2];
			expect(triplicated == 39, m.name + ": every child bone X-triplicated");
		}
		bad_free(&bf);
		return;
	}
	expect(bf.version == (m.name == "rig19_v0.bad" ? 0u : 1u) && bf.header_size == 80 && bf.fps == 30 && bf.bone_count == 19 && bf.num_channels == 19 &&
					bf.num_events == bf.frame_count + 1 && bf.num_translations == 0 &&
					std::strcmp(bf.bones[0].name, "BN01 Hips") == 0 && bf.bones[0].parent_index == -1 &&
					bf.bones[14].parent_index == 13 && std::strcmp(bf.bones[14].name, "BN15 Head") == 0,
			m.name + ": the 19-bone person rig");
	if (m.name == "rig19_idle.bad" || m.name == "rig19_twist.bad")
		expect(bf.frame_count == 3 && bf.flags == BAD_FLAG_LOOP && bf.num_events == 4 && bf.channels[0].frame_count == 4 &&
						std::strcmp(bf.bones[5].name, "BN06 R UpperArm") == 0 && std::strcmp(bf.bones[7].name, "BN08 R Thigh") == 0,
				m.name + ": the stock idle's shape");
	if (m.name == "rig19_short.bad")
		expect(bf.frame_count == 2 && bf.channels[0].frame_count == 2, m.name + ": two keys on two frames");
	if (m.name == "rig19_sprs.bad")
		expect(bf.frame_count == 11 && bf.channels[0].frame_count < 12 && bf.channels[1].frame_count != bf.channels[0].frame_count,
				m.name + ": sparse keys, a table a bone");
	if (m.name == "rig19_v0.bad") expect(bf.events[0].trigger == -1, m.name + ": no trigger word");
	if (m.name == "rig19_b3.bad") expect(bf.flags == BAD_FLAG_BIT3, m.name + ": flags 0x8");
	if (m.name == "rig19_junk.bad")
		expect(bf.flags == 0x9u && bf.bones[0].parent_index == -1 && bf.bones[3].parent_index == 2, m.name + ": the links still read");
	if (m.name == "rig19_walk.bad")
		expect(bf.frame_count == 30 && bf.events[0].trigger == 0x1 && bf.events[15].trigger == 0x2 && bf.events[8].trigger == 0x20,
				m.name + ": the footsteps and the foley bit");
	bad_free(&bf);
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/anim";
	std::vector<Minted> minted;
	if (!expect(mint_all(minted), "the rig19 set mints")) return 1;
	for (const Minted &m : minted) {
		const std::string path = dir + "/" + m.name;
		if (write_mode) {
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(m.bytes.data()), static_cast<std::streamsize>(m.bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), m.bytes.size());
			check_facts(m);
			continue;
		}
		std::vector<uint8_t> committed;
		if (!expect(test_io::read_file(path, committed), path + " missing; run with --write")) continue;
		if (test_io::is_lfs_pointer(committed)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
			continue;
		}
		expect(committed == m.bytes, path + " differs from the generator output; regenerate with --write");
		check_facts(Minted{m.name, committed});
		std::printf("%-20s %6zu bytes\n", m.name.c_str(), committed.size());
	}
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/anim/rig19* and vm40* byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
