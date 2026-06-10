// Corpus canonicalization test: parse -> write -> parse -> write across a directory of REAL shipped
// .bms missions. The first write is the OpenNova canonical form; the second write must be byte-identical
// to that canonical form and bms::equal must report the same modeled mission.
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

// Canonicalizes one mission. Returns true on success; logs the first divergence on failure.
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
	// The canonical bytes must reparse, and equal() (the editor's undo/dirty change-detector) must
	// agree that nothing changed across the canonical round-trip.
	opennova::bms::File reparsed;
	if (!opennova::bms::parse(encoded.data(), encoded.size(), reparsed, error)) {
		std::fprintf(stderr, "  FAIL %s: reparse of re-encoded bytes failed: %s\n", name.c_str(),
		             error.c_str());
		return false;
	}
	std::vector<uint8_t> encoded2;
	if (!opennova::bms::write(reparsed, encoded2, error)) {
		std::fprintf(stderr, "  FAIL %s: rewrite of canonical form failed: %s\n", name.c_str(), error.c_str());
		return false;
	}
	if (encoded2 != encoded) {
		size_t off = 0;
		while (off < encoded.size() && off < encoded2.size() && encoded[off] == encoded2[off]) {
			++off;
		}
		std::fprintf(stderr, "  FAIL %s: canonical rewrite differs at offset %zu\n", name.c_str(), off);
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

	std::fprintf(stderr, "mission_corpus: %zu/%zu canonicalized idempotently (%zu failed)\n",
	             missions.size() - failed, missions.size(), failed);
	return failed == 0 ? 0 : 1;
}
