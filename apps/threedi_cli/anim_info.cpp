// opennova-3di anim info: print a clip set — the table's rows and every clip's
// header, bone table, channels, events and translations. `--verbose` adds the
// per-bone rows, `--keys` every key.

#include "anim_cli.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/adm/adm.h>
#include <formats/bad/bad_build.h>

using namespace opennova::bad;

namespace threedi_cli {

namespace {

const char *flag_words(uint32_t flags, std::string &out) {
	out.clear();
	if ((flags & BAD_FLAG_LOOP) != 0) out += " loop";
	if ((flags & BAD_FLAG_TRANSLATION) != 0) out += " translations";
	if ((flags & BAD_FLAG_BIT3) != 0) out += " bit3";
	const uint32_t known = BAD_FLAG_LOOP | BAD_FLAG_TRANSLATION | BAD_FLAG_BIT3;
	if ((flags & ~known) != 0) out += " other";
	return out.c_str();
}

void print_clip(const AnimLoadedClip &clip, int verbose) {
	const BadFile &f = clip.file;
	std::string words;
	std::printf("clip %s: version %u, fps %u, frames %u, flags 0x%x%s, bones %u, events %zu%s\n",
			clip.name.c_str(), f.version, f.fps, f.frame_count, static_cast<unsigned>(f.flags),
			flag_words(f.flags, words), f.bone_count, f.num_events,
			f.num_translations != 0 ? ", translated" : "");
	const size_t keys = static_cast<size_t>(f.frame_count) + 1;
	size_t sparse = 0;
	size_t nonuniform = 0;
	for (size_t i = 0; i < f.num_channels; ++i) {
		const BadChannel &ch = f.channels[i];
		if (ch.frame_count != keys) ++sparse;
		for (uint32_t k = 0; k < ch.frame_count; ++k) {
			if (ch.frame_lengths != nullptr && ch.frame_lengths[k] != 1) {
				++nonuniform;
				break;
			}
		}
	}
	if (sparse != 0 || nonuniform != 0)
		std::printf("  channels: %zu key sparsely, %zu carry a duration other than one\n", sparse,
				nonuniform);
	if (f.num_events != 0) {
		float bottom_min = f.events[0].bottom, bottom_max = f.events[0].bottom;
		float top_min = f.events[0].top, top_max = f.events[0].top;
		double travel = 0.0;
		size_t triggers = 0;
		for (size_t e = 0; e < f.num_events; ++e) {
			const BadEvent &ev = f.events[e];
			bottom_min = ev.bottom < bottom_min ? ev.bottom : bottom_min;
			bottom_max = ev.bottom > bottom_max ? ev.bottom : bottom_max;
			top_min = ev.top < top_min ? ev.top : top_min;
			top_max = ev.top > top_max ? ev.top : top_max;
			travel += std::sqrt(static_cast<double>(ev.velocity[0]) * ev.velocity[0] +
					static_cast<double>(ev.velocity[1]) * ev.velocity[1] +
					static_cast<double>(ev.velocity[2]) * ev.velocity[2]);
			if (ev.trigger > 0) ++triggers;
		}
		std::printf("  events: capsule bottom %g..%g, top %g..%g, travel %g m, %zu triggers\n",
				bottom_min, bottom_max, top_min, top_max, travel, triggers);
	}
	if (verbose == 0) return;
	for (size_t i = 0; i < f.num_bones; ++i) {
		const BadBone &b = f.bones[i];
		const BadChannel &ch = f.channels[i];
		std::printf("  %2zu %-32s parent %3d length %8.4f position %9.4f %9.4f %9.4f keys %u\n", i,
				b.name, b.parent_index, b.length, b.position[0], b.position[1], b.position[2],
				ch.frame_count);
		if (verbose < 2) continue;
		for (uint32_t k = 0; k < ch.frame_count; ++k) {
			const BadQuaternion &q = ch.rotations[k];
			std::printf("      key %3u  %9.6f %9.6f %9.6f %9.6f  x%u\n", k, q.x, q.y, q.z, q.w,
					ch.frame_lengths != nullptr ? ch.frame_lengths[k] : 1);
		}
	}
	if (verbose < 2) return;
	for (size_t e = 0; e < f.num_events; ++e) {
		const BadEvent &ev = f.events[e];
		std::printf("      event %3zu velocity %9.5f %9.5f %9.5f bottom %8.4f top %8.4f trigger "
					"0x%x\n",
				e, ev.velocity[0], ev.velocity[1], ev.velocity[2], ev.bottom, ev.top,
				static_cast<unsigned>(ev.trigger));
	}
}

} // namespace

std::string anim_info_row(const BadBuildRow &row) {
	char key[48];
	std::snprintf(key, sizeof(key), "%-40s", row.key.c_str());
	std::string out = key;
	// The ring serves a row from its last variant back, but slot 0 (`reset`)
	// is no ring: each reset variant replaces the head, so the last is the
	// rig's bind [orig: AnimMap_RegisterBoneNode @0x40C2D0, slot 0 self-rings
	// @0x40c38b].
	const bool reset = opennova::adm::adm_key_names_slot(row.key, "reset");
	if (reset && row.variants.size() > 1) {
		for (const std::string &variant : row.variants) out += " " + variant;
		return out + "   (no ring: the last is the rig's bind)";
	}
	for (size_t v = row.variants.size(); v-- > 0;) out += " " + row.variants[v];
	return out + (row.variants.size() > 1 ? "   (ring order)" : "");
}

int cmd_anim_info(const char *in_path, int verbose) {
	AnimLoadedSet set;
	std::string error;
	if (!anim_load(in_path, set, error)) {
		std::fprintf(stderr, "opennova-3di: %s\n", error.c_str());
		return 1;
	}
	if (!set.table_name.empty()) {
		std::printf("table %s: %zu rows, %zu clips\n", set.table_name.c_str(), set.rows.size(),
				set.clips.size());
		for (const BadBuildRow &row : set.rows) std::printf("  %s\n", anim_info_row(row).c_str());
		for (const AnimMissingClip &absent : set.missing)
			std::printf("  missing: %s (%s)\n", absent.variant.c_str(), absent.reason.c_str());
	}
	for (const AnimLoadedClip &clip : set.clips) print_clip(clip, verbose);
	anim_free(set);
	return 0;
}

} // namespace threedi_cli
