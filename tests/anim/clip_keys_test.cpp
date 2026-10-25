// A rig's clip keys sampled at their first read (anim::Clip::key, anim::ClipSampler),
// against the table the load used to bake whole. Retail pins a rig's skeleton once and builds
// a playing channel's bone matrices from its .bad when a bone builder reads them; it keeps no
// table of every key of every clip [orig: AnimMap_RegisterEntity @0x40BB60;
// AnimChannel_ComputeBoneMatrices @0x410DA0, called by Entity_BuildBoneTransformMatrices
// @0x4B1290 (the call @0x4B1388) and BoneAnim_BuildWorldMatrices @0x40C400 (the call
// @0x40C421)]. The pins:
//   1. sample_clip (now a ClipSampler and every key in order) is bit-identical to the former
//      whole-table sampler, carried below verbatim as the oracle (legacy_sample_clip), on every
//      minted fixture .bad and every mode: plain, over the skeleton's translation gate, the
//      composed bind, the model table, both, and the skeleton's shared rests;
//   2. a rig's load (SkeletalClips::load_from_adm over the minted rig19 person and vm40
//      viewmodel sets, with and without the model table) samples no key: every clip holds its
//      key slots empty;
//   3. each key read through Clip::key is bit-identical to the oracle's key over the same rig
//      (its shared rests, its skeleton .bad, its table parents), read last to first;
//   4. a pose evaluation samples its window's keys alone (two at most, of its own clip), and its
//      pose is bit-identical to the oracle table's.
#include "common/file_io.h"
#include "common/rig19_set.h"
#include "common/temp_dir.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/bad/bad.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity_pose.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::anim;
using namespace opennova::bad;

namespace {

int failures = 0;
bool expect(bool cond, const std::string &msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
	++failures;
	return false;
}

// The former sampler's helpers and body (engine/runtime/anim/anim_sample.cpp ahead of the
// ClipSampler split), its comments dropped: the oracle every key is pinned against.
constexpr Quat kIdentityQuat = {1.0f, 0.0f, 0.0f, 0.0f};
constexpr Vec3 kZeroVec = {0.0f, 0.0f, 0.0f};
Vec3 vec_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vec_sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Quat legacy_bone_world_rot(const BadChannel &ch, uint32_t tick) {
	if (ch.rotations == nullptr || ch.frame_count == 0) {
		return kIdentityQuat;
	}
	const uint32_t count = ch.frame_count;
	const auto kf = [&](uint32_t i) {
		const BadQuaternion &r = ch.rotations[i];
		return bad_channel_quat(r.x, r.y, r.z, r.w);
	};
	if (count == 1) {
		return kf(0);
	}
	uint32_t acc = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t dur = (ch.frame_lengths != nullptr && ch.frame_lengths[i] > 0) ? ch.frame_lengths[i] : 1u;
		if (acc + dur > tick) {
			const float blend = static_cast<float>(tick - acc) / static_cast<float>(dur);
			const uint32_t next = (i + 1 < count) ? i + 1 : 0u;
			return quat_slerp(kf(i), kf(next), blend);
		}
		acc += dur;
	}
	return kf(count - 1);
}

Clip legacy_sample_clip(const BadFile &bad, const std::vector<Vec3> &shared_rest_origins, bool model_bind,
				 const BadFile *bind_source, const std::vector<int> &model_parents) {
	Clip clip;
	clip.fps = bad.fps;
	clip.flags = bad.flags;
	clip.frame_count = bad.frame_count;
	clip.file_bones = bad.bone_count;

	const size_t bad_bone_count = bad.num_bones;
	const BadFile &gate_source = (bind_source != nullptr) ? *bind_source : bad;
	const bool translated = (bad.flags & 0x02u) != 0 && (gate_source.flags & 0x02u) != 0;
	const bool model_table =
			!model_parents.empty() && model_parents.size() == shared_rest_origins.size();
	const size_t bone_count = model_table ? model_parents.size() : bad_bone_count;
	const bool use_shared = model_table || shared_rest_origins.size() == bone_count;

	clip.bones.resize(bone_count);
	std::vector<Vec3> rest_origins(bone_count, kZeroVec);
	const BadFile &bind_bad = (model_bind && bind_source != nullptr) ? *bind_source : bad;
	std::vector<Quat> bind_rot;
	if (model_bind) {
		bind_rot.resize(bone_count, kIdentityQuat);
	}
	for (size_t b = 0; b < bone_count; ++b) {
		const BadBone *bone = (b < bad_bone_count) ? &bad.bones[b] : nullptr;
		clip.bones[b].name = (bone != nullptr) ? bone->name : "";
		int parent = model_table ? model_parents[b]
								 : ((bone != nullptr) ? bone->parent_index : -1);
		if (parent < 0 || static_cast<size_t>(parent) >= bone_count ||
				static_cast<size_t>(parent) == b) {
			parent = -1;
		}
		clip.bones[b].parent_index = parent;
		const Vec3 origin = use_shared
				? shared_rest_origins[b]
				: Vec3{bone->position[0], bone->position[1], bone->position[2]};
		if (model_bind) {
			for (int k = 0; k < 9; ++k) {
				clip.bones[b].rest_rotation[k] = (k % 4 == 0) ? 1.0f : 0.0f;
			}
			if (b < bind_bad.num_bones) {
				bind_rot[b] = mat3_to_quat(bind_bad.bones[b].rotation);
			}
		} else if (bone != nullptr) {
			for (int k = 0; k < 9; ++k) {
				clip.bones[b].rest_rotation[k] = bone->rotation[k];
			}
		}
		rest_origins[b] = origin;
		clip.bones[b].rest_position[0] = origin.x;
		clip.bones[b].rest_position[1] = origin.y;
		clip.bones[b].rest_position[2] = origin.z;
	}
	std::vector<Vec3> rest_pivots(bone_count, kZeroVec);
	for (size_t b = 0; b < bone_count; ++b) {
		Vec3 pivot = rest_origins[b];
		int parent = clip.bones[b].parent_index;
		for (size_t steps = 0; parent >= 0 && steps < bone_count; ++steps) {
			pivot = vec_add(pivot, rest_origins[static_cast<size_t>(parent)]);
			parent = clip.bones[static_cast<size_t>(parent)].parent_index;
		}
		rest_pivots[b] = pivot;
	}

	clip.frames.resize(clip.frame_count + 1);
	std::vector<Quat> raw_rotation(bone_count, kIdentityQuat);
	for (uint32_t f = 0; f <= clip.frame_count; ++f) {
		std::vector<BoneSample> &frame = clip.frames[f];
		frame.resize(bone_count);

		for (size_t b = 0; b < bone_count; ++b) {
			Quat world_rot = kIdentityQuat;
			if (b < bad.num_channels) {
				world_rot = legacy_bone_world_rot(bad.channels[b], f);
			} else if (model_table) {
				raw_rotation[b] = (bad.num_channels > 0) ? raw_rotation[0] : kIdentityQuat;
				continue;
			}
			if (model_bind) {
				world_rot = quat_normalize(quat_mul(bind_rot[b], world_rot));
			}
			raw_rotation[b] = world_rot;
		}

		const auto place = [&](size_t b, const Vec3 &translation) {
			const int parent = clip.bones[b].parent_index;
			Vec3 carried = rest_origins[b];
			if (parent >= 0 && static_cast<size_t>(parent) < b) {
				const BoneSample &p = frame[static_cast<size_t>(parent)];
				carried = vec_add(p.world_position, quat_rotate(p.world_rotation, rest_origins[b]));
			} else if (parent >= 0) {
				carried = quat_rotate(raw_rotation[static_cast<size_t>(parent)], rest_pivots[b]);
			}
			frame[b].world_position = vec_add(carried, translation);
		};

		for (size_t b = 0; b < bone_count; ++b) {
			frame[b].world_rotation = raw_rotation[b];
			if (b >= bad.num_channels && model_table) {
				place(b, kZeroVec);
				continue;
			}

			Vec3 translation = kZeroVec;
			if (translated && bad.translations != nullptr && b < bad_bone_count) {
				const size_t rows = bad.num_translations / bad_bone_count;
				if (rows > 0) {
					const size_t row = f < rows ? static_cast<size_t>(f) : rows - 1;
					const float *t = bad.translations[row * bad_bone_count + b];
					translation = {t[0], t[1], t[2]};
				}
			}
			place(b, translation);
		}

		for (size_t b = 0; b < bone_count; ++b) {
			const int parent = clip.bones[b].parent_index;
			BoneSample &out = frame[b];
			if (parent < 0) {
				out.local_rotation = quat_normalize(out.world_rotation);
				out.local_position = out.world_position;
			} else {
				const BoneSample &p = frame[static_cast<size_t>(parent)];
				out.local_rotation = quat_normalize(quat_mul(quat_inv(p.world_rotation), out.world_rotation));
				out.local_position = quat_rotate(quat_inv(p.world_rotation),
												 vec_sub(out.world_position, p.world_position));
			}
		}
	}

	return clip;
}

bool same_bits(const void *a, const void *b, size_t size) { return std::memcmp(a, b, size) == 0; }

bool same_key(const std::vector<BoneSample> &a, const std::vector<BoneSample> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (!same_bits(&a[i], &b[i], sizeof(BoneSample))) return false;
	return true;
}

bool same_header(const Clip &a, const Clip &b) {
	if (a.fps != b.fps || a.flags != b.flags || a.frame_count != b.frame_count ||
			a.file_bones != b.file_bones || a.bones.size() != b.bones.size())
		return false;
	for (size_t i = 0; i < a.bones.size(); ++i) {
		const ClipBone &x = a.bones[i];
		const ClipBone &y = b.bones[i];
		if (x.name != y.name || x.parent_index != y.parent_index ||
				!same_bits(x.rest_rotation, y.rest_rotation, sizeof(x.rest_rotation)) ||
				!same_bits(x.rest_position, y.rest_position, sizeof(x.rest_position)))
			return false;
	}
	return true;
}

// A table clip against the oracle's: the header and every key.
bool same_table(const Clip &clip, const Clip &oracle) {
	if (!same_header(clip, oracle) || clip.key_count() != oracle.frames.size()) return false;
	for (size_t f = 0; f < clip.key_count(); ++f)
		if (!same_key(clip.key(f), oracle.frames[f])) return false;
	return true;
}

std::shared_ptr<BadFile> read_bad(const std::string &name) {
	auto file = std::shared_ptr<BadFile>(new BadFile{}, [](BadFile *f) {
		bad_free(f);
		delete f;
	});
	std::vector<uint8_t> bytes;
	if (!test_io::read_file(test_paths_fixture("anim/" + name), bytes) ||
			bad_parse_buffer(bytes.data(), bytes.size(), file.get()) != 0)
		return nullptr;
	return file;
}

const std::vector<std::string> &vm40_files() {
	static const std::vector<std::string> kFiles = {"vm40.adm", "vm40_rst.bad", "vm40_i.bad",
		"vm40_f.bad", "vm40_r.bad", "vm40_e.bad", "vm40_swt.bad", "vm40_swf.bad", "vm40_swr.bad"};
	return kFiles;
}

bool stage_vm40(const std::string &dir) {
	std::vector<uint8_t> bytes;
	if (!test_io::read_file(test_paths_fixture("threedi/synth/vm40.3di"), bytes) ||
			!test_io::write_file(dir + "/vm40.3di", bytes))
		return false;
	for (const std::string &name : vm40_files())
		if (!test_io::read_file(test_paths_fixture("anim/" + name), bytes) ||
				!test_io::write_file(dir + "/" + name, bytes))
			return false;
	return true;
}

size_t sampled_keys(const Clip &clip) {
	size_t n = 0;
	for (const std::vector<BoneSample> &slot : clip.frames) n += slot.empty() ? 0 : 1;
	return n;
}

size_t sampled_keys(const SkeletalClips &rig) {
	size_t n = 0;
	for (const SkeletalClips::LoadedClip &clip : rig.clips()) n += sampled_keys(clip.clip);
	return n;
}

// The rig's shared rests (what its load hands every clip's sampler): each row's rest origin.
std::vector<Vec3> rig_rests(const SkeletalClips &rig) {
	std::vector<Vec3> rests;
	for (const ClipBone &bone : rig.bones())
		rests.push_back({bone.rest_position[0], bone.rest_position[1], bone.rest_position[2]});
	return rests;
}

// Pins 2..4 over one rig: its load, then every clip's keys against the oracle, and a fresh
// load's evaluations.
void check_rig(const assets::AssetStore &assets, const std::string &tag, const std::string &adm,
		const std::string &skeleton_name, const std::vector<Vec3> &origins,
		const std::vector<int> &parents) {
	SkeletalClips rig;
	if (!expect(rig.load_from_adm(&assets, adm, origins, parents), tag + ": the rig loads")) return;
	expect(!rig.clips().empty(), tag + ": the rig registers clips");
	for (const SkeletalClips::LoadedClip &clip : rig.clips()) {
		expect(clip.clip.sampler != nullptr, tag + ": " + clip.key + " carries its sampler");
		expect(clip.clip.key_count() == static_cast<size_t>(clip.clip.frame_count) + 1,
				tag + ": " + clip.key + " holds frame_count + 1 key slots");
	}
	expect(sampled_keys(rig) == 0, tag + ": the load samples no key");

	const auto skeleton = assets.bone_animation(skeleton_name);
	if (!expect(skeleton != nullptr, tag + ": the skeleton .bad reads")) return;
	const std::vector<Vec3> rests = rig_rests(rig);
	const bool table = !parents.empty() && parents.size() == origins.size();
	const std::vector<int> rig_parents = table ? parents : std::vector<int>{};
	std::vector<Clip> oracles;
	for (const SkeletalClips::LoadedClip &clip : rig.clips()) {
		const auto file = assets.bone_animation(clip.source.file);
		if (!expect(file != nullptr, tag + ": " + clip.source.file + " reads")) {
			oracles.emplace_back();
			continue;
		}
		oracles.push_back(legacy_sample_clip(*file, rests, false, skeleton.get(), rig_parents));
		const Clip &oracle = oracles.back();
		expect(same_header(clip.clip, oracle), tag + ": " + clip.key + "'s header matches the oracle's");
		// The keys read last to first: each one's sample stands alone.
		bool keys_match = clip.clip.key_count() == oracle.frames.size();
		for (size_t f = clip.clip.key_count(); keys_match && f-- > 0;)
			keys_match = same_key(clip.clip.key(f), oracle.frames[f]);
		expect(keys_match, tag + ": every key of " + clip.key + " is the oracle's, bit for bit");
	}

	// A fresh load: each evaluation samples its window's keys alone, and poses as the oracle does.
	SkeletalClips fresh;
	if (!expect(fresh.load_from_adm(&assets, adm, origins, parents), tag + ": the rig loads again")) return;
	for (size_t i = 0; i < fresh.clips().size() && i < oracles.size(); ++i) {
		const SkeletalClips::LoadedClip &clip = fresh.clips()[i];
		const Clip &oracle = oracles[i];
		if (clip.clip.fps == 0 || clip.clip.frame_count == 0 || oracle.frames.empty()) continue;
		const double frame_seconds = 1.0 / static_cast<double>(clip.clip.fps);
		const double playheads[] = {0.0, 0.5 * frame_seconds, 1.25 * frame_seconds,
				(clip.clip.frame_count - 0.5) * frame_seconds, 1000.0};
		for (const double seconds : playheads) {
			const size_t before = sampled_keys(fresh);
			const size_t clip_before = sampled_keys(clip.clip);
			std::vector<PoseBone> lazy, eager;
			eval_clip_pose(clip.clip, seconds, lazy);
			eval_clip_pose(oracle, seconds, eager);
			const size_t grown = sampled_keys(fresh) - before;
			expect(grown <= 2 && sampled_keys(clip.clip) - clip_before == grown,
					tag + ": an evaluation of " + clip.key + " samples its window's keys alone");
			expect(lazy.size() == eager.size() &&
							(lazy.empty() ||
									same_bits(lazy.data(), eager.data(), lazy.size() * sizeof(PoseBone))),
					tag + ": " + clip.key + "'s pose is the oracle table's, bit for bit");
		}
	}
}

// The costs the change moves, printed (an instrument; no assertion): the former load's whole
// table over every clip, the load now, a first pose (its window's two keys sampled) and the same
// pose again (both keys kept).
void print_costs(const assets::AssetStore &assets, const std::string &tag, const std::string &adm,
		const std::string &skeleton_name, const std::vector<Vec3> &origins,
		const std::vector<int> &parents) {
	using Clock = std::chrono::steady_clock;
	const auto us = [](Clock::time_point a, Clock::time_point b) {
		return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
	};
	const Clock::time_point load_start = Clock::now();
	SkeletalClips rig;
	if (!rig.load_from_adm(&assets, adm, origins, parents) || rig.clips().empty()) return;
	const Clock::time_point load_end = Clock::now();
	const auto skeleton = assets.bone_animation(skeleton_name);
	if (skeleton == nullptr) return;
	const std::vector<Vec3> rests = rig_rests(rig);
	const bool table = !parents.empty() && parents.size() == origins.size();
	const std::vector<int> rig_parents = table ? parents : std::vector<int>{};
	size_t keys = 0;
	const Clock::time_point bake_start = Clock::now();
	for (const SkeletalClips::LoadedClip &clip : rig.clips())
		if (const auto file = assets.bone_animation(clip.source.file))
			keys += legacy_sample_clip(*file, rests, false, skeleton.get(), rig_parents).frames.size();
	const Clock::time_point bake_end = Clock::now();
	const SkeletalClips::LoadedClip &first = rig.clips().front();
	const double seconds = first.clip.fps > 0 ? 0.5 / static_cast<double>(first.clip.fps) : 0.0;
	std::vector<PoseBone> pose;
	const Clock::time_point pose_start = Clock::now();
	eval_clip_pose(first.clip, seconds, pose);
	const Clock::time_point pose_mid = Clock::now();
	eval_clip_pose(first.clip, seconds, pose);
	const Clock::time_point pose_end = Clock::now();
	std::printf("clip_keys: %s: former whole table %lld us (%zu clips, %zu keys); load now %lld us; "
			"first pose %lld us; kept pose %lld us\n",
			tag.c_str(), us(bake_start, bake_end), rig.clips().size(), keys, us(load_start, load_end),
			us(pose_start, pose_mid), us(pose_mid, pose_end));
}

struct FixtureSet {
	const char *skeleton;
	const char *model;
	std::vector<std::string> clips;
};

} // namespace

int main() {
	// --- 1. sample_clip against the oracle on every minted .bad, every mode.
	const std::vector<FixtureSet> sets = {
		{"rig19_rst.bad", "threedi/synth/person.3di",
			{"rig19_rst.bad", "rig19_idle.bad", "rig19_crch.bad", "rig19_walk.bad", "rig19_rld1.bad",
				"rig19_rld2.bad", "rig19_dthf.bad", "rig19_dthr.bad", "rig19_dthb.bad", "rig19_dthl.bad",
				"rig19_b3.bad", "rig19_junk.bad", "rig19_short.bad", "rig19_sprs.bad", "rig19_twist.bad",
				"rig19_v0.bad", "idle.bad", "walk.bad"}},
		{"vm40_rst.bad", "threedi/synth/vm40.3di",
			{"vm40_rst.bad", "vm40_i.bad", "vm40_f.bad", "vm40_r.bad", "vm40_e.bad", "vm40_swt.bad",
				"vm40_swf.bad", "vm40_swr.bad", "vm40_b3lp.bad", "vm40_brk.bad", "vm40_long.bad",
				"vm40_nopad.bad"}},
	};
	for (const FixtureSet &set : sets) {
		const auto skeleton = read_bad(set.skeleton);
		if (!expect(skeleton != nullptr, std::string(set.skeleton) + " parses")) continue;
		const assets::Model model = assets::read_model_file(test_paths_fixture(set.model));
		std::vector<Vec3> origins;
		std::vector<int> parents;
		if (!expect(model != nullptr && world::model_bone_table(*model, origins, parents),
					std::string(set.model) + " carries a bone table"))
			continue;
		std::vector<Vec3> skeleton_rests;
		for (size_t b = 0; b < skeleton->num_bones; ++b)
			skeleton_rests.push_back({skeleton->bones[b].position[0], skeleton->bones[b].position[1],
					skeleton->bones[b].position[2]});
		const std::vector<Vec3> zero_rest = {{0.0f, 0.0f, 0.0f}};
		for (const std::string &name : set.clips) {
			const auto file = read_bad(name);
			if (!expect(file != nullptr, name + " parses")) continue;
			expect(same_table(sample_clip(*file), legacy_sample_clip(*file, {}, false, nullptr, {})),
					name + ": plain");
			expect(same_table(sample_clip(*file, {}, false, skeleton.get()),
						   legacy_sample_clip(*file, {}, false, skeleton.get(), {})),
					name + ": over the skeleton's translation gate");
			expect(same_table(sample_clip(*file, zero_rest, true, skeleton.get()),
						   legacy_sample_clip(*file, zero_rest, true, skeleton.get(), {})),
					name + ": the composed bind");
			expect(same_table(sample_clip(*file, origins, false, skeleton.get(), parents),
						   legacy_sample_clip(*file, origins, false, skeleton.get(), parents)),
					name + ": the model table");
			expect(same_table(sample_clip(*file, origins, true, skeleton.get(), parents),
						   legacy_sample_clip(*file, origins, true, skeleton.get(), parents)),
					name + ": the model table under the composed bind");
			expect(same_table(sample_clip(*file, skeleton_rests, false, skeleton.get()),
						   legacy_sample_clip(*file, skeleton_rests, false, skeleton.get(), {})),
					name + ": the skeleton's shared rests");
		}
	}

	// --- 2..4. The rigs' loads and their keys.
	test_temp::TempDir root("opennova_clip_keys");
	if (!expect(rig19::stage(root.root()) && stage_vm40(root.root()), "the rigs stage")) return 1;
	ResourceIndex index;
	if (!expect(index.scan(root.root(), std::string(), VfsMountMode::LooseOnly), "the staged rigs mount"))
		return 1;
	assets::AssetStore assets{&index};
	struct RigCase {
		const char *graphic;
		const char *adm;
		const char *skeleton;
	};
	const RigCase rigs[] = {{"person", "rig19.adm", "rig19_rst"}, {"vm40", "vm40.adm", "vm40_rst"}};
	for (const RigCase &rig : rigs) {
		const assets::Model model = assets.model(rig.graphic);
		std::vector<Vec3> origins;
		std::vector<int> parents;
		if (!expect(model != nullptr && world::model_bone_table(*model, origins, parents),
					std::string(rig.graphic) + " mounts with a bone table"))
			continue;
		check_rig(assets, std::string(rig.adm) + " over the model table", rig.adm, rig.skeleton, origins,
				parents);
	}
	check_rig(assets, "rig19.adm without a table", "rig19.adm", "rig19_rst", {}, {});
	for (const RigCase &rig : rigs) {
		const assets::Model model = assets.model(rig.graphic);
		std::vector<Vec3> origins;
		std::vector<int> parents;
		if (model != nullptr && world::model_bone_table(*model, origins, parents))
			print_costs(assets, rig.adm, rig.adm, rig.skeleton, origins, parents);
	}

	if (failures != 0) {
		std::fprintf(stderr, "clip_keys: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("clip_keys: ok\n");
	return 0;
}
