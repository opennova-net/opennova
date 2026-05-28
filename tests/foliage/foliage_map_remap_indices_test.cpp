#include <foliage/foliage.h>

#include <array>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

std::array<uint8_t, 256> identity_lut() {
	std::array<uint8_t, 256> lut;
	for (int i = 0; i < 256; ++i) {
		lut[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
	}
	return lut;
}

} // namespace

int main() {
	// indices laid out as [1, 2, 3, 1]
	opennova::FoliageMap map = opennova::foliage_make_default_map(4, 1, 0);
	opennova::foliage_set_index(map, 0, 0, 1);
	opennova::foliage_set_index(map, 1, 0, 2);
	opennova::foliage_set_index(map, 2, 0, 3);
	opennova::foliage_set_index(map, 3, 0, 1);

	// Remap 1->2 and 2->3 in a single pass. The key correctness property: a cell
	// that started as 1 must end as 2 (not chained 1->2->3).
	std::array<uint8_t, 256> lut = identity_lut();
	lut[1] = 2;
	lut[2] = 3;

	const int changed = opennova::foliage_remap_indices(map, lut);
	if (!expect(changed == 3, "remap_indices should report one change per rewritten cell (cell 3->3 is a no-op)"))
		return 1;

	if (!expect(opennova::foliage_get_index(map, 0, 0) == 2, "original index 1 must map to 2, not chain to 3")) return 1;
	if (!expect(opennova::foliage_get_index(map, 1, 0) == 3, "original index 2 must map to 3")) return 1;
	if (!expect(opennova::foliage_get_index(map, 2, 0) == 3, "original index 3 stays 3 under identity entry")) return 1;
	if (!expect(opennova::foliage_get_index(map, 3, 0) == 2, "second original index 1 must also map to 2")) return 1;

	if (!expect(opennova::foliage_count_index(map, 1) == 0, "no original index 1 should remain")) return 1;
	if (!expect(opennova::foliage_count_index(map, 2) == 2, "both original 1 cells should now be 2")) return 1;
	if (!expect(opennova::foliage_count_index(map, 3) == 2, "original 2 and original 3 should both be 3")) return 1;

	// An identity LUT changes nothing.
	if (!expect(opennova::foliage_remap_indices(map, identity_lut()) == 0, "identity remap should change no cells")) return 1;

	// An unsized map is a no-op.
	opennova::FoliageMap empty;
	if (!expect(opennova::foliage_remap_indices(empty, lut) == 0, "remap on an unsized map should be a no-op")) return 1;

	std::printf("OK: foliage_remap_indices applies a single-pass LUT without chaining\n");
	return 0;
}
