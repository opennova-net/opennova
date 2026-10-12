// Retail .til sweep: every .til directly under the extracted asset tree reads with MED's header
// (+8 = 12 * count + 16, +12 = 0, no byte past the records), its records grouped by ascending layer
// (MED's write order), flags within the game's three bits, whole-unit positions and an index on a
// 64-cell atlas, and written again by save_til it is the same bytes, the header computed from the
// model [orig: JOTACmed MapTileStore_Serialize @ 0x433EC0]. Reports Skipped without OPENNOVA_JO_ASSETS.

#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

bool is_til(const std::filesystem::path &path) {
	std::string ext = path.extension().string();
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".til";
}

bool check(bool ok, const std::string &name, const char *what, int &failures) {
	if (!ok) {
		std::printf("FAIL: %s: %s\n", name.c_str(), what);
		++failures;
	}
	return ok;
}

} // namespace

int main() {
	const std::string assets = retail::assets();
	if (assets.empty())
		return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose mission .til files)");
	int failures = 0;
	int swept = 0;
	size_t records = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file() || !is_til(entry.path())) continue;
		const std::string name = entry.path().filename().string();
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		opennova::TilFile til;
		opennova::TilHeaderFacts facts;
		std::string error;
		if (!check(opennova::load_til(bytes.data(), bytes.size(), til, error, &facts), name, "load_til refused it",
		           failures))
			continue;
		++swept;
		records += til.entries.size();
		check(facts.size_word == 12u * til.entries.size() + 16u && facts.word12 == 0 && facts.trailing == 0, name,
		      "the header is not MED's", failures);
		bool ascending = true, flags_ok = true, whole = true, index_ok = true;
		for (size_t i = 0; i < til.entries.size(); ++i) {
			const opennova::TilOverlayEntry &e = til.entries[i];
			if (i > 0 && e.layer < til.entries[i - 1].layer) ascending = false;
			if (e.flags > 7) flags_ok = false;
			if ((e.x_fixed & 0xFFFF) != 0 || (e.z_fixed & 0xFFFF) != 0) whole = false;
			if (e.tile_index >= 64) index_ok = false;
		}
		check(ascending, name, "records not grouped by ascending layer", failures);
		check(flags_ok, name, "a flags byte past the game's three bits", failures);
		check(whole, name, "a position off whole units", failures);
		check(index_ok, name, "an index past a 64-cell atlas", failures);
		std::vector<uint8_t> again;
		if (!check(opennova::save_til(til, again, error), name, "save_til refused it", failures)) continue;
		check(again == bytes, name, "written again differs", failures);
	}
	if (swept == 0) {
		std::printf("FAIL: no .til under %s\n", assets.c_str());
		return 1;
	}
	std::printf("%s: %d .til files, %zu records swept, %d failures\n", failures == 0 ? "OK" : "FAIL", swept, records,
	            failures);
	return failures == 0 ? 0 : 1;
}
