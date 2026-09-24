// opennova-3di anim scene: write a clip set back out as `.o3a` text, the
// inverse of `anim build`, so a DCC importer lays the clips out on the rig the
// naming contract describes and they export again. Only records `build` reads
// are written, in the order it requires, and every conversion is the exact
// inverse of the seam's, so `build(scene(x))` re-mints a builder-made set byte
// for byte.
//
// The bone pivot a clip was exported from is recovered by inverting the
// position relation the seam derives (bad_build.h). Where the re-derivation
// misses the stored bytes — retail's own exporter left junk in that dead field
// — the bone also carries an explicit `bonepos`, the way the `.o3d` scene
// carries a collision normal it cannot rebuild.

#include "anim_cli.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <formats/bad/bad_build.h>

using namespace opennova::bad;

namespace threedi_cli {

namespace {

// Floats print with 9 significant digits (a float32 round-trips exactly);
// a pivot is a double the builder turns back into floats, so it takes 17.
std::string f9(double v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.9g", v);
	return std::strcmp(buf, "-0") == 0 ? std::string("0") : std::string(buf);
}

std::string f17(double v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	return std::strcmp(buf, "-0") == 0 ? std::string("0") : std::string(buf);
}

std::string name_field(const std::string &name) {
	bool plain = !name.empty();
	for (const char c : name) plain = plain && c != ' ' && c != '\t' && c != '"' && c != '#';
	return plain ? name : "\"" + name + "\"";
}

struct Writer {
	FILE *f = nullptr;
	std::vector<std::string> notes;

	void line(const std::string &s) { std::fprintf(f, "%s\n", s.c_str()); }
	void note(const std::string &s) {
		notes.push_back(s);
		line("# dropped: " + s);
	}
};

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

void write_clip(Writer &w, const AnimLoadedClip &clip) {
	const BadFile &file = clip.file;
	w.line("");
	w.line("clip " + name_field(clip.name));
	if (file.version != 1) w.line("version " + std::to_string(file.version));
	w.line("fps " + std::to_string(file.fps));
	char flags[24];
	std::snprintf(flags, sizeof(flags), "0x%x", static_cast<unsigned>(file.flags));
	w.line(std::string("flags ") + flags);
	w.line("frames " + std::to_string(file.frame_count));

	const size_t bones = file.num_bones;
	const size_t keys = static_cast<size_t>(file.frame_count) + 1;
	const bool translated = (file.flags & BAD_FLAG_TRANSLATION) != 0 && file.translations != nullptr;

	// The bind the seam derives from each bone's first key: the pivots are
	// recovered against it, so a set the builder made carries no `bonepos`.
	std::vector<std::vector<float>> bind(bones, std::vector<float>(9, 0.0f));
	std::vector<BadBuildVec3> pivot(bones);
	for (size_t i = 0; i < bones; ++i) {
		const BadChannel &ch = file.channels[i];
		BadBuildQuat k0{};
		if (ch.frame_count > 0 && ch.rotations != nullptr)
			k0 = BadBuildQuat{ch.rotations[0].x, ch.rotations[0].y, ch.rotations[0].z,
					ch.rotations[0].w};
		float rows[9];
		bad_quat_to_rows(k0, rows);
		bad_rows_transpose(rows, bind[i].data());
	}
	for (size_t i = 0; i < bones; ++i) {
		const BadBone &bone = file.bones[i];
		const int parent = bone.parent_index;
		if (parent < 0 || static_cast<size_t>(parent) >= i) {
			pivot[i] = bad_mission_from_clip(BadBuildVec3{bone.position[0], bone.position[1],
					bone.position[2]});
			continue;
		}
		const BadBuildVec3 rel = bad_bone_rel(bind[static_cast<size_t>(parent)].data(), bone.position);
		pivot[i] = BadBuildVec3{pivot[static_cast<size_t>(parent)].x + rel.x,
				pivot[static_cast<size_t>(parent)].y + rel.y,
				pivot[static_cast<size_t>(parent)].z + rel.z};
	}

	for (size_t i = 0; i < bones; ++i) {
		const BadBone &bone = file.bones[i];
		const int parent = bone.parent_index;
		w.line("bone " + std::to_string(parent) + " " + f17(pivot[i].x) + " " + f17(pivot[i].y) +
				" " + f17(pivot[i].z) + " " + f9(bone.length) + " " + name_field(bone.name));

		// Does the pivot re-derive the stored position? Only then is the
		// `bonepos` override unnecessary.
		BadBuildVec3 derived;
		if (parent < 0 || static_cast<size_t>(parent) >= i) {
			derived = bad_clip_from_mission(pivot[i]);
		} else {
			const BadBuildVec3 rel{pivot[i].x - pivot[static_cast<size_t>(parent)].x,
					pivot[i].y - pivot[static_cast<size_t>(parent)].y,
					pivot[i].z - pivot[static_cast<size_t>(parent)].z};
			derived = bad_bone_position(bind[static_cast<size_t>(parent)].data(), rel);
		}
		if (!same_float(static_cast<float>(derived.x), bone.position[0]) ||
				!same_float(static_cast<float>(derived.y), bone.position[1]) ||
				!same_float(static_cast<float>(derived.z), bone.position[2])) {
			w.line("bonepos " + f9(bone.position[0]) + " " + f9(bone.position[1]) + " " +
					f9(bone.position[2]));
		}

		const BadChannel &ch = file.channels[i];
		// The duration table is written whenever the seam could not assume it:
		// a non-uniform table, or a bone that keys sparsely (its key count is
		// what the table, not the header, accounts for).
		const bool uniform = [&] {
			if (ch.frame_count != keys) return false;
			if (ch.frame_lengths == nullptr) return true;
			for (uint32_t k = 0; k < ch.frame_count; ++k)
				if (ch.frame_lengths[k] != 1) return false;
			return true;
		}();
		for (uint32_t k = 0; k < ch.frame_count; ++k) {
			const BadQuaternion &q = ch.rotations[k];
			const BadBuildQuat m = bad_mission_from_clip(BadBuildQuat{q.x, q.y, q.z, q.w});
			std::string row = " k " + f9(m.x) + " " + f9(m.y) + " " + f9(m.z) + " " + f9(m.w);
			if (!uniform)
				row += " " + std::to_string(ch.frame_lengths != nullptr ? ch.frame_lengths[k] : 1);
			w.line(row);
		}
		if (translated) {
			// The loader reads bone_count * frame_count rows, frame-major, and
			// holds the last past the end, so the scene carries exactly those.
			for (size_t k = 0; k < file.frame_count; ++k) {
				const size_t index = k * bones + i;
				if (index >= file.num_translations) break;
				const BadBuildVec3 t = bad_mission_from_clip(BadBuildVec3{
						file.translations[index][0], file.translations[index][1],
						file.translations[index][2]});
				w.line(" tr " + f9(t.x) + " " + f9(t.y) + " " + f9(t.z));
			}
		}
	}

	for (size_t e = 0; e < file.num_events; ++e) {
		const BadEvent &ev = file.events[e];
		const BadBuildVec3 v = bad_mission_from_clip(
				BadBuildVec3{ev.velocity[0], ev.velocity[1], ev.velocity[2]});
		char trigger[24];
		std::snprintf(trigger, sizeof(trigger), "0x%x", static_cast<unsigned>(ev.trigger));
		w.line("event " + f9(v.x) + " " + f9(v.y) + " " + f9(v.z) + " " + trigger + " " +
				f9(ev.bottom) + " " + f9(ev.top));
	}
	if (file.num_events != 0 && file.num_events != keys)
		w.note("the clip holds " + std::to_string(file.num_events) + " events, not the " +
				std::to_string(keys) + " the header's frame count implies");
	if (file.num_events == 0) w.note("the clip carries no event record (no root motion, no capsule)");
}

} // namespace

int cmd_anim_scene(const char *in_path, const char *out_path) {
	AnimLoadedSet set;
	std::string error;
	if (!anim_load(in_path, set, error)) {
		std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
		return 1;
	}
	Writer w;
	w.f = std::fopen(out_path, "w");
	if (w.f == nullptr) {
		std::fprintf(stderr, "opennova-3di: cannot write %s\n", out_path);
		anim_free(set);
		return 1;
	}
	w.line("o3a 1");
	w.line(std::string("# clip set of ") + in_path + " (opennova-3di anim scene)");
	if (!set.table_name.empty()) w.line("adm " + name_field(set.table_name));
	for (const BadBuildRow &row : set.rows) {
		std::string line = "row " + name_field(row.key);
		size_t held = 0;
		for (const std::string &variant : row.variants) {
			// A row names a clip the set does not hold when its .bad is not
			// beside the table; build would refuse the row, so it is dropped.
			bool missing = false;
			for (const std::string &absent : set.missing) missing = missing || absent == variant;
			if (missing) continue;
			line += " \"" + variant + "\"";
			++held;
		}
		if (held == 0) {
			w.note("row '" + row.key + "' names no clip that is beside the table");
			continue;
		}
		w.line(line);
	}
	for (const std::string &variant : set.missing)
		w.note("the table names '" + variant + "', whose .bad is not beside it");
	for (const AnimLoadedClip &clip : set.clips) write_clip(w, clip);
	std::fclose(w.f);
	for (const std::string &n : w.notes)
		std::fprintf(stderr, "opennova-3di: note: scene drops %s\n", n.c_str());
	std::printf("wrote %s (%zu rows, %zu clips)\n", out_path, set.rows.size(), set.clips.size());
	anim_free(set);
	return 0;
}

} // namespace threedi_cli
