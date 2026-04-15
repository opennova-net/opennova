#include <til/til.h>
#include <til/til_io.h>

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

} // namespace

int main() {
	opennova::TilFile source;
	source.reserved0 = 0x11223344u;
	source.reserved1 = 0xAABBCCDDu;
	// Entry 0: the full authored mask (FLIP_X | ROTATE_90 | OUTLINE) plus an
	// unauthored bit (0x80) that must be stripped. All four authored bits survive.
	// Entry 1: FLIP_Y + OUTLINE — both survive.
	source.entries = {
		{16 * 65536, -32 * 65536, 7, static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90 | opennova::TIL_FLAG_OUTLINE | 0x80u), 0x1234u},
		{48 * 65536, -8 * 65536, 19, static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_Y | opennova::TIL_FLAG_OUTLINE), 0x4321u},
	};

	std::vector<uint8_t> encoded;
	std::string error;
	if (!opennova::save_til(source, encoded, error)) {
		std::fprintf(stderr, "FAIL: save_til failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(encoded.size() == 16 + source.entries.size() * 12, "encoded payload should match header + entries")) return 1;
	if (!expect(encoded[0] == '0' && encoded[1] == 'l' && encoded[2] == 'i' && encoded[3] == 't',
	            "til magic should be little-endian '0lit'")) return 1;
	if (!expect(encoded[4] == 2 && encoded[5] == 0 && encoded[6] == 0 && encoded[7] == 0,
	            "entry count should be encoded in the header")) return 1;
	if (!expect(encoded[8] == 0 && encoded[9] == 0 && encoded[10] == 0 && encoded[11] == 0,
	            "reserved header word 0 should be zeroed on save")) return 1;
	if (!expect(encoded[12] == 0 && encoded[13] == 0 && encoded[14] == 0 && encoded[15] == 0,
	            "reserved header word 1 should be zeroed on save")) return 1;
	if (!expect(encoded[25] == static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90 | opennova::TIL_FLAG_OUTLINE),
	            "save_til should keep authored bits including OUTLINE and strip 0x80")) return 1;
	if (!expect(encoded[26] == 0 && encoded[27] == 0,
	            "save_til should zero the trailing reserved field")) return 1;

	opennova::TilFile loaded;
	error.clear();
	if (!opennova::load_til(encoded.data(), encoded.size(), loaded, error)) {
		std::fprintf(stderr, "FAIL: load_til failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(loaded.reserved0 == 0, "reserved0 should normalize to zero")) return 1;
	if (!expect(loaded.reserved1 == 0, "reserved1 should normalize to zero")) return 1;
	if (!expect(loaded.entries.size() == source.entries.size(), "entry count should round-trip")) return 1;
	if (!expect(loaded.entries[0].x_fixed == source.entries[0].x_fixed, "x_fixed should round-trip")) return 1;
	if (!expect(loaded.entries[0].z_fixed == source.entries[0].z_fixed, "z_fixed should round-trip")) return 1;
	if (!expect(loaded.entries[0].tile_index == source.entries[0].tile_index, "tile index should round-trip")) return 1;
	if (!expect(loaded.entries[0].flags == static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90 | opennova::TIL_FLAG_OUTLINE), "flags should normalize to authored bits (incl. OUTLINE)")) return 1;
	if (!expect(loaded.entries[0].reserved == 0, "entry reserved field should normalize to zero")) return 1;
	if (!expect(loaded.entries[1].flags == static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_Y | opennova::TIL_FLAG_OUTLINE), "second entry flags should round-trip FLIP_Y|OUTLINE")) return 1;
	if (!expect(loaded.entries[1].reserved == 0, "second entry reserved field should normalize to zero")) return 1;
	if (!expect(opennova::til_cell_from_world(0.0) == 0, "world origin should map to cell 0")) return 1;
	if (!expect(opennova::til_cell_from_world(31.9) == 1, "world units should floor to the containing cell")) return 1;
	if (!expect(opennova::til_cell_from_world(-0.1) == -1, "negative world units should floor toward negative infinity")) return 1;
	if (!expect(opennova::til_world_x_from_fixed(loaded.entries[0].x_fixed) == 16.0f, "x_fixed should decode to world x")) return 1;
	if (!expect(opennova::til_world_z_from_fixed(loaded.entries[0].z_fixed) == 32.0f, "z_fixed should decode to world z")) return 1;
	if (!expect(opennova::til_cell_x_from_fixed(loaded.entries[0].x_fixed) == 1, "x_fixed should decode to snapped cell_x")) return 1;
	if (!expect(opennova::til_cell_z_from_fixed(loaded.entries[0].z_fixed) == 2, "z_fixed should decode to snapped cell_z")) return 1;
	if (!expect(opennova::til_cell_x_from_world(-0.1) == -1, "world x should floor toward negative infinity")) return 1;
	if (!expect(opennova::til_cell_z_from_world(16.1) == 1, "world z should floor to the containing snapped cell")) return 1;
	if (!expect(opennova::til_x_fixed_from_world(-3.5) == -229376, "world x should encode to 16.16 fixed")) return 1;
	if (!expect(opennova::til_z_fixed_from_world(2.25) == -147456, "world z should encode to signed 16.16 fixed")) return 1;
	if (!expect(opennova::til_entry_matches_cell(loaded.entries[0], 1, 2), "entry should match its decoded snapped cell")) return 1;
	// make_til_overlay_entry with OUTLINE + FLIP_Y — both are authored bits, both survive.
	const opennova::TilOverlayEntry rebuilt = opennova::make_til_overlay_entry(-3, 5, 9, static_cast<uint8_t>(opennova::TIL_FLAG_OUTLINE | opennova::TIL_FLAG_FLIP_Y));
	if (!expect(opennova::til_cell_x_from_fixed(rebuilt.x_fixed) == -3, "negative cell_x should round-trip through fixed conversion")) return 1;
	if (!expect(opennova::til_cell_z_from_fixed(rebuilt.z_fixed) == 5, "cell_z should round-trip through fixed conversion")) return 1;
	if (!expect(rebuilt.flags == static_cast<uint8_t>(opennova::TIL_FLAG_FLIP_Y | opennova::TIL_FLAG_OUTLINE), "helper-built entry should keep authored transform + OUTLINE flags")) return 1;
	if (!expect(rebuilt.reserved == 0, "helper-built entry should zero the trailing reserved field")) return 1;

	std::vector<uint8_t> legacy = encoded;
	legacy[8] = 0x44;
	legacy[12] = 0x88;
	legacy[25] = 0xFF;
	legacy[26] = 0x34;
	legacy[27] = 0x12;
	opennova::TilFile normalized_legacy;
	error.clear();
	if (!opennova::load_til(legacy.data(), legacy.size(), normalized_legacy, error)) {
		std::fprintf(stderr, "FAIL: load_til legacy normalization failed: %s\n", error.c_str());
		return 1;
	}
	if (!expect(normalized_legacy.reserved0 == 0 && normalized_legacy.reserved1 == 0, "legacy header reserved fields should be cleared on load")) return 1;
	// legacy[25] = 0xFF: all 8 bits set. After the authored 0x0F mask the low nibble (0x0F) survives.
	if (!expect(normalized_legacy.entries[0].flags == static_cast<uint8_t>(opennova::TIL_FLAG_AUTHORED_MASK),
	            "legacy entry flags should mask down to the authored 0x0F on load")) return 1;
	if (!expect(normalized_legacy.entries[0].reserved == 0, "legacy entry reserved field should clear on load")) return 1;

	std::printf("OK: til IO canonicalized authored flags, cleared unknown data, and preserved world-space placement\n");
	return 0;
}
