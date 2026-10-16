// The .til reader and writer: every byte of a record is a field (the +9 flags byte whole, the +10 MED
// layer), the header is computed on save as MED's serializer writes it (til0, count, 12 * count + 16,
// 0) and never replayed from the file read, and what the game cannot place is refused.
// [orig: JOTACmed.exe MapTileStore_Serialize @ 0x433EC0]
// [orig: Terrain_LoadTileInfoFile @ 0x60A740]

#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

uint32_t u32_at(const std::vector<uint8_t> &bytes, size_t at) {
	return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
	       (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

bool same_entry(const opennova::TilOverlayEntry &a, const opennova::TilOverlayEntry &b) {
	return a.x_fixed == b.x_fixed && a.z_fixed == b.z_fixed && a.tile_index == b.tile_index &&
	       a.flags == b.flags && a.layer == b.layer;
}

} // namespace

int main() {
	// A minted model: a whole-unit corner, a fractional one (no snap), every flag bit the byte can
	// hold (0x08, Nile's selection mark, and 0x80, which no tool writes) and MED layers.
	opennova::TilFile source;
	source.entries = {
		opennova::make_til_entry(16 * 65536, -32 * 65536, 7,
		                         static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90 |
		                                              opennova::TIL_FLAG_OUTLINE | 0x80u),
		                         0),
		opennova::make_til_entry(48 * 65536 + 0x8000, -8 * 65536 - 3, 19, opennova::TIL_FLAG_FLIP_Y, 22),
		opennova::make_til_entry(-5 * 65536, 7 * 65536, 63, 0xFF, 0x1234),
	};

	std::vector<uint8_t> encoded;
	std::string error;
	if (!opennova::save_til(source, encoded, error)) {
		std::fprintf(stderr, "FAIL: save_til failed: %s\n", error.c_str());
		return 1;
	}

	const size_t expected_size = 16 + source.entries.size() * 12;
	if (!expect(encoded.size() == expected_size, "encoded payload should be the header plus 12 bytes a record")) return 1;
	if (!expect(encoded[0] == '0' && encoded[1] == 'l' && encoded[2] == 'i' && encoded[3] == 't',
	            "til magic should be little-endian '0lit'")) return 1;
	if (!expect(u32_at(encoded, 4) == 3, "the header's count should be the record count")) return 1;
	if (!expect(u32_at(encoded, 8) == expected_size, "the header's +8 should be MED's 12 * count + 16")) return 1;
	if (!expect(u32_at(encoded, 12) == 0, "the header's +12 should be MED's 0")) return 1;
	if (!expect(encoded[16 + 9] == static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90 |
	                                                     opennova::TIL_FLAG_OUTLINE | 0x80u),
	            "the flags byte should be written whole")) return 1;
	if (!expect(encoded[28 + 10] == 22 && encoded[28 + 11] == 0, "the layer should be the record's +10 word")) return 1;
	if (!expect(encoded[40 + 10] == 0x34 && encoded[40 + 11] == 0x12, "a layer past MED's 32 should be carried")) return 1;

	opennova::TilFile loaded;
	opennova::TilHeaderFacts facts;
	error.clear();
	if (!opennova::load_til(encoded.data(), encoded.size(), loaded, error, &facts)) {
		std::fprintf(stderr, "FAIL: load_til failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(loaded.entries.size() == source.entries.size(), "entry count should round-trip")) return 1;
	for (size_t i = 0; i < source.entries.size(); ++i) {
		if (!expect(same_entry(loaded.entries[i], source.entries[i]), "every record field should round-trip")) return 1;
	}
	if (!expect(facts.size_word == expected_size && facts.word12 == 0 && facts.trailing == 0,
	            "a MED header should read back as MED's facts")) return 1;

	// A header not MED's and bytes past the records: read for source notes, never written back.
	std::vector<uint8_t> foreign = encoded;
	foreign[8] = 0x44;
	foreign[12] = 0x88;
	foreign.push_back(0xAB);
	foreign.push_back(0xCD);
	opennova::TilFile foreign_loaded;
	error.clear();
	if (!opennova::load_til(foreign.data(), foreign.size(), foreign_loaded, error, &facts)) {
		std::fprintf(stderr, "FAIL: load_til of a foreign header failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(facts.size_word != expected_size && facts.word12 == 0x88 && facts.trailing == 2,
	            "a foreign header's words and trailing bytes should be reported")) return 1;
	std::vector<uint8_t> rewritten;
	if (!opennova::save_til(foreign_loaded, rewritten, error) || rewritten != encoded) {
		std::fprintf(stderr, "FAIL: a foreign header should save as MED's header, its trailing bytes dropped\n");
		return 1;
	}

	// What the game cannot place.
	opennova::TilFile refused;
	if (!expect(!opennova::load_til(encoded.data(), 15, refused, error), "under 16 bytes should be refused")) return 1;
	std::vector<uint8_t> bad_magic = encoded;
	bad_magic[0] = 'X';
	if (!expect(!opennova::load_til(bad_magic.data(), bad_magic.size(), refused, error), "no til0 mark should be refused")) return 1;
	if (!expect(!opennova::til_load_accepts(bad_magic.data(), bad_magic.size()), "the game's load should refuse no til0 mark")) return 1;
	std::vector<uint8_t> overcount = encoded;
	overcount[4] = 4;
	if (!expect(!opennova::load_til(overcount.data(), overcount.size(), refused, error),
	            "a count past the file's bytes should be refused")) return 1;
	if (!expect(opennova::til_load_accepts(overcount.data(), overcount.size()),
	            "the game's load takes any til0 file of 16 bytes or more, whatever its count")) return 1;

	// The empty file: the header alone.
	std::vector<uint8_t> empty;
	if (!opennova::save_til(opennova::TilFile{}, empty, error) || empty.size() != 16 || u32_at(empty, 4) != 0 ||
	    u32_at(empty, 8) != 16 || u32_at(empty, 12) != 0) {
		std::fprintf(stderr, "FAIL: an empty .til should be MED's 16-byte header with count 0\n");
		return 1;
	}

	// The coordinate helpers.
	if (!expect(opennova::til_cell_from_world(0.0) == 0, "world origin should map to cell 0")) return 1;
	if (!expect(opennova::til_cell_from_world(31.9) == 1, "world units should floor to the containing cell")) return 1;
	if (!expect(opennova::til_cell_from_world(-0.1) == -1, "negative world units should floor toward negative infinity")) return 1;
	if (!expect(opennova::til_world_x_from_fixed(loaded.entries[0].x_fixed) == 16.0f, "x_fixed should decode to world x")) return 1;
	if (!expect(opennova::til_world_z_from_fixed(loaded.entries[0].z_fixed) == 32.0f, "z_fixed should decode to world z")) return 1;
	if (!expect(opennova::til_cell_x_from_fixed(loaded.entries[0].x_fixed) == 1, "x_fixed should decode to its cell_x")) return 1;
	if (!expect(opennova::til_cell_z_from_fixed(loaded.entries[0].z_fixed) == 2, "z_fixed should decode to its cell_z")) return 1;
	if (!expect(opennova::til_cell_x_from_world(-0.1) == -1, "world x should floor toward negative infinity")) return 1;
	if (!expect(opennova::til_cell_z_from_world(16.1) == 1, "world z should floor to the containing cell")) return 1;
	if (!expect(opennova::til_x_fixed_from_world(-3.5) == -229376, "world x should encode to 16.16 fixed")) return 1;
	if (!expect(opennova::til_z_fixed_from_world(2.25) == -147456, "world z should encode to signed 16.16 fixed")) return 1;
	if (!expect(opennova::til_entry_matches_cell(loaded.entries[0], 1, 2), "entry should match its decoded cell")) return 1;
	if (!expect(loaded.entries[1].x_fixed == 48 * 65536 + 0x8000, "make_til_entry should not snap a position")) return 1;

	// make_til_overlay_entry: the cell's corner, layer 0, the flags byte as given.
	const opennova::TilOverlayEntry rebuilt =
	    opennova::make_til_overlay_entry(-3, 5, 9, static_cast<uint8_t>(opennova::TIL_FLAG_OUTLINE | 0x40u));
	if (!expect(opennova::til_cell_x_from_fixed(rebuilt.x_fixed) == -3, "negative cell_x should round-trip through fixed conversion")) return 1;
	if (!expect(opennova::til_cell_z_from_fixed(rebuilt.z_fixed) == 5, "cell_z should round-trip through fixed conversion")) return 1;
	if (!expect(rebuilt.flags == static_cast<uint8_t>(opennova::TIL_FLAG_OUTLINE | 0x40u), "the flags byte should be kept whole")) return 1;
	if (!expect(rebuilt.layer == 0, "a grid record should take layer 0")) return 1;

	std::printf("OK: til IO carries every record byte and writes MED's header from the model\n");
	return 0;
}
