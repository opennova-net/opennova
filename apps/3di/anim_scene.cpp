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

#include <base/io/strutil.h>
#include <formats/bad/bad_build.h>

#include <formats/threedi/scene_text.h>
#include "threedi_cli.h"

using namespace opennova::bad;
using opennova::threedi::f17;
using opennova::threedi::f9;
using opennova::threedi::name_field;

namespace opennova::threedi_cli {

namespace {

// The scene text, written out whole once the set has been walked.
struct Writer {
	std::string text;
	std::vector<std::string> notes;

	void line(const std::string &s) {
		text += s;
		text += '\n';
	}
	// What the text cannot carry, as a comment in the scene and on stderr,
	// marked as the `.o3d` scene marks what it drops, so a front end reading
	// either scene surfaces both the same way.
	void note(const std::string &s) {
		notes.push_back(s);
		line("# dropped: " + s);
	}
};

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// A loaded clip as the seam sees what it derives from: each bone's name,
// parent, length and first key (mission axes), pivots left for the caller.
BadBuildClip clip_shape(const AnimLoadedClip &clip) {
	BadBuildClip shape;
	shape.name = clip.name;
	shape.frame_count = clip.file.frame_count;
	shape.bones.resize(clip.file.num_bones);
	for (size_t i = 0; i < clip.file.num_bones; ++i) {
		BadBuildBone &bone = shape.bones[i];
		bone.name = clip.file.bones[i].name;
		bone.parent = clip.file.bones[i].parent_index;
		bone.length = clip.file.bones[i].length;
		const BadChannel &ch = clip.file.channels[i];
		if (ch.frame_count > 0 && ch.rotations != nullptr) {
			const BadQuaternion &q = ch.rotations[0];
			bone.keys.push_back(bad_mission_from_clip(BadBuildQuat{q.x, q.y, q.z, q.w}));
		}
	}
	return shape;
}

// Why `build` could not mint this clip again from its text, or empty: the
// scene notes such a clip and leaves it out rather than write a set that
// build refuses whole.
std::string inexpressible(const AnimLoadedClip &clip) {
	const BadFile &file = clip.file;
	const size_t keys = static_cast<size_t>(file.frame_count) + 1;
	if (file.num_bones == 0) return "it holds no bones";
	if (file.frame_count == 0) return "it holds no frames";
	if (file.version > 1) return "its version is " + std::to_string(file.version) + ", not 0 or 1";
	if (file.num_events != 0 && file.num_events != keys)
		return "it holds " + std::to_string(file.num_events) + " events, not the " +
				std::to_string(keys) + " its frame count implies";
	for (size_t i = 0; i < file.num_bones; ++i) {
		const BadBone &bone = file.bones[i];
		const std::string at = "bone " + std::to_string(i);
		if (std::strlen(bone.name) > 31) return at + "'s name fills all 32 bytes";
		if (std::strchr(bone.name, '"') != nullptr) return at + "'s name holds a quote";
		if (i > 0 && (bone.parent_index < 0 || static_cast<size_t>(bone.parent_index) >= i))
			return at + "'s parent is not a lower index";
		const BadChannel &ch = file.channels[i];
		if (ch.frame_count == 0 || ch.rotations == nullptr) return at + " holds no key";
		for (uint32_t k = 0; k < ch.frame_count; ++k) {
			const BadQuaternion &q = ch.rotations[k];
			const double len = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
					static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w);
			if (!(std::fabs(len - 1.0) <= 1e-3)) return at + " holds a key that is not a unit quaternion";
			if (ch.frame_lengths != nullptr && ch.frame_lengths[k] == 0)
				return at + " holds a zero duration";
		}
		if (ch.frame_count != keys && ch.frame_lengths == nullptr)
			return at + " keys sparsely with no duration table";
	}
	return std::string();
}

void write_clip(Writer &w, const AnimLoadedClip &clip, const BadBuildClip *reset) {
	const BadFile &file = clip.file;
	w.line("");
	w.line("clip " + name_field(w, clip.name));
	if (file.version != 1) w.line("version " + std::to_string(file.version));
	w.line("fps " + std::to_string(file.fps));
	char flags[24];
	std::snprintf(flags, sizeof(flags), "0x%x", static_cast<unsigned>(file.flags));
	w.line(std::string("flags ") + flags);
	w.line("frames " + std::to_string(file.frame_count));

	const size_t bones = file.num_bones;
	const size_t keys = static_cast<size_t>(file.frame_count) + 1;
	const bool translated = (file.flags & BAD_FLAG_TRANSLATION) != 0 && file.translations != nullptr;

	// The pivots are recovered through the bind the seam derives positions
	// with, the set's reset clip's (the runtime composes every clip of a rig
	// against it), so every clip of a table recovers the rig's one set of
	// pivots and a set the builder made carries no `bonepos`.
	BadBuildClip shaped = clip_shape(clip);
	std::vector<BadBuildVec3> pivot(bones);
	for (size_t i = 0; i < bones; ++i) {
		const BadBone &bone = file.bones[i];
		const int parent = bone.parent_index;
		if (parent < 0 || static_cast<size_t>(parent) >= i) {
			pivot[i] = bad_mission_from_clip(BadBuildVec3{bone.position[0], bone.position[1],
					bone.position[2]});
		} else {
			float bind[9];
			bad_derive_bind_rows(shaped, reset, static_cast<size_t>(parent), bind);
			const BadBuildVec3 rel = bad_bone_rel(bind, bone.position);
			pivot[i] = BadBuildVec3{pivot[static_cast<size_t>(parent)].x + rel.x,
					pivot[static_cast<size_t>(parent)].y + rel.y,
					pivot[static_cast<size_t>(parent)].z + rel.z};
		}
		shaped.bones[i].pivot = pivot[i];
	}
	// Does the pivot re-derive the stored position? Only then is the `bonepos`
	// override unnecessary.
	std::vector<BadBone> derived;
	bad_derive_bone_table(shaped, reset, derived);

	for (size_t i = 0; i < bones; ++i) {
		const BadBone &bone = file.bones[i];
		const int parent = bone.parent_index;
		w.line("bone " + std::to_string(parent) + " " + f17(pivot[i].x) + " " + f17(pivot[i].y) +
				" " + f17(pivot[i].z) + " " + f9(bone.length) + " " + name_field(w, bone.name));
		if (!same_float(derived[i].position[0], bone.position[0]) ||
				!same_float(derived[i].position[1], bone.position[1]) ||
				!same_float(derived[i].position[2], bone.position[2])) {
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
			// Rows 0..frame_count, frame-major: every row the runtime's read
			// gives weight. The pad row past them is the writer's.
			for (size_t k = 0; k < keys; ++k) {
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
		// A version 0 record carries no trigger word (the reader fills in -1),
		// so its event states none.
		char trigger[24];
		std::snprintf(trigger, sizeof(trigger), "0x%x",
				file.version == 0 ? 0u : static_cast<unsigned>(ev.trigger));
		w.line("event " + f9(v.x) + " " + f9(v.y) + " " + f9(v.z) + " " + trigger + " " +
				f9(ev.bottom) + " " + f9(ev.top));
	}
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
	w.line("o3a 1");
	w.line(std::string("# clip set of ") + in_path + " (opennova-3di anim scene)");
	if (!set.table_name.empty()) w.line("adm " + name_field(w, set.table_name));
	// A clip the text cannot express is left out, and its variants with it.
	std::vector<std::string> left_out;
	for (const AnimLoadedClip &clip : set.clips) {
		const std::string why = inexpressible(clip);
		if (why.empty()) continue;
		left_out.push_back(clip.name);
		w.note("clip '" + clip.name + "' (" + why + ")");
	}
	const auto written = [&](const std::string &stem) {
		for (const std::string &name : left_out)
			if (opennova::strutil::iequals(name, stem)) return false;
		for (const AnimLoadedClip &clip : set.clips)
			if (opennova::strutil::iequals(clip.name, stem)) return true;
		return false;
	};
	std::vector<BadBuildRow> kept; // the rows as written
	// A key naming no anim slot registers nothing in the game, and build
	// refuses it (anim_load keeps such rows apart).
	for (const BadBuildRow &row : set.dropped_rows)
		w.note("row '" + row.key + "' (its key names no anim slot, so the game drops it)");
	for (const BadBuildRow &row : set.rows) {
		BadBuildRow held{row.key, {}};
		std::string line = "row " + name_field(w, row.key);
		for (const std::string &variant : row.variants) {
			// A variant whose clip did not load, or is left out, is dropped
			// from its row: build refuses a row naming a clip the set lacks.
			if (!written(bad_build_clip_stem(variant))) continue;
			line += " \"" + variant + "\"";
			held.variants.push_back(variant);
		}
		if (held.variants.empty()) {
			w.note("row '" + row.key + "' (it names no clip the scene holds)");
			continue;
		}
		w.line(line);
		kept.push_back(held);
	}
	for (const AnimMissingClip &absent : set.missing)
		w.note("'" + absent.variant + "' from its rows (" + absent.reason + ")");
	// The set's reset clip, whose bind every clip's positions turn through: the
	// last variant of the last reset row the scene writes, the one build binds
	// to (a reset variant that did not load registers nothing in retail
	// either). A lone clip turns through its own first key. A table left with
	// no reset row binds nothing: retail cannot load it and build refuses it,
	// so it is noted, and its clips are written through their own first keys.
	if (!set.table_name.empty() && bad_build_reset_stem(kept).empty())
		w.note("the table's binding (it has no reset row naming a clip the scene holds: "
			   "`anim build` refuses it, as the game cannot load it)");
	const std::string reset_stem = bad_build_reset_stem(kept);
	BadBuildClip reset_shape;
	const BadBuildClip *reset = nullptr;
	for (const AnimLoadedClip &clip : set.clips) {
		if (!reset_stem.empty() && opennova::strutil::iequals(clip.name, reset_stem)) {
			reset_shape = clip_shape(clip);
			reset = &reset_shape;
		}
	}
	for (const AnimLoadedClip &clip : set.clips) {
		if (inexpressible(clip).empty()) write_clip(w, clip, reset);
	}
	const size_t rows = set.rows.size(), clips = set.clips.size();
	anim_free(set);
	// Whole or not at all, and LF on every platform (write_output).
	if (!write_output(out_path, w.text.data(), w.text.size())) return 1;
	for (const std::string &n : w.notes)
		std::fprintf(stderr, "opennova-3di: note: scene drops %s\n", n.c_str());
	std::printf("wrote %s (%zu rows, %zu clips)\n", out_path, rows, clips);
	return 0;
}

} // namespace opennova::threedi_cli
