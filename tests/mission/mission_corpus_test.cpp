// The mission writer against the files it must write: parse -> write compared with the bytes read,
// section by section, then the canonical fixed point (parse -> write -> parse -> write: the second
// write the first's bytes, bms::equal agreeing).
//
// The ordinary row reads the minted fixtures/bms/synth_dense.bms. The retail leg (a SKIP-LEG without
// OPENNOVA_JO_DIR) reads every mission the game install ships, out of its archives through the VFS,
// the base game's and each expansion's: every one rewrites to its own bytes but the ones
// kLoadoutDamaged names, whose weapon loadout chunk ends inside a record or holds bytes past its
// terminator (D-EVT-7, docs/mission/bms-event-runtime-re.md 6.3a), each differing in that chunk and
// in the header's length of it alone. A mission that differs and is not named fails, and so does a
// named one that no longer differs.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_paths.h"

namespace fs = std::filesystem;
namespace bms = opennova::bms;

namespace {

// The shipped missions whose loadout chunk the writer does not give back, by stem (upper case): the
// chunk as shipped is damaged, and the writer writes the records the sanitizer makes of it.
const char *const kLoadoutDamaged[] = {"ASP_G8A", "ASR_C2A", "TDH_G1A", "TKR_G3A", "TKX_G5B"};

uint16_t u16_at(const std::vector<uint8_t> &bytes, size_t offset) {
	return uint16_t(bytes[offset] | (bytes[offset + 1] << 8));
}
int32_t i32_at(const std::vector<uint8_t> &bytes, size_t offset) {
	return int32_t(uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) |
	               (uint32_t(bytes[offset + 3]) << 24));
}

// One section of a written mission: its name and its bytes' place.
struct Section {
	const char *name;
	size_t at = 0, size = 0;
};

// The sections of `bytes` in the order the loader reads them [orig: Mission_LoadBMSFile @0x40f7b6],
// sized by the counts the bytes themselves hold; false where they run past the end.
bool sections_of(const std::vector<uint8_t> &bytes, std::vector<Section> &out) {
	out.clear();
	if (bytes.size() < bms::kHeaderSize) return false;
	size_t at = 0;
	const auto take = [&](const char *name, size_t size) {
		out.push_back({name, at, size});
		at += size;
		return at <= bytes.size();
	};
	const auto u32 = [&](size_t offset) { return size_t(uint32_t(i32_at(bytes, offset))); };
	if (!take("header", bms::kHeaderSize)) return false;
	if (!take("loadout chunk", u16_at(bytes, offsetof(bms::Header, weapon_loadout_chunk_len)))) return false;
	if (!take("availability chunk", u16_at(bytes, offsetof(bms::Header, secondary_chunk_len)))) return false;
	if (!take("items", u32(offsetof(bms::Header, num_items)) * bms::kEntitySize)) return false;
	if (!take("buildings", u32(offsetof(bms::Header, num_buildings)) * bms::kEntitySize)) return false;
	if (!take("markers", u32(offsetof(bms::Header, num_markers)) * bms::kEntitySize)) return false;
	if (!take("organics", u32(offsetof(bms::Header, num_people)) * bms::kEntitySize)) return false;
	if (!take("waypoint paths", size_t(bms::kWaypointRecordCount) * bms::kWaypointRecordSize)) return false;
	if (!take("groups", size_t(bms::kGroupRecordCount) * bms::kGroupRecordSize)) return false;
	if (!take("layers", size_t(bms::kLayerRecordCount) * bms::kLayerRecordSize)) return false;
	if (!take("area triggers", size_t(u16_at(bytes, offsetof(bms::Header, area_trigger_count))) * bms::kAreaTriggerSize))
		return false;
	if (at + 12 > bytes.size()) return false;
	const size_t events = u32(at), triggers = u32(at + 4), actions = u32(at + 8);
	if (!take("event counts", 12)) return false;
	if (!take("events", events * bms::kEventSize)) return false;
	if (!take("triggers", triggers * bms::kTriggerSize)) return false;
	if (!take("actions", actions * bms::kActionSize)) return false;
	if (at + 4 > bytes.size()) return false;
	const size_t boxes = u32(at);
	if (!take("bounding box count", 4)) return false;
	if (!take("bounding boxes", boxes * bms::kBoundingBoxSize)) return false;
	return at == bytes.size();
}

// The sections of `a` that `b` writes otherwise, by name, in file order. The header is compared
// apart from its two chunk lengths (`header lengths` names a difference there), so a chunk that
// differs is one difference, not two.
bool differing_sections(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, std::vector<std::string> &out) {
	std::vector<Section> left, right;
	if (!sections_of(a, left) || !sections_of(b, right) || left.size() != right.size()) return false;
	for (size_t i = 0; i < left.size(); ++i) {
		const bool same = left[i].size == right[i].size &&
		                  std::memcmp(a.data() + left[i].at, b.data() + right[i].at, left[i].size) == 0;
		if (i == 0 && !same) {
			std::vector<uint8_t> x(a.begin(), a.begin() + bms::kHeaderSize), y(b.begin(), b.begin() + bms::kHeaderSize);
			for (const size_t offset : {offsetof(bms::Header, weapon_loadout_chunk_len), offsetof(bms::Header, secondary_chunk_len)})
				x[offset] = x[offset + 1] = y[offset] = y[offset + 1] = 0;
			out.push_back(x == y ? "header lengths" : "header");
		} else if (!same) {
			out.push_back(left[i].name);
		}
	}
	return true;
}

struct Counts {
	size_t missions = 0, identical = 0, failed = 0;
	std::vector<std::string> differing; // "NAME: section, section"
};

// One mission: parsed, written, compared with the bytes read; then the canonical fixed point. `damaged`
// says the writer is known not to give this one's loadout chunk back.
void check_mission(const std::string &name, const std::vector<uint8_t> &original, bool damaged, Counts &counts) {
	++counts.missions;
	const auto fail = [&](const std::string &why) {
		std::fprintf(stderr, "  FAIL %s: %s\n", name.c_str(), why.c_str());
		++counts.failed;
	};
	bms::File parsed;
	std::string error;
	if (!bms::is_bms(original.data(), original.size()) ||
	    !bms::parse(original.data(), original.size(), parsed, error))
		return fail("does not parse: " + error);

	// Pool vectors must match the header counts, and the fixed sections are always full-size.
	if (parsed.items.size() != parsed.header.num_items || parsed.buildings.size() != parsed.header.num_buildings ||
	    parsed.markers.size() != parsed.header.num_markers || parsed.organics.size() != parsed.header.num_people)
		return fail("pool counts != header counts");
	if (parsed.waypoint_records.size() != size_t(bms::kWaypointRecordCount) ||
	    parsed.group_records.size() != size_t(bms::kGroupRecordCount) ||
	    parsed.layer_records.size() != size_t(bms::kLayerRecordCount))
		return fail("fixed section count wrong (wp/grp/layer)");

	// Former GUT waypoint-view oracle, including CP19's authored count of
	// 39: preserve the raw record, but bound the public view to its 32 slots.
	for (const auto &summary : opennova::mission::waypoint_summaries(parsed)) {
		opennova::mission::WaypointPath waypoint;
		if (summary.marker_count < 0 || summary.marker_count > 32 ||
		    !opennova::mission::waypoint_path(parsed, summary.index, waypoint) || waypoint.marker_indices.size() > 32)
			return fail("waypoint view exceeds its stored slots");
	}

	std::vector<uint8_t> encoded;
	if (!bms::write(parsed, encoded, error)) return fail("write failed: " + error);

	// The bytes read, written again.
	if (encoded == original) {
		++counts.identical;
		if (damaged) fail("named as one whose loadout chunk the writer does not give back, and it does: drop its name");
	} else {
		std::vector<std::string> sections;
		if (!differing_sections(original, encoded, sections)) return fail("the rewrite's sections do not line up");
		std::string list;
		for (const std::string &section : sections) list += (list.empty() ? "" : ", ") + section;
		counts.differing.push_back(name + ": " + list);
		const bool loadout_alone =
		        sections == std::vector<std::string>{"header lengths", "loadout chunk"} ||
		        sections == std::vector<std::string>{"loadout chunk"};
		if (!damaged || !loadout_alone) fail("rewrite differs from the bytes read in: " + list);
	}

	// The canonical bytes must reparse, and equal() (the editor's undo/dirty change-detector) must
	// agree that nothing changed across the canonical round-trip.
	bms::File reparsed;
	if (!bms::parse(encoded.data(), encoded.size(), reparsed, error)) return fail("reparse of the rewrite failed: " + error);
	std::vector<uint8_t> encoded2;
	if (!bms::write(reparsed, encoded2, error)) return fail("rewrite of the canonical form failed: " + error);
	if (encoded2 != encoded) {
		size_t off = 0;
		while (off < encoded.size() && off < encoded2.size() && encoded[off] == encoded2[off]) ++off;
		return fail("canonical rewrite differs at offset " + std::to_string(off));
	}
	if (!bms::equal(parsed, reparsed)) return fail("bms::equal(parsed, reparsed) is false");
}

int test_minted() {
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms");
	if (bytes.empty()) {
		std::fprintf(stderr, "mission_corpus: fixtures/bms/synth_dense.bms is missing\n");
		return 1;
	}
	Counts counts;
	check_mission("synth_dense.bms", bytes, false, counts);
	// The section walk names what a change touches: a byte of the first item moved names its pool.
	std::vector<uint8_t> moved = bytes;
	std::vector<Section> sections;
	std::vector<std::string> differing;
	if (!sections_of(bytes, sections) || sections.size() != 17 || sections[3].size == 0) return 1;
	moved[sections[3].at + 20] ^= 0x01;
	if (!differing_sections(bytes, moved, differing) || differing != std::vector<std::string>{"items"}) return 1;
	std::printf("minted: synth_dense.bms rewrites to its own bytes, %zu sections\n", sections.size());
	return counts.failed == 0 && counts.identical == 1 ? 0 : 1;
}

int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every mission the game install ships, rewritten)");
	std::set<std::string> damaged(std::begin(kLoadoutDamaged), std::end(kLoadoutDamaged));
	// The base game's missions and each expansion's (each file once, by its archive and name).
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen, met;
	Counts counts;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		if (!game.mount_game(root, expansion, opennova::VfsMountMode::Packed)) {
			std::fprintf(stderr, "mission_corpus: the install does not mount: %s\n", root.c_str());
			return 1;
		}
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			const fs::path path(file.logical_name);
			if (retail::lower_ascii(path.extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			if (!game.read_file(file.logical_name, bytes)) {
				std::fprintf(stderr, "  FAIL %s: could not read it out of its archive\n", file.logical_name.c_str());
				++counts.failed;
				continue;
			}
			std::string stem = path.stem().string();
			std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::toupper(c)); });
			const bool named = damaged.count(stem) != 0;
			if (named) met.insert(stem);
			check_mission(file.logical_name, bytes, named, counts);
		}
	}
	if (counts.missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	for (const std::string &line : counts.differing) std::printf("  differs: %s\n", line.c_str());
	std::printf("retail: %zu missions, %zu rewritten to their own bytes, %zu differing in the loadout chunk alone "
	            "(%zu of the %zu named are in this install), %zu failed\n",
	            counts.missions, counts.identical, counts.differing.size(), met.size(), damaged.size(), counts.failed);
	return counts.failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_minted() != 0) return 1;
	return test_retail();
}
