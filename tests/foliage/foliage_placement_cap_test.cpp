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
	bool captured_mask_point = false;
	Fixed16_16 mask_x_fixed = 0;
	Fixed16_16 mask_engine_z_fixed = 0;
	s.slot_mask_at = [&](Fixed16_16 wx, Fixed16_16 engine_z) -> uint32_t {
		if (!captured_mask_point) {
			captured_mask_point = true;
			mask_x_fixed = wx;
			mask_engine_z_fixed = engine_z;
		}
		return 0xFu;
	};
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

	PlacementConfig cfg{};
	cfg.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;

	const uint32_t key = pack_cell_key(0x00100000, 0x00100000);

	const auto result = place_cell(0, key, cfg, s);

	// [orig: generate_foliage_instances_0 @ 0x5ffdd0]
	if (!expect(result.count == FOLIAGE_CANDIDATES_PER_CELL,
	            "permissive far placement must accept all 36 candidates")) {
		std::fprintf(stderr, "  actual count: %d\n", result.count);
		return 1;
	}
	if (!expect(captured_mask_point, "FAR placement must query the slot mask")) return 1;
	if (!expect(mask_x_fixed == result.instances[0].world_x_fixed,
	            "slot-mask X receives the candidate world X")) return 1;
	if (!expect(mask_engine_z_fixed == result.instances[0].world_z_fixed,
	            "slot-mask Z receives the candidate's own world Z (no boundary "
	            "negation; placement.h)")) return 1;

	// Zero slot mask should emit nothing (FORCE_ON bypasses path check but NOT slot mask).
	// The retail generator has no view cull [orig: generate_foliage_instances_0
	// @ 0x5ffdd0 - args are (key, VB, IB, out counts)].
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0u; };
	const auto masked = place_cell(0, key, cfg, s);
	if (!expect(masked.count == 0, "slot mask 0 must emit nothing")) return 1;

	// [orig: generate_foliage_instances_0 @ 0x5ffdd0] The sign bit is the
	// caller-visible empty-cell marker, not part of the packed coordinates.
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	const auto empty = place_cell(0, 0x80000000u, cfg, s);
	if (!expect(empty.count == 0, "the 0x80000000 FAR key must stay empty")) return 1;

	std::printf("OK: FAR placement accepts all 36 candidates and honors mask/empty-key gates\n");
	return 0;
}
