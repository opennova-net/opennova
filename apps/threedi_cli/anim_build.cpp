// opennova-3di anim build: read the `.o3a` clip-set text a DCC exporter writes
// (the grammar is docs/anim/o3a-scene-format.md) into the engine's clip
// construction seam (formats/bad/bad_build.h) and serialize every clip through
// the writer, so a shipped `.bad` is produced by the same writer the fixtures
// are (ADR 0003). The text carries rotations, pivots, translations and event
// velocities in MISSION axes (x forward, y left, z up); the clip-frame
// conversion is the seam's, never the exporter's.

#include "anim_cli.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>
#include <runtime/anim/adm_clip_index.h>

#include "scene_text.h"
#include "threedi_cli.h"

using namespace opennova::bad;

namespace threedi_cli {

namespace {

struct Parser {
	std::string path;
	int line = 0;
	std::vector<std::string> errors;
	// The line each clip opens on, in set order: a seam refusal names it.
	std::vector<int> clip_lines;

	void error(const std::string &what) { error_at(line, what); }
	void error_at(int at, const std::string &what) {
		errors.push_back(path + ":" + std::to_string(at) + ": " + what);
	}
};

constexpr long long kWordMax = 0xFFFFFFFFll;

// Per bone, which of its keys stated a duration: a bone gives one on every
// key or on none.
struct BoneKeys {
	size_t clip = 0;
	size_t bone = 0;
	int line = 0;
	size_t with = 0;
	size_t without = 0;
};

void parse_set(Parser &ps, std::istream &file, BadBuildSet &set) {
	std::string raw;
	bool header = false;
	BadBuildClip *clip = nullptr;
	BadBuildBone *bone = nullptr;
	std::vector<BoneKeys> bone_keys;
	while (std::getline(file, raw)) {
		++ps.line;
		// The text's shared grammar (scene_text.h), finite numbers only: a clip
		// holds no nan or inf anywhere.
		SceneLine in(strip_comment(raw), SceneNumbers::finite);
		if (!in.bad.empty()) {
			ps.error(in.bad);
			continue;
		}
		if (in.tokens.empty()) continue;
		const std::string key = in.key();

		if (!header) {
			long long version = 0;
			if (key != "o3a" || !in.integer(version, 1, 1) || in.more()) {
				ps.error("a clip set starts with `o3a 1`");
				return;
			}
			header = true;
			continue;
		}

		if (key == "adm") {
			if (!in.name(set.adm_name) || set.adm_name.empty() || in.more())
				ps.error("adm needs one name");
		} else if (key == "row") {
			BadBuildRow row;
			if (!in.name(row.key) || row.key.empty()) {
				ps.error("row needs a slot key");
				continue;
			}
			std::string variant;
			bool whole = true;
			while (whole && in.more()) {
				whole = in.name(variant) && !variant.empty();
				if (whole) row.variants.push_back(variant);
			}
			if (!whole) {
				ps.error("row '" + row.key + "' names an empty clip");
				continue;
			}
			if (row.variants.empty()) ps.error("row '" + row.key + "' names no clip");
			set.rows.push_back(row);
		} else if (key == "clip") {
			set.clips.push_back(BadBuildClip{});
			ps.clip_lines.push_back(ps.line);
			clip = &set.clips.back();
			bone = nullptr;
			if (!in.name(clip->name) || clip->name.empty() || in.more())
				ps.error("clip needs one name");
		} else if (clip == nullptr) {
			ps.error("`" + key + "` before any clip");
		} else if (key == "fps" || key == "frames" || key == "version" || key == "flags") {
			long long value = 0;
			if (!in.integer(value, 0, kWordMax) || in.more()) {
				ps.error(key + " needs one whole number from 0 to 0xffffffff");
				continue;
			}
			if (key == "fps") clip->fps = static_cast<uint32_t>(value);
			if (key == "frames") clip->frame_count = static_cast<uint32_t>(value);
			if (key == "version") clip->version = static_cast<uint32_t>(value);
			if (key == "flags") clip->flags = static_cast<uint32_t>(value);
		} else if (key == "bone") {
			long long parent = 0;
			double pivot[3];
			double length = 0.0;
			std::string name;
			if (!in.integer(parent, -1, 0x7FFFFFFFll) || !in.numbers(pivot, 3) || !in.number(length) ||
					!in.name(name) || in.more()) {
				ps.error("bone needs a parent, a pivot, a length and a name");
				continue;
			}
			BadBuildBone row;
			row.parent = static_cast<int>(parent);
			row.pivot = BadBuildVec3{pivot[0], pivot[1], pivot[2]};
			row.length = length;
			row.name = name;
			clip->bones.push_back(row);
			bone = &clip->bones.back();
			bone_keys.push_back(BoneKeys{set.clips.size() - 1, clip->bones.size() - 1, ps.line});
		} else if (key == "k" || key == "tr" || key == "bonepos") {
			if (bone == nullptr) {
				ps.error("`" + key + "` before any bone");
				continue;
			}
			if (key == "k") {
				double q[4];
				if (!in.numbers(q, 4)) {
					ps.error("k needs four quaternion components");
					continue;
				}
				// A duration states how long the key holds; a bone that gives
				// one gives it on every key, and keeps a table only then.
				long long duration = 0;
				const bool stated = in.more();
				if (stated && (!in.integer(duration, 1, 0xFFFF) || in.more())) {
					ps.error("a key duration is one whole number from 1 to 65535");
					continue;
				}
				bone->keys.push_back(BadBuildQuat{q[0], q[1], q[2], q[3]});
				if (stated) {
					bone->durations.push_back(static_cast<uint16_t>(duration));
					++bone_keys.back().with;
				} else {
					++bone_keys.back().without;
				}
			} else if (key == "tr") {
				double t[3];
				if (!in.numbers(t, 3) || in.more()) {
					ps.error("tr needs three components");
					continue;
				}
				bone->translations.push_back(BadBuildVec3{t[0], t[1], t[2]});
			} else {
				double p[3];
				if (!in.numbers(p, 3) || in.more()) {
					ps.error("bonepos needs three components");
					continue;
				}
				bone->position_given = true;
				bone->position_stored = BadBuildVec3{p[0], p[1], p[2]};
			}
		} else if (key == "event") {
			// The heights are the hips' and the head's above the ground, which
			// the clip does not hold: every event states both.
			double v[3];
			long long trigger = 0;
			double heights[2];
			if (!in.numbers(v, 3) || !in.integer(trigger, 0, kWordMax) || !in.numbers(heights, 2) ||
					in.more()) {
				ps.error("event needs a velocity, a trigger word, a bottom and a top");
				continue;
			}
			BadBuildEvent ev;
			ev.velocity = BadBuildVec3{v[0], v[1], v[2]};
			ev.trigger = static_cast<int32_t>(static_cast<uint32_t>(trigger));
			ev.bottom = heights[0];
			ev.top = heights[1];
			clip->events.push_back(ev);
		} else {
			ps.error("unknown record `" + key + "`");
		}
	}
	if (!header) ps.error("the file is empty");
	// A bone keeps a duration table only when its keys state one. A uniform
	// table over one key per frame is no table at all (the reader reads a
	// missing table as ones); a bone that keys a different number of times
	// keeps its table, uniform or not, because the count is what the table
	// accounts for (retail's DT1RST, stgr_RST and M60_1i key uniformly but not
	// once per frame).
	for (const BoneKeys &keys : bone_keys) {
		BadBuildClip &c = set.clips[keys.clip];
		BadBuildBone &b = c.bones[keys.bone];
		if (keys.with != 0 && keys.without != 0) {
			ps.error_at(keys.line, "bone '" + b.name + "' states a duration on some keys and not "
												 "on others");
			continue;
		}
		if (b.keys.size() != static_cast<size_t>(c.frame_count) + 1) continue;
		bool uniform = true;
		for (const uint16_t d : b.durations) uniform = uniform && d == 1;
		if (uniform) b.durations.clear();
	}
}

// What no single record can see: a row naming a clip the set lacks, two
// clips under one name (they would write the same file), a clip name or a
// variant that is not a bare file stem (`build` writes each clip beside the
// table, so a path there would write outside it), a row whose key names no
// anim slot, and the set's own checks (bad_build_check_set: every clip file
// packs, translations only over a translated reset).
void validate(Parser &ps, const BadBuildSet &set) {
	ps.line = 0;
	// The game registers a row only under the slot its key names past the
	// first five characters and drops any other without a word.
	// [orig: AnimMap_ParseConfigLine @0x40CB60, AnimMap_FindSlotByName
	//  @0x40CFA0 returns -1 @0x40CFCE and the row registers nothing @0x40CBA4]
	for (const BadBuildRow &row : set.rows) {
		if (opennova::anim::adm_slot_index(row.key) < 0)
			ps.errors.push_back(ps.path + ": row '" + row.key + "' names no anim slot (past its first "
					"five characters the key is none of the 252 slot names `opennova-3di catalog` "
					"lists), and the game drops such a row");
	}
	std::vector<std::string> problems;
	bad_build_check_set(set, problems);
	for (const std::string &problem : problems) ps.errors.push_back(ps.path + ": " + problem);
	for (const BadBuildClip &clip : set.clips) {
		if (!bad_build_bare_stem(clip.name))
			ps.errors.push_back(ps.path + ": clip '" + clip.name + "' is not a bare file name");
	}
	for (size_t i = 0; i < set.clips.size(); ++i) {
		for (size_t j = i + 1; j < set.clips.size(); ++j) {
			if (opennova::strutil::iequals(set.clips[i].name, set.clips[j].name))
				ps.errors.push_back(ps.path + ": two clips are named '" + set.clips[i].name + "'");
		}
	}
	for (const BadBuildRow &row : set.rows) {
		for (const std::string &variant : row.variants) {
			const std::string stem = bad_build_clip_stem(variant);
			if (!bad_build_bare_stem(stem)) {
				ps.errors.push_back(ps.path + ": row '" + row.key + "' names '" + variant +
						"', which is not a bare file name");
				continue;
			}
			bool found = false;
			for (const BadBuildClip &clip : set.clips)
				found = found || opennova::strutil::iequals(clip.name, stem);
			if (!found)
				ps.errors.push_back(ps.path + ": row '" + row.key + "' names '" + variant +
						"', which the set does not hold");
		}
	}
}

} // namespace

int cmd_anim_build(const char *scene_path, const char *out_path) {
	std::ifstream file(scene_path);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot open %s\n", scene_path);
		return 1;
	}
	Parser ps;
	ps.path = scene_path;
	BadBuildSet set;
	parse_set(ps, file, set);
	if (ps.errors.empty()) validate(ps, set);
	if (!ps.errors.empty()) {
		for (const std::string &e : ps.errors) std::fprintf(stderr, "%s\n", e.c_str());
		return 1;
	}
	if (set.clips.empty()) {
		std::fprintf(stderr, "opennova-3di: the set holds no clip\n");
		return 1;
	}

	const std::filesystem::path out = std::filesystem::path(out_path);
	const bool lone = opennova::strutil::iequals(out.extension().string(), ".bad");
	// The file the game packs keeps its name: 15 bytes at most, the
	// extension included (bad_build_packable_name).
	const std::string out_name = out.filename().string();
	if (!bad_build_packable_name(out_name)) {
		std::fprintf(stderr, "opennova-3di: '%s' is %zu bytes; the game packs a file name of at most %zu "
							 "ASCII bytes, its extension included\n",
				out_name.c_str(), out_name.size(), kBadPackedNameMax);
		return 1;
	}
	if (lone && (set.clips.size() != 1 || !set.rows.empty())) {
		std::fprintf(stderr, "opennova-3di: a .bad output takes one clip and no table row\n");
		return 1;
	}
	if (!lone && set.rows.empty()) {
		std::fprintf(stderr, "opennova-3di: the set holds no table row (write one clip as .bad "
							 "instead)\n");
		return 1;
	}
	// `-o` names the file; an `adm` record that names another is only noted.
	if (!set.adm_name.empty() &&
			(lone || !opennova::strutil::iequals(set.adm_name, out.filename().string())))
		std::fprintf(stderr, "opennova-3di: note: the set names its table '%s'; -o writes %s\n",
				set.adm_name.c_str(), out_path);

	// Every clip of a table composes against its reset clip; a lone clip
	// against its own first key.
	const BadBuildClip *reset = lone ? nullptr : bad_build_reset_clip(set);
	// Mint every clip and the table in memory, and read each back, before any
	// file is written: a set that fails anywhere writes nothing.
	struct Minted {
		std::string path;
		std::vector<uint8_t> bytes;
	};
	std::vector<Minted> files;
	size_t total = 0;
	for (size_t c = 0; c < set.clips.size(); ++c) {
		const BadBuildClip &clip = set.clips[c];
		Minted minted;
		std::string error;
		if (!bad_build_mint(clip, reset, minted.bytes, &error)) {
			// The seam's refusal names the line the clip opens on.
			std::fprintf(stderr, "%s:%d: clip '%s': %s\n", scene_path, ps.clip_lines[c],
					clip.name.c_str(), error.c_str());
			return 1;
		}
		// Read the bytes back through the loader's own reader before shipping.
		BadFile check{};
		if (bad_parse_buffer(minted.bytes.data(), minted.bytes.size(), &check) != 0) {
			std::fprintf(stderr, "opennova-3di: clip '%s' does not read back\n", clip.name.c_str());
			return 1;
		}
		bad_free(&check);
		minted.path = lone ? out.string() : (out.parent_path() / (clip.name + ".bad")).string();
		total += minted.bytes.size();
		files.push_back(std::move(minted));
	}
	if (!lone) {
		std::string text;
		std::string error;
		if (!bad_build_mint_table(set, text, &error)) {
			std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
			return 1;
		}
		files.push_back(Minted{out.string(), std::vector<uint8_t>(text.begin(), text.end())});
	}

	// Each file lands whole or not at all (write_output): a full disk never
	// leaves a truncated clip where the last good one was.
	for (const Minted &minted : files)
		if (!write_output(minted.path.c_str(), minted.bytes.data(), minted.bytes.size())) return 1;
	if (!lone) {
		std::printf("wrote %s (%zu rows) and %zu clips (%zu bytes)\n", out_path, set.rows.size(),
				set.clips.size(), total);
		return 0;
	}
	std::printf("wrote %s (%zu bytes)\n", out_path, total);
	return 0;
}

} // namespace threedi_cli
