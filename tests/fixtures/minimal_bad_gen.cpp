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
//                                  and head; every key a small turn of its own
//                                  (the last repeating the first, a seamless
//                                  loop), so no bone's bind is the identity
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
//   fixtures/anim/rig19_dthb.bad   30 frames, one-shot
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
// Every turn is a rational rotation (a quaternion (t * axis, 1) normalized: t
// the tangent of half the angle), so the bytes come out the same on every
// platform. Default: rebuild every file in memory and byte-compare the
// committed copies; `--write` (re)writes them. Every run re-reads the committed
// bytes and checks the shape the consumers pin.
#include "fixtures/person_rig.h"

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_write.h>
#include <runtime/anim/adm_clip_index.h>

#include "common/file_io.h"
#include "common/test_paths.h"

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
	// + k) / 128 on key k (7 to 14 degrees), the last key repeating the first.
	key_every_bone(clip, [](int b, int k) {
		const int step = k == 3 ? 0 : k;
		return turn(b % 3, (8.0 + 2.0 * (b % 5) + step) / 128.0);
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
BadBuildClip make_death(const char *name, int axis, double sign) {
	BadBuildClip clip;
	clip.name = name;
	clip.frame_count = 30;
	clip.bones = rig_bones();
	// Every bone takes the hips' turn: the body tips whole, to t = 0.4 (43.6
	// degrees) at the last frame.
	key_every_bone(clip, [axis, sign](int, int k) { return turn(axis, sign * 0.4 * k / 30.0); });
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
	for (size_t k = 0; k < clip.events.size(); ++k) {
		BadBuildEvent &e = clip.events[k];
		e.velocity = BadBuildVec3{1.0 / 16.0, 0.0, 0.0};
		e.trigger = (k == 0 || k == 30) ? 0x1 : (k == 15 ? 0x2 : (k == 8 ? 0x20 : 0));
	}
	return clip;
}

BadBuildSet make_set() {
	BadBuildSet set;
	set.adm_name = "rig19.adm";
	set.clips = {make_rst(), make_idle(), make_crouch(), make_reload("rig19_rld1", kY), make_reload("rig19_rld2", kZ),
		make_death("rig19_dthf", kY, 1.0), make_death("rig19_dthr", kX, 1.0), make_death("rig19_dthb", kY, -1.0),
		make_death("rig19_dthl", kX, -1.0), make_walk()};
	set.rows = {{"anim_reset", {"rig19_rst"}}, {"anim_idle", {"rig19_idle"}}, {"anim_idle_crouch", {"rig19_crch"}},
		{"anim_walk_forward", {"rig19_walk"}}, {"anim_reload", {"rig19_rld1"}}, {"anim_reload2", {"rig19_rld2"}},
		{"anim_death_bullet_torso_forward", {"rig19_dthf"}}, {"anim_death_bullet_torso_right", {"rig19_dthr"}},
		{"anim_death_bullet_torso_back", {"rig19_dthb"}}, {"anim_death_bullet_torso_left", {"rig19_dthl"}}};
	return set;
}

// The 40-bone first-person rig (mission axes, the eye at the origin): 0 the
// root; per arm (right from 1, left from 19) the upper arm, the forearm, the
// hand and five fingers of three joints; 37 the receiver, 38 the magazine and
// 39 the bolt under it.
constexpr int kVmBones = 40;
constexpr int kVmReceiver = 37, kVmMagazine = 38, kVmBolt = 39;

std::vector<BadBuildBone> vm_bones() {
	std::vector<BadBuildBone> bones(kVmBones);
	char name[32];
	bones[0].name = "BN01 Root";
	for (int side = 0; side < 2; ++side) {
		const int base = 1 + side * 18;
		const double y = side == 0 ? -1.0 : 1.0;
		const char *letter = side == 0 ? "R" : "L";
		const double reach = side == 0 ? 0.0 : 0.125;
		const BadBuildVec3 joints[3] = {{0.0, 0.25 * y, -0.25}, {0.25 + reach, 0.1875 * y, -0.3125},
			{0.5 + reach, 0.125 * y, -0.25}};
		const char *roles[3] = {"UpperArm", "Forearm", "Hand"};
		for (int j = 0; j < 3; ++j) {
			BadBuildBone &b = bones[static_cast<size_t>(base + j)];
			std::snprintf(name, sizeof(name), "BN%02d %s %s", base + j + 1, letter, roles[j]);
			b.name = name;
			b.parent = j == 0 ? 0 : base + j - 1;
			b.pivot = joints[j];
			b.length = 0.25;
		}
		const BadBuildVec3 &hand = joints[2];
		for (int f = 0; f < 5; ++f) {
			for (int k = 0; k < 3; ++k) {
				const int index = base + 3 + f * 3 + k;
				BadBuildBone &b = bones[static_cast<size_t>(index)];
				std::snprintf(name, sizeof(name), "BN%02d %s Finger%d%d", index + 1, letter, f + 1, k + 1);
				b.name = name;
				b.parent = k == 0 ? base + 2 : index - 1;
				b.pivot = BadBuildVec3{hand.x + 0.0625 + 0.03125 * k, hand.y + 0.015625 * (f - 2) * y, hand.z};
				b.length = 0.03125;
			}
		}
	}
	const BadBuildVec3 gun[3] = {{0.375, -0.125, -0.1875}, {0.4375, -0.125, -0.25}, {0.3125, -0.125, -0.15625}};
	const char *gun_names[3] = {"BN38 Receiver", "BN39 Magazine", "BN40 Bolt"};
	for (int g = 0; g < 3; ++g) {
		BadBuildBone &b = bones[static_cast<size_t>(kVmReceiver + g)];
		b.name = gun_names[g];
		b.parent = g == 0 ? 0 : kVmReceiver;
		b.pivot = gun[g];
		b.length = 0.125;
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

struct Minted {
	std::string name;
	std::vector<uint8_t> bytes;
};

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
	if (!mint_set(make_set(), out) || !mint_set(make_vm_set(), out)) return false;
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
		bad_free(&bf);
		return;
	}
	expect(bf.version == 1 && bf.header_size == 80 && bf.fps == 30 && bf.bone_count == 19 && bf.num_channels == 19 &&
					bf.num_events == bf.frame_count + 1 && bf.num_translations == 0 &&
					std::strcmp(bf.bones[0].name, "BN01 Hips") == 0 && bf.bones[0].parent_index == -1 &&
					bf.bones[14].parent_index == 13 && std::strcmp(bf.bones[14].name, "BN15 Head") == 0,
			m.name + ": the 19-bone person rig");
	if (m.name == "rig19_idle.bad" || m.name == "rig19_twist.bad")
		expect(bf.frame_count == 3 && bf.flags == BAD_FLAG_LOOP && bf.num_events == 4 && bf.channels[0].frame_count == 4 &&
						std::strcmp(bf.bones[5].name, "BN06 R UpperArm") == 0 && std::strcmp(bf.bones[7].name, "BN08 R Thigh") == 0,
				m.name + ": the stock idle's shape");
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
