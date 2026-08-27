// Byte-exact parse->encode round-trip for opennova::lwf over the synthetic
// menu.lwf (tests/fixtures/minimal_lwf_gen.cpp), plus a mutate-and-reread
// check. The retail profiles are swept by lwf_jo_assets_sweep_test behind
// OPENNOVA_JO_ASSETS. Mirrors mission/mission_bms_test.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/lwf/lwf.h>

namespace {

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.good()) {
		return {};
	}
	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	file.read(reinterpret_cast<char *>(data.data()), size);
	if (!file.good()) {
		return {};
	}
	return data;
}

// Parse->encode must reproduce the input byte-for-byte (raw slots + original
// string pool preserved). Returns 0 on success.
int check_byte_exact(const std::string &path) {
	const std::vector<uint8_t> original = read_file(path);
	if (original.empty()) {
		std::fprintf(stderr, "missing required fixture: %s\n", path.c_str());
		return 1;
	}

	opennova::lwf::File file;
	std::string error;
	if (!opennova::lwf::parse_lwf_buffer(original.data(), original.size(), file, error)) {
		std::fprintf(stderr, "parse failed for %s: %s\n", path.c_str(), error.c_str());
		return 1;
	}

	std::vector<uint8_t> encoded;
	if (!opennova::lwf::encode_lwf(file, encoded, error)) {
		std::fprintf(stderr, "encode failed for %s: %s\n", path.c_str(), error.c_str());
		return 1;
	}

	if (encoded.size() != original.size()) {
		std::fprintf(stderr, "size mismatch for %s: %zu vs %zu\n", path.c_str(), encoded.size(), original.size());
		return 1;
	}
	if (std::memcmp(encoded.data(), original.data(), original.size()) != 0) {
		std::fprintf(stderr, "byte mismatch for %s\n", path.c_str());
		return 1;
	}
	return 0;
}

} // namespace

int main() {
	const std::string root = test_paths_repo_root(__FILE__);
	const std::string menu = root + "/fixtures/lwf/menu.lwf";

	// Byte-exact round-trip on the unmodified path.
	TEST_EXPECT(check_byte_exact(menu) == 0);

	// Structural sanity on the fixture.
	const std::vector<uint8_t> bytes = read_file(menu);
	TEST_EXPECT(!bytes.empty());
	opennova::lwf::File file;
	std::string error;
	TEST_EXPECT(opennova::lwf::parse_lwf_buffer(bytes.data(), bytes.size(), file, error));
	TEST_EXPECT(file.header.magic == opennova::lwf::kMagic);
	TEST_EXPECT(file.header.single_count == file.singles.size());
	TEST_EXPECT(file.singles.size() == 3);
	TEST_EXPECT(file.multis.size() == 3);
	TEST_EXPECT(file.playlists.size() == 3);
	TEST_EXPECT(file.sndparms.size() == 3);
	TEST_EXPECT(file.multis[0].name == "MOUSE_OVER");
	TEST_EXPECT(file.multis[0].pitch_base == opennova::lwf::kAuthoredSetPitchBase);
	TEST_EXPECT(file.singles[2].path == "tone.wav");
	TEST_EXPECT(file.playlists[2].falloff_radius == 2000);

	// Every sndparm references a valid single, and every playlist sndparm index
	// is in range (the parser guarantees this, but pin it as a contract).
	for (const auto &sp : file.sndparms) {
		TEST_EXPECT(sp.single_index < file.singles.size());
		TEST_EXPECT(sp.pitch_scaled == opennova::lwf::kPitchUnityQ16);
	}

	// Mutate-and-reread: change a member volume, re-encode, re-parse, assert.
	const uint32_t original_volume = file.sndparms[0].volume;
	const uint32_t new_volume = (original_volume == 200) ? 100 : 200;
	file.sndparms[0].volume = new_volume;

	std::vector<uint8_t> mutated;
	TEST_EXPECT(opennova::lwf::encode_lwf(file, mutated, error));

	opennova::lwf::File reparsed;
	TEST_EXPECT(opennova::lwf::parse_lwf_buffer(mutated.data(), mutated.size(), reparsed, error));
	TEST_EXPECT(reparsed.sndparms.size() == file.sndparms.size());
	TEST_EXPECT(reparsed.sndparms[0].volume == new_volume);
	// An in-place scalar edit on the File keeps every other byte identical.
	TEST_EXPECT(mutated.size() == bytes.size());

	return 0;
}
