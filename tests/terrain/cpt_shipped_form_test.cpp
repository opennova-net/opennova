// The .cpt form the shipped JO:CA polydata takes (fixtures/terrain/variants/shipped.cpt, minted by
// tests/fixtures/terrain_variants_gen.cpp after the census of the extracted tree's 23 .cpt; and the Tmap map's
// own fixtures/terrain/tmap/Tmap.cpt): each parses through the CDEP/DPTH/POLY reader into the 1024 x 1024 depth
// atlas with its 341 tiles, and the shipped form carries the header facts the reader's contract was grilled on
// (Dvxi5.cpt's terrain name "Dvxi5", creator "Brophy" and version "jbrophy", the reserved bytes 00 04 05 00, CDEP).
// Each file read by load_cpt and written again by save_cpt (the writer behind CptFile::write_bytes and the TrnGen
// bake) is the same bytes: every CDEP block width the shipped files take (1 to 15), a block steeper than a block
// holds read back clamped, strip and list LODs, a chunk of indices past the 12-bit field its least one is stored in. The bytes are pinned (FNV-1a64): a changed vector is a changed CPT
// writer and needs an [orig] witness or a D-entry in the same commit (`--dump` prints the table).

#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>

#include <base/io/hash.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/test_paths.h"

namespace {

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

const Vector kExpected[] = {
	{"variants/shipped.cpt", 255983u, 0x459c750bf5427d4eull},
	{"tmap/Tmap.cpt", 738781u, 0x47f26730fb433697ull},
};

int failures = 0;

void expect(bool ok, const std::string &what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what.c_str());
		++failures;
	}
}

// The CDEP block widths the file's depth section holds (bit w set for a block w bits wide).
unsigned cdep_widths(const std::vector<uint16_t> &depth) {
	unsigned widths = 0;
	for (size_t start = 0; start + 256 <= depth.size(); start += 256) {
		const auto [low, high] = std::minmax_element(depth.begin() + long(start), depth.begin() + long(start + 256));
		int range = int(*high) - int(*low), width = 0;
		for (int v = range + 1; v; v >>= 1) ++width;
		widths |= 1u << width;
	}
	return widths;
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	std::vector<std::pair<std::string, std::vector<uint8_t>>> written;
	for (const char *rel : {"variants/shipped.cpt", "tmap/Tmap.cpt"}) {
		const std::string path = test_paths_fixture(std::string("terrain/") + rel);
		opennova::CptFile cpt;
		try {
			cpt = opennova::CptFile::read(path);
		} catch (const std::exception &e) {
			expect(false, std::string(rel) + ": " + e.what());
			continue;
		}
		expect(cpt.depth_buffer.size() == 1024u * 1024u && cpt.tiles.size() == 341u,
		       std::string(rel) + ": the 1024 x 1024 atlas and 341 tiles");
		const std::vector<uint8_t> bytes = test_io::read_file(path);
		opennova::CptFile loaded;
		std::string error;
		std::vector<uint8_t> again;
		expect(opennova::load_cpt(bytes.data(), bytes.size(), loaded, error) && opennova::save_cpt(loaded, again, error),
		       std::string(rel) + ": load_cpt / save_cpt: " + error);
		size_t differ = 0, first = 0;
		for (size_t i = 0; i < std::min(bytes.size(), again.size()); ++i)
			if (bytes[i] != again[i] && differ++ == 0) first = i;
		if (again.size() != bytes.size() || differ != 0)
			std::printf("  %s: %zu bytes, written %zu, %zu differ from 0x%zx\n", rel, bytes.size(), again.size(), differ, first);
		expect(again.size() == bytes.size() && differ == 0, std::string(rel) + " written again is its bytes");
		written.emplace_back(rel, again);
		if (std::strcmp(rel, "variants/shipped.cpt") == 0) {
			const opennova::CptFile::Header &h = cpt.header;
			expect(std::string(h.terrain_name) == "Dvxi5" && std::string(h.creator) == "Brophy" &&
			               std::string(h.version) == "jbrophy" && cpt.depth_format == opennova::DepthFormat::CDEP,
			       "shipped.cpt: the shipped header's strings and CDEP");
			const uint8_t reserved[12] = {0, 4, 5, 0};
			expect(std::memcmp(h.reserved, reserved, sizeof(reserved)) == 0, "shipped.cpt: the reserved bytes 00 04 05 00");
			expect(cdep_widths(cpt.depth_buffer) == 0xFFFEu, "shipped.cpt: every CDEP block width 1 to 15");
			expect(opennova::cpt_steep_blocks(cpt.depth_buffer) == 0, "shipped.cpt: its steep block read back clamped");
			bool strip = false, list = false;
			for (const opennova::CptTile &tile : cpt.tiles)
				for (const opennova::CptTileLOD &lod : tile.lods) (lod.is_strip ? strip : list) = true;
			expect(strip && list, "shipped.cpt: strip and list LODs");
			// A tile past 4,096 vertices whose 48-index chunks' least index passes the 12-bit field the writer
			// stores it in (4,095 written, the deltas widened), as the shipped files hold them.
			bool past_field = false;
			for (const opennova::CptTile &tile : cpt.tiles)
				for (const opennova::CptTileLOD &lod : tile.lods)
					for (size_t at = 0; at < lod.indices.size(); at += 48)
						past_field = past_field || *std::min_element(lod.indices.begin() + long(at),
						                                             lod.indices.begin() + long(std::min(at + 48, lod.indices.size()))) > 4095;
			expect(past_field, "shipped.cpt: a chunk whose least index passes 4,095");
		}
	}
	if (dump) {
		for (const auto &[name, bytes] : written)
			std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", name.c_str(), bytes.size(),
			            (unsigned long long)opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size()));
		return 0;
	}
	const size_t expected = sizeof(kExpected) / sizeof(kExpected[0]);
	expect(written.size() == expected, "every committed vector has its file");
	for (size_t i = 0; i < written.size() && i < expected; ++i) {
		const std::vector<uint8_t> &bytes = written[i].second;
		const uint64_t h = opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size());
		if (written[i].first != kExpected[i].name || bytes.size() != kExpected[i].len || h != kExpected[i].hash)
			std::printf("  %s: len %zu hash 0x%016llx, expected len %zu hash 0x%016llx\n", written[i].first.c_str(),
			            bytes.size(), (unsigned long long)h, kExpected[i].len, (unsigned long long)kExpected[i].hash);
		expect(written[i].first == kExpected[i].name && bytes.size() == kExpected[i].len && h == kExpected[i].hash,
		       written[i].first + ": the pinned vector");
	}
	std::printf("cpt_shipped_form: %zu polydata files written again byte-identical, %d failures\n", written.size(),
	            failures);
	return failures == 0 ? 0 : 1;
}
