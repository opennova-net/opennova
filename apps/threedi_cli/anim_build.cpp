// opennova-3di anim build: read the `.o3a` clip-set text a DCC exporter writes
// (the grammar is docs/anim/o3a-scene-format.md) into the engine's clip
// construction seam (formats/bad/bad_build.h) and serialize every clip through
// the writer, so a shipped `.bad` is produced by the same writer the fixtures
// are (ADR 0003). The text carries rotations, pivots, translations and event
// velocities in MISSION axes (x forward, y left, z up); the clip-frame
// conversion is the seam's, never the exporter's.

#include "anim_cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>

using namespace opennova::bad;

namespace threedi_cli {

namespace {

struct Parser {
	std::string path;
	int line = 0;
	std::vector<std::string> errors;

	void error(const std::string &what) {
		errors.push_back(path + ":" + std::to_string(line) + ": " + what);
	}
};

bool read_double(std::istringstream &in, double &out) {
	std::string token;
	if (!(in >> token)) return false;
	char *end = nullptr;
	out = std::strtod(token.c_str(), &end);
	return end != nullptr && *end == '\0' && end != token.c_str();
}

bool read_doubles(std::istringstream &in, double *out, int n) {
	for (int i = 0; i < n; ++i)
		if (!read_double(in, out[i])) return false;
	return true;
}

// An integer field in decimal or 0x hex (the flag and trigger words).
bool read_word(std::istringstream &in, long long &out) {
	std::string token;
	if (!(in >> token)) return false;
	char *end = nullptr;
	out = std::strtoll(token.c_str(), &end, 0);
	return end != nullptr && *end == '\0';
}

// A name field: a bare token, or "a quoted one" that may hold spaces (a bone
// name is `BN01 Pelvis`).
bool read_name(std::istringstream &in, std::string &out) {
	out.clear();
	in >> std::ws;
	if (in.peek() != '"') return static_cast<bool>(in >> out);
	in.get();
	std::getline(in, out, '"');
	return !in.bad();
}

// Strip a comment: `#` at the start of a line or after whitespace, never
// inside quotes.
void strip_comment(std::string &line) {
	bool quoted = false;
	for (size_t i = 0; i < line.size(); ++i) {
		if (line[i] == '"') quoted = !quoted;
		if (!quoted && line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
			line.erase(i);
			return;
		}
	}
}

void parse_set(Parser &ps, std::istream &file, BadBuildSet &set) {
	std::string raw;
	bool header = false;
	BadBuildClip *clip = nullptr;
	BadBuildBone *bone = nullptr;
	while (std::getline(file, raw)) {
		++ps.line;
		if (!raw.empty() && raw.back() == '\r') raw.pop_back();
		strip_comment(raw);
		std::istringstream in(raw);
		std::string key;
		if (!(in >> key)) continue;

		if (!header) {
			long long version = 0;
			if (key != "o3a" || !read_word(in, version) || version != 1) {
				ps.error("a clip set starts with `o3a 1`");
				return;
			}
			header = true;
			continue;
		}

		if (key == "adm") {
			if (!read_name(in, set.adm_name)) ps.error("adm needs a name");
		} else if (key == "row") {
			BadBuildRow row;
			if (!read_name(in, row.key)) {
				ps.error("row needs a slot key");
				continue;
			}
			std::string variant;
			while (read_name(in, variant) && !variant.empty()) row.variants.push_back(variant);
			if (row.variants.empty()) ps.error("row '" + row.key + "' names no clip");
			set.rows.push_back(row);
		} else if (key == "clip") {
			set.clips.push_back(BadBuildClip{});
			clip = &set.clips.back();
			bone = nullptr;
			if (!read_name(in, clip->name) || clip->name.empty()) ps.error("clip needs a name");
		} else if (clip == nullptr) {
			ps.error("`" + key + "` before any clip");
		} else if (key == "fps" || key == "frames" || key == "version" || key == "flags") {
			long long value = 0;
			if (!read_word(in, value) || value < 0) {
				ps.error(key + " needs a whole number");
				continue;
			}
			if (key == "fps") clip->fps = static_cast<uint32_t>(value);
			if (key == "frames") clip->frame_count = static_cast<uint32_t>(value);
			if (key == "version") clip->version = static_cast<uint32_t>(value);
			if (key == "flags") clip->flags = static_cast<uint32_t>(value);
		} else if (key == "capsule") {
			double pair[2];
			if (!read_doubles(in, pair, 2)) {
				ps.error("capsule needs a bottom and a top");
				continue;
			}
			clip->capsule_given = true;
			clip->capsule_bottom = pair[0];
			clip->capsule_top = pair[1];
		} else if (key == "bone") {
			long long parent = 0;
			double pivot[3];
			double length = 0.0;
			std::string name;
			if (!read_word(in, parent) || !read_doubles(in, pivot, 3) || !read_double(in, length) ||
					!read_name(in, name)) {
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
		} else if (key == "k" || key == "tr" || key == "bonepos") {
			if (bone == nullptr) {
				ps.error("`" + key + "` before any bone");
				continue;
			}
			if (key == "k") {
				double q[4];
				if (!read_doubles(in, q, 4)) {
					ps.error("k needs four quaternion components");
					continue;
				}
				bone->keys.push_back(BadBuildQuat{q[0], q[1], q[2], q[3]});
				long long duration = 1;
				if (read_word(in, duration) && (duration < 1 || duration > 0xFFFF)) {
					ps.error("a key duration is outside 1..65535");
					continue;
				}
				// One entry per key; an all-ones table is dropped below, so the
				// seam's own default is what a uniform clip takes.
				bone->durations.push_back(static_cast<uint16_t>(duration < 1 ? 1 : duration));
			} else if (key == "tr") {
				double t[3];
				if (!read_doubles(in, t, 3)) {
					ps.error("tr needs three components");
					continue;
				}
				bone->translations.push_back(BadBuildVec3{t[0], t[1], t[2]});
			} else {
				double p[3];
				if (!read_doubles(in, p, 3)) {
					ps.error("bonepos needs three components");
					continue;
				}
				bone->position_given = true;
				bone->position_stored = BadBuildVec3{p[0], p[1], p[2]};
			}
		} else if (key == "event") {
			double v[3];
			long long trigger = 0;
			if (!read_doubles(in, v, 3) || !read_word(in, trigger)) {
				ps.error("event needs a velocity and a trigger word");
				continue;
			}
			BadBuildEvent ev;
			ev.velocity = BadBuildVec3{v[0], v[1], v[2]};
			ev.trigger = static_cast<int32_t>(trigger);
			double pair[2];
			if (read_doubles(in, pair, 2)) {
				ev.extents_given = true;
				ev.bottom = pair[0];
				ev.top = pair[1];
			}
			clip->events.push_back(ev);
		} else {
			ps.error("unknown record `" + key + "`");
		}
	}
	if (!header) ps.error("the file is empty");
	// A uniform duration table over one key per frame is no table at all: the
	// reader reads a missing table as ones. A bone that keys a different number
	// of times keeps its table, uniform or not, because the count is what the
	// table accounts for (retail's DT1RST, stgr_RST and M60_1i key uniformly
	// but not once per frame).
	for (BadBuildClip &c : set.clips) {
		for (BadBuildBone &b : c.bones) {
			if (b.keys.size() != static_cast<size_t>(c.frame_count) + 1) continue;
			bool uniform = true;
			for (const uint16_t d : b.durations) uniform = uniform && d == 1;
			if (uniform) b.durations.clear();
		}
	}
}

// What no single record can see: a row naming a clip the set lacks, and two
// clips under one name (they would write the same file).
void validate(Parser &ps, const BadBuildSet &set) {
	ps.line = 0;
	for (size_t i = 0; i < set.clips.size(); ++i) {
		for (size_t j = i + 1; j < set.clips.size(); ++j) {
			if (opennova::strutil::iequals(set.clips[i].name, set.clips[j].name))
				ps.errors.push_back(ps.path + ": two clips are named '" + set.clips[i].name + "'");
		}
	}
	for (const BadBuildRow &row : set.rows) {
		for (const std::string &variant : row.variants) {
			const std::string stem = anim_clip_stem(variant);
			bool found = false;
			for (const BadBuildClip &clip : set.clips)
				found = found || opennova::strutil::iequals(clip.name, stem);
			if (!found)
				ps.errors.push_back(ps.path + ": row '" + row.key + "' names '" + variant +
						"', which the set does not hold");
		}
	}
}

bool write_bytes(const std::string &path, const std::vector<uint8_t> &bytes) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) return false;
	const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	std::fclose(f);
	return ok;
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
	if (lone && (set.clips.size() != 1 || !set.rows.empty())) {
		std::fprintf(stderr, "opennova-3di: a .bad output takes one clip and no table row\n");
		return 1;
	}
	if (!lone && set.rows.empty()) {
		std::fprintf(stderr, "opennova-3di: the set holds no table row (write one clip as .bad "
							 "instead)\n");
		return 1;
	}

	size_t written = 0;
	size_t total = 0;
	for (const BadBuildClip &clip : set.clips) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (!bad_build_mint(clip, bytes, &error)) {
			std::fprintf(stderr, "opennova-3di: clip '%s': %s\n", clip.name.c_str(), error.c_str());
			return 1;
		}
		// Read the bytes back through the loader's own reader before shipping.
		BadFile check{};
		if (bad_parse_buffer(bytes.data(), bytes.size(), &check) != 0) {
			std::fprintf(stderr, "opennova-3di: clip '%s' does not read back\n", clip.name.c_str());
			return 1;
		}
		bad_free(&check);
		const std::string path =
				lone ? out.string() : (out.parent_path() / (clip.name + ".bad")).string();
		if (!write_bytes(path, bytes)) {
			std::fprintf(stderr, "opennova-3di: cannot write %s\n", path.c_str());
			return 1;
		}
		++written;
		total += bytes.size();
	}

	if (!lone) {
		std::string text;
		std::string error;
		if (!bad_build_mint_table(set, text, &error)) {
			std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
			return 1;
		}
		const std::vector<uint8_t> bytes(text.begin(), text.end());
		if (!write_bytes(out.string(), bytes)) {
			std::fprintf(stderr, "opennova-3di: cannot write %s\n", out_path);
			return 1;
		}
		std::printf("wrote %s (%zu rows) and %zu clips (%zu bytes)\n", out_path, set.rows.size(),
				written, total);
		return 0;
	}
	std::printf("wrote %s (%zu bytes)\n", out_path, total);
	return 0;
}

} // namespace threedi_cli
