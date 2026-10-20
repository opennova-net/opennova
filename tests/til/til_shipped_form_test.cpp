// The .til form every shipped JO:CA tile overlay takes (fixtures/terrain/variants/med.til, minted by
// tests/fixtures/terrain_variants_gen.cpp after the census of the extracted tree's 116 .til): it reads with MED's
// header (+8 = 12 * count + 16, +12 = 0, no byte past the records), its records grouped by ascending layer (MED's
// write order; the shipped files' layers 0 to 30), every flag combination within the game's three bits, whole-unit
// positions either side of the origin and an index on a 64-cell atlas (0 to 63), and written again by save_til it is
// the same bytes, the header computed from the model [orig: JOTACmed.exe MapTileStore_Serialize @ 0x433EC0]. The
// bytes are pinned (FNV-1a64): a changed vector is a changed .til writer and needs an [orig] witness or a D-entry in
// the same commit (`--dump` prints it).

#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include <base/io/hash.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/test_paths.h"

namespace {

constexpr size_t kExpectedLen = 172u;
constexpr uint64_t kExpectedHash = 0xad802852ab5a2e9full;

bool check(bool ok, const char *what, int &failures) {
	if (!ok) {
		std::printf("FAIL: med.til: %s\n", what);
		++failures;
	}
	return ok;
}

} // namespace

int main(int argc, char **argv) {
	const std::vector<uint8_t> bytes = test_io::read_file(test_paths_fixture("terrain/variants/med.til"));
	int failures = 0;
	opennova::TilFile til;
	opennova::TilHeaderFacts facts;
	std::string error;
	if (!check(opennova::load_til(bytes.data(), bytes.size(), til, error, &facts), "load_til refused it", failures))
		return 1;
	check(facts.size_word == 12u * til.entries.size() + 16u && facts.word12 == 0 && facts.trailing == 0,
	      "the header is not MED's", failures);
	bool ascending = true, flags_ok = true, whole = true, index_ok = true;
	unsigned flag_set = 0;
	uint16_t top_layer = 0;
	bool west = false, east = false;
	for (size_t i = 0; i < til.entries.size(); ++i) {
		const opennova::TilOverlayEntry &e = til.entries[i];
		if (i > 0 && e.layer < til.entries[i - 1].layer) ascending = false;
		if (e.flags > 7) flags_ok = false;
		if ((e.x_fixed & 0xFFFF) != 0 || (e.z_fixed & 0xFFFF) != 0) whole = false;
		if (e.tile_index >= 64) index_ok = false;
		flag_set |= 1u << (e.flags & 7);
		top_layer = e.layer > top_layer ? e.layer : top_layer;
		west |= e.x_fixed < 0, east |= e.x_fixed > 0;
	}
	check(ascending, "records not grouped by ascending layer", failures);
	check(flags_ok, "a flags byte past the game's three bits", failures);
	check(whole, "a position off whole units", failures);
	check(index_ok, "an index past a 64-cell atlas", failures);
	check(flag_set == 0xFFu && top_layer == 30 && west && east, "not every form the shipped records take", failures);
	std::vector<uint8_t> again;
	if (check(opennova::save_til(til, again, error), "save_til refused it", failures))
		check(again == bytes, "written again differs", failures);
	const uint64_t hash = opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, again.data(), again.size());
	if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
		std::printf("constexpr size_t kExpectedLen = %zuu;\nconstexpr uint64_t kExpectedHash = 0x%016llxull;\n", again.size(),
		            (unsigned long long)hash);
		return 0;
	}
	check(again.size() == kExpectedLen && hash == kExpectedHash, "the bytes written again are not the pinned vector",
	      failures);
	std::printf("%s: %zu records swept, %d failures\n", failures == 0 ? "OK" : "FAIL", til.entries.size(), failures);
	return failures == 0 ? 0 : 1;
}
