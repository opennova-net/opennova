// The `.o3a` clip-set text into the clip construction seam (bad_o3a_read.h).

#include <formats/bad/bad_o3a_read.h>

#include <string>
#include <vector>

#include <base/io/strutil.h>

namespace opennova::bad {

using threedi::SceneFinding;
using threedi::SceneLine;
using threedi::SceneNumbers;
using threedi::strip_comment;

namespace {

struct Parser {
	std::vector<SceneFinding> &findings;
	int line = 0;
	// The line each clip opens on, in set order: a seam refusal names it.
	std::vector<int> clip_lines;

	void error(const std::string &what) { error_at(line, what); }
	void error_at(int at, const std::string &what) { findings.push_back(SceneFinding{at, true, what}); }
	void set_error(const std::string &what) { findings.push_back(SceneFinding{0, true, what}); }
	bool failed() const {
		for (const SceneFinding &f : findings)
			if (f.error) return true;
		return false;
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
void validate(Parser &ps, BadSlotLookup slots, const BadBuildSet &set) {
	// The game registers a row only under the slot its key names past the
	// first five characters and drops any other without a word.
	// [orig: AnimMap_ParseConfigLine @0x40CB60, AnimMap_FindSlotByName
	//  @0x40CFA0 returns -1 @0x40CFCE and the row registers nothing @0x40CBA4]
	for (const BadBuildRow &row : set.rows) {
		if (slots == nullptr || slots(row.key) < 0)
			ps.set_error("row '" + row.key + "' names no anim slot (past its first five characters the key is none "
					"of the 252 slot names `opennova-3di catalog` lists), and the game drops such a row");
	}
	std::vector<std::string> problems;
	bad_build_check_set(set, problems);
	for (const std::string &problem : problems) ps.set_error(problem);
	for (const BadBuildClip &clip : set.clips) {
		if (!bad_build_bare_stem(clip.name)) ps.set_error("clip '" + clip.name + "' is not a bare file name");
	}
	for (size_t i = 0; i < set.clips.size(); ++i) {
		for (size_t j = i + 1; j < set.clips.size(); ++j) {
			if (strutil::iequals(set.clips[i].name, set.clips[j].name))
				ps.set_error("two clips are named '" + set.clips[i].name + "'");
		}
	}
	for (const BadBuildRow &row : set.rows) {
		for (const std::string &variant : row.variants) {
			const std::string stem = bad_build_clip_stem(variant);
			if (!bad_build_bare_stem(stem)) {
				ps.set_error("row '" + row.key + "' names '" + variant + "', which is not a bare file name");
				continue;
			}
			bool found = false;
			for (const BadBuildClip &clip : set.clips) found = found || strutil::iequals(clip.name, stem);
			if (!found) ps.set_error("row '" + row.key + "' names '" + variant + "', which the set does not hold");
		}
	}
}

} // namespace

bool bad_o3a_read(std::istream &text, BadSlotLookup slots, BadBuildSet &set, std::vector<SceneFinding> &findings,
		std::vector<int> *clip_lines) {
	Parser ps{findings};
	parse_set(ps, text, set);
	if (!ps.failed()) validate(ps, slots, set);
	if (clip_lines != nullptr) *clip_lines = ps.clip_lines;
	return !ps.failed();
}

} // namespace opennova::bad
