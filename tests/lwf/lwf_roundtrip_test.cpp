// Parse->encode round-trip for opennova::lwf over the synthetic menu.lwf
// (tests/fixtures/minimal_lwf_gen.cpp, our writer's own bytes, so byte-exact),
// plus a mutate-and-reread check, and the stale bytes a tool leaves written
// zero. The retail profiles are swept by lwf_jo_assets_sweep_test behind
// OPENNOVA_JO_ASSETS. Mirrors mission/mission_bms_test.cpp.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/lwf/lwf.h>

#include "common/file_io.h"

namespace {

using test_io::read_file;

// Parse->encode reproduces a bank our writer made byte-for-byte. Returns 0 on
// success.
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

	// --- Banks the engine opens that the authoring tools never wrote -----------------------

	// A bank whose magic is not 'LWF1': the engine checks the magic nowhere on open; it only
	// gates the 256-byte filename table [orig: SoundBank_LoadTriggerSets @ 0x75c671], so the
	// sets, layers and members load and no single names a file.
	{
		std::vector<uint8_t> foreign = bytes;
		foreign[4] = '0'; // the magic dword's low byte: 'LWF1' -> 'LWF0' (kMagic's convention)
		opennova::lwf::File parsed;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(foreign.data(), foreign.size(), parsed, error));
		TEST_EXPECT(parsed.header.magic == 0x4C574630u);
		TEST_EXPECT(parsed.singles.size() == 3 && parsed.multis.size() == 3 && parsed.playlists.size() == 3 &&
		            parsed.sndparms.size() == 3);
		for (const auto &single : parsed.singles) TEST_EXPECT(single.path.empty());
		TEST_EXPECT(parsed.singles.size() == 3 && parsed.singles[2].name == "TONE");
		// Written back: the records as they were, the string pool (which no reader of such a bank
		// reads) a zero slot per single.
		std::vector<uint8_t> again;
		TEST_EXPECT(
				opennova::lwf::encode_lwf(parsed, again, error) && again.size() == foreign.size());
		uint32_t pool = 0;
		std::memcpy(&pool, foreign.data() + 20, sizeof(pool));
		TEST_EXPECT(pool <= again.size() &&
				std::equal(again.begin(), again.begin() + pool, foreign.begin()));
		for (size_t i = pool; i < again.size(); ++i)
			TEST_EXPECT(again[i] == 0);
	}

	// What the authoring tools leave stale is written zero (ADR 0003): a single's name bytes past
	// its terminator, a set's name bytes past its own, a set's and a layer's offset slots past
	// their count, a file slot's bytes past its path; read back, the bank is the same. In a bank of
	// another magic the dword at +28 of a single's entry is the one the game's lookup tests [orig:
	// SoundBank_FindEntryByName @ 0x75bba0, @ 0x75bbdf], kept; in an 'LWF1' bank, zero.
	{
		std::vector<uint8_t> stale = bytes;
		const auto put = [&stale](size_t at, const char *text) {
			for (size_t i = 0; text[i] != '\0' && at + i < stale.size(); ++i)
				stale[at + i] = static_cast<uint8_t>(text[i]);
		};
		uint32_t multi_header = 0, multi_table = 0, pool = 0;
		std::memcpy(&multi_header, stale.data() + 16, 4);
		std::memcpy(&pool, stale.data() + 20, 4);
		std::memcpy(&multi_table, stale.data() + multi_header + 8, 4);
		put(28 + 20, "OLDNAME"); // the first single's name (MSOVR_2) past its NUL
		put(28 + 28, "WXYZ"); // its dword +28
		put(multi_table + 16, "ZZ"); // the first set's name past its NUL
		const uint32_t junk = 0xDEADBEEFu;
		std::memcpy(stale.data() + multi_table + 40 + 7 * 4, &junk, 4); // its last playlist slot
		const uint32_t layers = multi_table + 3 * 80;
		std::memcpy(stale.data() + layers + 12, &junk, 4); // the first layer's scratch dword
		std::memcpy(stale.data() + layers + 16 + 7 * 4, &junk, 4); // its last member slot
		put(pool + 200, "stale path"); // the first file slot past its path
		opennova::lwf::File read;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(stale.data(), stale.size(), read, error));
		TEST_EXPECT(read.singles.size() == 3 && read.singles[0].entry_word28 == 0);
		std::vector<uint8_t> written;
		TEST_EXPECT(opennova::lwf::encode_lwf(read, written, error) && written == bytes);
		// The same in a bank of another magic: the +28 dword is kept.
		stale[4] = '0';
		opennova::lwf::File foreign;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(stale.data(), stale.size(), foreign, error));
		uint32_t word = 0;
		std::memcpy(&word, "WXYZ", 4);
		TEST_EXPECT(foreign.singles.size() == 3 && foreign.singles[0].entry_word28 == word);
		TEST_EXPECT(opennova::lwf::encode_lwf(foreign, written, error) && written.size() > 28 + 32);
		uint32_t written_word = 0;
		if (written.size() > 28 + 32)
			std::memcpy(&written_word, written.data() + 28 + 28, 4);
		TEST_EXPECT(written_word == word && written[28 + 20] == 0);
		opennova::lwf::File again;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(written.data(), written.size(), again, error) &&
				again.singles.size() == 3 && again.singles[0].entry_word28 == word);
	}

	// The game's lookup in a bank of another magic tests the whole dword at +28: a name of 29
	// characters or more always finds (its 29th byte is a character's), one of 28 by the three
	// bytes after its terminator. A name as wide as its field (32 characters, no terminator), a
	// set's of 24 and a path of 256 come back as they went.
	{
		opennova::lwf::File wide = file;
		wide.header.magic = 0x4C574630u;
		wide.singles[0].name = std::string(28, 'N');
		wide.singles[0].entry_word28 = 0x01020300u;
		wide.singles[1].name = std::string(29, 'M');
		wide.singles[1].entry_word28 = 0x55u; // no room: the name's own bytes
		std::vector<uint8_t> written;
		TEST_EXPECT(
				opennova::lwf::encode_lwf(wide, written, error) && written.size() > 28 + 2 * 52);
		uint32_t at28 = 0, at28_next = 0;
		if (written.size() > 28 + 2 * 52) {
			std::memcpy(&at28, written.data() + 28 + 28, 4);
			std::memcpy(&at28_next, written.data() + 28 + 52 + 28, 4);
		}
		TEST_EXPECT(at28 == 0x01020300u && (at28_next & 0xFFu) == 'M');
		opennova::lwf::File back;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(written.data(), written.size(), back, error) &&
				back.singles.size() == 3 && back.singles[0].name == std::string(28, 'N') &&
				back.singles[0].entry_word28 == 0x01020300u && back.singles[1].entry_word28 == 0);

		opennova::lwf::File full = file;
		full.singles[0].name = std::string(32, 'F');
		full.multis[0].name = std::string(24, 'S');
		full.singles[0].path = std::string(256, 'p');
		TEST_EXPECT(opennova::lwf::encode_lwf(full, written, error));
		opennova::lwf::File full_back;
		std::vector<uint8_t> twice;
		TEST_EXPECT(
				opennova::lwf::parse_lwf_buffer(written.data(), written.size(), full_back, error) &&
				full_back.singles[0].name == std::string(32, 'F') &&
				full_back.multis[0].name == std::string(24, 'S') &&
				full_back.singles[0].path == std::string(256, 'p'));
		TEST_EXPECT(opennova::lwf::encode_lwf(full_back, twice, error) && twice == written);
	}

	// A nonzero trigger count: the engine reads 12 * count bytes after the singles and nothing
	// reads them again [orig: SoundBank_OpenFile @ 0x75cb63 / 0x75cb7a]; the multi header is
	// reached by its offset. Two records spliced into menu.lwf after its three singles, every
	// offset past them moved by their 24 bytes.
	{
		constexpr uint32_t kInserted = 24;
		const uint32_t at = 28 + 3 * 52;
		std::vector<uint8_t> triggered = bytes;
		const uint8_t records[kInserted] = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0,
		                                    0xAA, 0, 0, 0, 0xBB, 0, 0, 0, 0xCC, 0, 0, 0};
		triggered.insert(triggered.begin() + at, std::begin(records), std::end(records));
		const auto u32 = [&](size_t offset) {
			uint32_t v = 0;
			std::memcpy(&v, triggered.data() + offset, 4);
			return v;
		};
		const auto shift = [&](size_t offset) {
			const uint32_t v = u32(offset);
			if (v >= at) {
				const uint32_t moved = v + kInserted;
				std::memcpy(triggered.data() + offset, &moved, 4);
			}
		};
		const uint32_t count = 2;
		std::memcpy(triggered.data() + 12, &count, 4);
		shift(16); // multi header
		shift(20); // string pool
		for (uint32_t i = 0; i < 3; ++i) shift(28 + i * 52 + 48); // single path offsets
		const uint32_t multi_header = u32(16);
		shift(multi_header + 8); // multi table
		const uint32_t multi_table = u32(multi_header + 8);
		for (uint32_t i = 0; i < 3; ++i)
			for (uint32_t slot = 0; slot < 8; ++slot) shift(multi_table + i * 80 + 40 + slot * 4);
		const uint32_t playlists = multi_table + 3 * 80;
		for (uint32_t i = 0; i < 3; ++i)
			for (uint32_t slot = 0; slot < 8; ++slot) shift(playlists + i * 48 + 16 + slot * 4);

		opennova::lwf::File parsed;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(triggered.data(), triggered.size(), parsed, error));
		TEST_EXPECT(parsed.header.trigger_count == 2 && parsed.trigger_count == 2);
		TEST_EXPECT(parsed.multis.size() == 3 && parsed.singles.size() == 3 && parsed.playlists.size() == 3);
		TEST_EXPECT(parsed.singles.size() == 3 && parsed.singles[2].path == "tone.wav");
		// Written back: the count kept, the records zero (no reader interprets one).
		std::vector<uint8_t> again;
		std::vector<uint8_t> zeroed = triggered;
		std::fill(zeroed.begin() + at, zeroed.begin() + at + kInserted, uint8_t{ 0 });
		TEST_EXPECT(opennova::lwf::encode_lwf(parsed, again, error) && again == zeroed);
	}

	// Both minted through the writer: a bank of another magic keeps it, and trigger records
	// land after the singles, the count theirs and the multi header after them.
	{
		opennova::lwf::File minted = file;
		minted.header.magic = 0x4C574630u; // 'LWF0', in kMagic's convention
		minted.trigger_count = 1;
		std::vector<uint8_t> written;
		TEST_EXPECT(opennova::lwf::encode_lwf(minted, written, error));
		uint32_t magic = 0, count = 0, multi_header = 0;
		TEST_EXPECT(written.size() > 28);
		if (written.size() > 28) {
			std::memcpy(&magic, written.data() + 4, 4);
			std::memcpy(&count, written.data() + 12, 4);
			std::memcpy(&multi_header, written.data() + 16, 4);
		}
		TEST_EXPECT(magic == 0x4C574630u && count == 1 && multi_header == 28 + 3 * 52 + 12);
		opennova::lwf::File back;
		TEST_EXPECT(opennova::lwf::parse_lwf_buffer(written.data(), written.size(), back, error));
		TEST_EXPECT(back.header.magic == 0x4C574630u && back.trigger_count == 1);
		TEST_EXPECT(back.multis.size() == 3 && back.sndparms.size() == 3);
		for (const auto &single : back.singles) TEST_EXPECT(single.path.empty());
		// A File built from scratch writes the 'LWF1' every shipped bank carries.
		opennova::lwf::File blank;
		std::vector<uint8_t> blank_bytes;
		TEST_EXPECT(opennova::lwf::encode_lwf(blank, blank_bytes, error) && blank_bytes.size() >= 8);
		if (blank_bytes.size() >= 8) {
			std::memcpy(&magic, blank_bytes.data() + 4, 4);
			TEST_EXPECT(magic == opennova::lwf::kMagic);
		}
	}

	return 0;
}
