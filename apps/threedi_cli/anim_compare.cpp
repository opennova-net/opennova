// opennova-3di anim compare: whether two clip sets hold the same animation.
// It compares what the runtime reads — the table's rows and rings (a variant
// whose clip does not load is a difference), each clip's header, its bones'
// names and parents, the bone table's bind rotation and every channel key as a
// ROTATION (q and -q are one rotation, and retail stores both), the key
// durations, the translations and the events — and ignores what it does not:
// the bone table's dead `position` and `length`, the relocation bookkeeping,
// name padding, and float noise. A value that is not a number is never the
// same as anything. Two lone clips compare with each other whatever their
// names.
//
// Exit 0 when the sets are the same animation, 1 when they differ.

#include "anim_cli.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/adm/adm.h>
#include <formats/bad/bad_build.h>

using namespace opennova::bad;

namespace threedi_cli {

namespace {

// A rotation difference of a twentieth of a degree, a millimetre of travel:
// below what a rig shows and above the float noise a round trip adds.
constexpr double kAngleTolerance = 0.05 * 3.14159265358979323846 / 180.0;
constexpr double kVectorTolerance = 1e-3;

struct Report {
	std::vector<std::string> differences;
	double worst_angle = 0.0;
	double worst_vector = 0.0;
	std::string worst_angle_at;

	void differ(const std::string &what) {
		if (differences.size() < 40) differences.push_back(what);
	}
};

// A gap past a tolerance, NaN included: `gap > tol` is false for NaN, which
// would call two NaNs, or a NaN and anything, the same.
bool beyond(double gap, double tolerance) { return !(gap <= tolerance); }

// Keep the worst gap, a NaN counting as the worst there is.
bool note_worst(double &worst, double gap) {
	if (!beyond(gap, worst)) return false;
	worst = std::isnan(gap) ? HUGE_VAL : gap;
	return true;
}

// The angle between two stored keys, taking the short arc: a negated
// quaternion is the same rotation.
double key_angle(const BadQuaternion &a, const BadQuaternion &b) {
	// Normalized: a retail key is a unit quaternion only to about 2.5e-7, which
	// is a twentieth of a degree of spurious angle on its own.
	const double la = std::sqrt(static_cast<double>(a.x) * a.x + static_cast<double>(a.y) * a.y +
			static_cast<double>(a.z) * a.z + static_cast<double>(a.w) * a.w);
	const double lb = std::sqrt(static_cast<double>(b.x) * b.x + static_cast<double>(b.y) * b.y +
			static_cast<double>(b.z) * b.z + static_cast<double>(b.w) * b.w);
	if (la <= 0.0 || lb <= 0.0) return la == lb ? 0.0 : 3.14159265358979323846;
	double dot = (static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y +
						 static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w) /
			(la * lb);
	dot = std::fabs(dot);
	if (dot > 1.0) dot = 1.0;
	return 2.0 * std::acos(dot);
}

double vector_gap(const float *a, const float *b, int n) {
	double worst = 0.0;
	for (int i = 0; i < n; ++i)
		note_worst(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
	return worst;
}

// The bone table's bind 3x3 as a rotation: the runtime composes every clip of
// a rig against the reset clip's, and a lone clip against its own.
BadQuaternion bind_quat(const BadBone &bone) {
	const BadBuildQuat q = bad_rows_to_quat(bone.rotation);
	return BadQuaternion{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
			static_cast<float>(q.w)};
}

void compare_clip(const AnimLoadedClip &ea, const AnimLoadedClip &eb, Report &r) {
	const std::string at = "clip '" + ea.name + "'";
	const BadFile &a = ea.file;
	const BadFile &b = eb.file;
	if (a.version != b.version)
		r.differ(at + ": version " + std::to_string(a.version) + " vs " + std::to_string(b.version));
	if (a.fps != b.fps)
		r.differ(at + ": fps " + std::to_string(a.fps) + " vs " + std::to_string(b.fps));
	if (a.frame_count != b.frame_count)
		r.differ(at + ": frames " + std::to_string(a.frame_count) + " vs " +
				std::to_string(b.frame_count));
	if (a.flags != b.flags) {
		char words[64];
		std::snprintf(words, sizeof(words), ": flags 0x%x vs 0x%x", static_cast<unsigned>(a.flags),
				static_cast<unsigned>(b.flags));
		r.differ(at + words);
	}
	if (a.bone_count != b.bone_count) {
		r.differ(at + ": " + std::to_string(a.bone_count) + " bones vs " +
				std::to_string(b.bone_count));
		return;
	}

	for (size_t i = 0; i < a.num_bones; ++i) {
		const std::string bone_at = at + " bone " + std::to_string(i);
		if (!opennova::strutil::iequals(a.bones[i].name, b.bones[i].name))
			r.differ(bone_at + ": named '" + a.bones[i].name + "' vs '" + b.bones[i].name + "'");
		if (a.bones[i].parent_index != b.bones[i].parent_index)
			r.differ(bone_at + ": parent " + std::to_string(a.bones[i].parent_index) + " vs " +
					std::to_string(b.bones[i].parent_index));
		const double bind = key_angle(bind_quat(a.bones[i]), bind_quat(b.bones[i]));
		if (note_worst(r.worst_angle, bind)) r.worst_angle_at = bone_at + " bind";
		if (beyond(bind, kAngleTolerance))
			r.differ(bone_at + ": the bind is " + std::to_string(bind * 180.0 / 3.14159265358979323846) +
					" degrees apart");
		const BadChannel &ca = a.channels[i];
		const BadChannel &cb = b.channels[i];
		if (ca.frame_count != cb.frame_count) {
			r.differ(bone_at + ": " + std::to_string(ca.frame_count) + " keys vs " +
					std::to_string(cb.frame_count));
			continue;
		}
		for (uint32_t k = 0; k < ca.frame_count; ++k) {
			const double angle = key_angle(ca.rotations[k], cb.rotations[k]);
			if (note_worst(r.worst_angle, angle)) r.worst_angle_at = bone_at + " key " + std::to_string(k);
			if (beyond(angle, kAngleTolerance))
				r.differ(bone_at + " key " + std::to_string(k) + ": " +
						std::to_string(angle * 180.0 / 3.14159265358979323846) + " degrees apart");
			const uint16_t da = ca.frame_lengths != nullptr ? ca.frame_lengths[k] : 1;
			const uint16_t db = cb.frame_lengths != nullptr ? cb.frame_lengths[k] : 1;
			if (da != db)
				r.differ(bone_at + " key " + std::to_string(k) + ": duration " +
						std::to_string(da) + " vs " + std::to_string(db));
		}
	}

	if (a.num_translations != b.num_translations) {
		r.differ(at + ": " + std::to_string(a.num_translations) + " translations vs " +
				std::to_string(b.num_translations));
	} else {
		for (size_t t = 0; t < a.num_translations; ++t) {
			const double gap = vector_gap(a.translations[t], b.translations[t], 3);
			note_worst(r.worst_vector, gap);
			if (beyond(gap, kVectorTolerance))
				r.differ(at + ": translation " + std::to_string(t) + " is " +
						std::to_string(gap) + " m apart");
		}
	}

	if (a.num_events != b.num_events) {
		r.differ(at + ": " + std::to_string(a.num_events) + " events vs " +
				std::to_string(b.num_events));
		return;
	}
	for (size_t e = 0; e < a.num_events; ++e) {
		const std::string event_at = at + " event " + std::to_string(e);
		const double gap = vector_gap(a.events[e].velocity, b.events[e].velocity, 3);
		note_worst(r.worst_vector, gap);
		if (beyond(gap, kVectorTolerance))
			r.differ(event_at + ": velocity is " + std::to_string(gap) + " m apart");
		const float extents_a[2] = {a.events[e].bottom, a.events[e].top};
		const float extents_b[2] = {b.events[e].bottom, b.events[e].top};
		const double extents = vector_gap(extents_a, extents_b, 2);
		note_worst(r.worst_vector, extents);
		if (beyond(extents, kVectorTolerance))
			r.differ(event_at + ": the capsule is " + std::to_string(extents) + " m apart");
		if (a.events[e].trigger != b.events[e].trigger) {
			char words[96];
			std::snprintf(words, sizeof(words), ": trigger 0x%x vs 0x%x",
					static_cast<unsigned>(a.events[e].trigger),
					static_cast<unsigned>(b.events[e].trigger));
			r.differ(event_at + words);
		}
	}
}

const AnimLoadedClip *find_clip(const AnimLoadedSet &set, const std::string &name) {
	for (const AnimLoadedClip &clip : set.clips)
		if (opennova::strutil::iequals(clip.name, name)) return &clip;
	return nullptr;
}

void compare_rows(const AnimLoadedSet &a, const AnimLoadedSet &b, Report &r) {
	if (a.rows.size() != b.rows.size())
		r.differ("the table holds " + std::to_string(a.rows.size()) + " rows vs " +
				std::to_string(b.rows.size()));
	const size_t rows = std::min(a.rows.size(), b.rows.size());
	for (size_t i = 0; i < rows; ++i) {
		// Row order is the ring's order and the slot-0 reset's place, so the
		// rows compare in order, each by the slot its key names past the first
		// five characters (`ANIM_RESET`, `xxxx_reset` and `anim_reset` are one
		// slot) [orig: AnimMap_FindSlotByName @0x40cfa0, stricmp on key + 5].
		if (opennova::adm::adm_slot_key(a.rows[i].key) !=
				opennova::adm::adm_slot_key(b.rows[i].key)) {
			r.differ("row " + std::to_string(i) + ": '" + a.rows[i].key + "' vs '" +
					b.rows[i].key + "'");
			continue;
		}
		const std::vector<std::string> &va = a.rows[i].variants;
		const std::vector<std::string> &vb = b.rows[i].variants;
		if (va.size() != vb.size()) {
			r.differ("row '" + a.rows[i].key + "': " + std::to_string(va.size()) +
					" variants vs " + std::to_string(vb.size()));
			continue;
		}
		for (size_t v = 0; v < va.size(); ++v) {
			// A row names its clip with or without the .bad the file carries
			// (440 of 5146 retail variants omit it); the clip is the same.
			if (!opennova::strutil::iequals(bad_build_clip_stem(va[v]), bad_build_clip_stem(vb[v])))
				r.differ("row '" + a.rows[i].key + "' variant " + std::to_string(v) + ": '" +
						va[v] + "' vs '" + vb[v] + "'");
		}
	}
}

} // namespace

int cmd_anim_compare(const char *expected_path, const char *actual_path) {
	AnimLoadedSet a;
	AnimLoadedSet b;
	std::string error;
	if (!anim_load(expected_path, a, error)) {
		std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
		return 1;
	}
	if (!anim_load(actual_path, b, error)) {
		std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
		anim_free(a);
		return 1;
	}

	Report r;
	compare_rows(a, b, r);
	// A variant whose clip did not load is no animation to compare: it
	// differs, on either side.
	for (const AnimMissingClip &absent : a.missing)
		r.differ("expected table names '" + absent.variant + "': " + absent.reason);
	for (const AnimMissingClip &absent : b.missing)
		r.differ("actual table names '" + absent.variant + "': " + absent.reason);
	if (a.table_name.empty() && b.table_name.empty() && a.clips.size() == 1 && b.clips.size() == 1) {
		// Two lone clips are one clip each, whatever their files are called.
		compare_clip(a.clips[0], b.clips[0], r);
	} else {
		for (const AnimLoadedClip &clip : a.clips) {
			const AnimLoadedClip *other = find_clip(b, clip.name);
			if (other == nullptr) {
				r.differ("clip '" + clip.name + "' is missing");
				continue;
			}
			compare_clip(clip, *other, r);
		}
		for (const AnimLoadedClip &clip : b.clips) {
			if (find_clip(a, clip.name) == nullptr) r.differ("clip '" + clip.name + "' is unexpected");
		}
	}

	std::printf("%zu rows, %zu clips: worst rotation %g degrees%s, worst vector %g m\n",
			a.rows.size(), a.clips.size(), r.worst_angle * 180.0 / 3.14159265358979323846,
			r.worst_angle_at.empty() ? "" : (" at " + r.worst_angle_at).c_str(), r.worst_vector);
	anim_free(a);
	anim_free(b);
	if (r.differences.empty()) {
		std::printf("the same animation\n");
		return 0;
	}
	for (const std::string &d : r.differences) std::printf("differs: %s\n", d.c_str());
	std::printf("%zu differences%s\n", r.differences.size(),
			r.differences.size() >= 40 ? " (the first 40)" : "");
	return 1;
}

} // namespace threedi_cli
