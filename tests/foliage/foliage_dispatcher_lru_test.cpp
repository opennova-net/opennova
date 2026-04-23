#include <foliage/dispatcher.h>

#include <cstdio>

using namespace opennova;
using namespace opennova::foliage;

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
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

	PlacementConfig cfg{};
	cfg.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;

	Dispatcher d;
	std::vector<ZSortInstance> out;

	// Teleport the centre across 40 distinct world positions (160 quadrant cells
	// total — enough to fill the 128-slot LRU and evict). Each dispatch runs
	// on a staggered-bake frame so placements happen.
	const int centre_count = 40;
	for (int i = 0; i < centre_count; ++i) {
		const Fixed16_16 cx = static_cast<Fixed16_16>(i * 0x200000);  // 32-unit strides
		const Fixed16_16 cz = 0;
		// Pick frame counters that all hit slot 0's rebake gate (multiples of 8).
		d.dispatch(0, cx, cz, 100.0f, 0x40000000,
		           /*frame*/ i * 8, cfg, s, out);
	}

	// LRU must be fully occupied (reached capacity).
	if (!expect(d.lru_occupancy() == DISPATCHER_LRU_SLOTS,
	            "LRU must saturate after many teleports")) {
		std::fprintf(stderr, "  actual occupancy: %d / %d\n", d.lru_occupancy(), DISPATCHER_LRU_SLOTS);
		return 1;
	}

	// None of the first-centre's cell keys should remain (they were oldest and got evicted).
	// Recompute what the first centre's 4 keys were.
	const Fixed16_16 first_cx = 0, first_cz = 0;
	uint32_t first_keys[DISPATCHER_QUADRANTS];
	{
		for (int q = 0; q < DISPATCHER_QUADRANTS; ++q) {
			const Fixed16_16 x_off = (q & 1) ? -DISPATCHER_QUADRANT_OFFSET : DISPATCHER_QUADRANT_OFFSET;
			const Fixed16_16 z_off = (q & 2) ? -DISPATCHER_QUADRANT_OFFSET : DISPATCHER_QUADRANT_OFFSET;
			const Fixed16_16 cell_x = (first_cx + x_off) & static_cast<Fixed16_16>(DISPATCHER_CELL_MASK);
			const Fixed16_16 cell_z =
			    ((first_cz + z_off) & static_cast<Fixed16_16>(DISPATCHER_CELL_MASK))
			    + static_cast<Fixed16_16>(DISPATCHER_CELL_SIZE);
			first_keys[q] = pack_cell_key(cell_x, cell_z);
		}
	}

	int surviving = 0;
	for (const auto &entry : d.lru_entries()) {
		for (int q = 0; q < DISPATCHER_QUADRANTS; ++q) {
			if (entry.occupied && entry.cell_key == first_keys[q]) {
				++surviving;
			}
		}
	}
	if (!expect(surviving == 0, "oldest cell keys must have been evicted")) {
		std::fprintf(stderr, "  surviving: %d\n", surviving);
		return 1;
	}

	std::printf("OK: 128-slot LRU evicts oldest entries when full\n");
	return 0;
}
