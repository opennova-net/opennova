// Corpus round-trip test: parse -> write -> compare byte-exact across a directory of REAL shipped
// .bms missions, then reparse and confirm bms::equal. The single committed fixture (ash_i5b) proves
// the format on one file; this proves it across the whole shipped set -- every climate, area-trigger
// count, weapon-loadout / secondary-chunk variation, and event/trigger/action/bbox count that appears
// in the wild. The engine loader was grilled byte-for-byte (Mission_LoadBMSFile @0x40f7b6,
// EventTrigger_LoadAllData @0x453eb0, Jointops.exe); this is the data-side proof on real data.
//
// The corpus is copyrighted game data and is NOT committed. The test is gated on the env var
// OPENNOVA_MISSION_CORPUS (point it at e.g. an extracted JO_ASSETS dir). When the var is unset the
// test prints a skip line and passes, so it never runs bare in CI. Mirrors the env-gated pattern in
// tests/oed/export_3di_test.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mission/bms.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.good()) {
		return {};
	}
	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	if (size > 0 && !file.read(reinterpret_cast<char *>(data.data()), size)) {
		return {};
	}
	return data;
}

bool has_bms_extension(const fs::path &path) {
	std::string ext = path.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext == ".bms";
}

// Round-trips one mission. Returns true on success; logs the first divergence on failure.
bool check_mission(const fs::path &path) {
	const std::string name = path.filename().string();
	const std::vector<uint8_t> original = read_file(path);
	if (original.empty()) {
		std::fprintf(stderr, "  FAIL %s: could not read file\n", name.c_str());
		return false;
	}
	if (!opennova::bms::is_bms(original.data(), original.size())) {
		std::fprintf(stderr, "  FAIL %s: not recognized as BMS (magic/size)\n", name.c_str());
		return false;
	}

	opennova::bms::File parsed;
	std::string error;
	if (!opennova::bms::parse(original.data(), original.size(), parsed, error)) {
		std::fprintf(stderr, "  FAIL %s: parse failed: %s\n", name.c_str(), error.c_str());
		return false;
	}

	// Pool vectors must match the header counts, and the fixed sections are always full-size.
	if (parsed.items.size() != parsed.header.num_items ||
	    parsed.buildings.size() != parsed.header.num_buildings ||
	    parsed.markers.size() != parsed.header.num_markers ||
	    parsed.organics.size() != parsed.header.num_people) {
		std::fprintf(stderr, "  FAIL %s: pool counts != header counts\n", name.c_str());
		return false;
	}
	if (parsed.waypoint_records.size() != static_cast<size_t>(opennova::bms::kWaypointRecordCount) ||
	    parsed.group_records.size() != static_cast<size_t>(opennova::bms::kGroupRecordCount) ||
	    parsed.layer_records.size() != static_cast<size_t>(opennova::bms::kLayerRecordCount)) {
		std::fprintf(stderr, "  FAIL %s: fixed section count wrong (wp/grp/layer)\n", name.c_str());
		return false;
	}

	std::vector<uint8_t> encoded;
	if (!opennova::bms::write(parsed, encoded, error)) {
		std::fprintf(stderr, "  FAIL %s: write failed: %s\n", name.c_str(), error.c_str());
		return false;
	}
	if (encoded.size() != original.size()) {
		std::fprintf(stderr, "  FAIL %s: re-encoded size %zu != original %zu\n", name.c_str(),
		             encoded.size(), original.size());
		return false;
	}
	if (std::memcmp(encoded.data(), original.data(), original.size()) != 0) {
		size_t off = 0;
		while (off < original.size() && encoded[off] == original[off]) {
			++off;
		}
		std::fprintf(stderr, "  FAIL %s: byte mismatch at offset %zu (orig=0x%02X enc=0x%02X)\n",
		             name.c_str(), off, original[off], encoded[off]);
		return false;
	}

	// The re-encoded bytes must reparse, and equal() (the editor's undo/dirty change-detector) must
	// agree that nothing changed across the round-trip.
	opennova::bms::File reparsed;
	if (!opennova::bms::parse(encoded.data(), encoded.size(), reparsed, error)) {
		std::fprintf(stderr, "  FAIL %s: reparse of re-encoded bytes failed: %s\n", name.c_str(),
		             error.c_str());
		return false;
	}
	if (!opennova::bms::equal(parsed, reparsed)) {
		std::fprintf(stderr, "  FAIL %s: bms::equal(parsed, reparsed) is false\n", name.c_str());
		return false;
	}

	return true;
}

} // namespace

int main() {
	const char *corpus_env = std::getenv("OPENNOVA_MISSION_CORPUS");
	if (corpus_env == nullptr || corpus_env[0] == '\0') {
		std::fprintf(stderr,
		             "mission_corpus: skipped (set OPENNOVA_MISSION_CORPUS to a dir of real .bms files)\n");
		return 0;
	}

	const fs::path corpus_dir(corpus_env);
	std::error_code ec;
	if (!fs::is_directory(corpus_dir, ec)) {
		std::fprintf(stderr, "mission_corpus: OPENNOVA_MISSION_CORPUS is not a directory: %s\n",
		             corpus_env);
		return 1;
	}

	std::vector<fs::path> missions;
	for (fs::recursive_directory_iterator it(corpus_dir, ec), end; it != end; it.increment(ec)) {
		if (ec) {
			break;
		}
		if (it->is_regular_file(ec) && has_bms_extension(it->path())) {
			missions.push_back(it->path());
		}
	}
	std::sort(missions.begin(), missions.end());

	if (missions.empty()) {
		std::fprintf(stderr, "mission_corpus: no .bms files found under %s\n", corpus_env);
		return 1;
	}

	std::fprintf(stderr, "mission_corpus: checking %zu missions under %s\n", missions.size(),
	             corpus_env);
	size_t failed = 0;
	for (const fs::path &mission : missions) {
		if (!check_mission(mission)) {
			++failed;
		}
	}

	std::fprintf(stderr, "mission_corpus: %zu/%zu round-tripped byte-exact (%zu failed)\n",
	             missions.size() - failed, missions.size(), failed);
	return failed == 0 ? 0 : 1;
}
