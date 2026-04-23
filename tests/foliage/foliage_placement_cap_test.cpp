#include <foliage/placement.h>

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
	// Permissive samplers: every candidate passes acceptance.
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

	PlacementConfig cfg{};
	cfg.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;

	const uint32_t key = pack_cell_key(0x00100000, 0x00100000);
	const int32_t huge_radius = 0x40000000;

	const auto result = place_cell(0, key, 0x00100000, 0x00100000, huge_radius, cfg, s);

	if (!expect(result.count == FOLIAGE_CELL_CAP,
	            "permissive samplers must saturate the 21-instance cap")) {
		std::fprintf(stderr, "  actual count: %d\n", result.count);
		return 1;
	}

	// L∞ cull should reduce count when the radius is tiny.
	const auto clipped = place_cell(0, key, 0x00100000, 0x00100000, 0x10000, cfg, s);
	if (!expect(clipped.count < result.count, "small view radius must clip candidates")) return 1;

	// Zero slot mask should emit nothing (FORCE_ON bypasses path check but NOT slot mask).
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0u; };
	const auto masked = place_cell(0, key, 0x00100000, 0x00100000, huge_radius, cfg, s);
	if (!expect(masked.count == 0, "slot mask 0 must emit nothing")) return 1;

	std::printf("OK: foliage placement honors the 21-cap, L∞ cull, and slot mask\n");
	return 0;
}
