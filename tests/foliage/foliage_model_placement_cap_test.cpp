#include <foliage/model_placement.h>

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
	ModelPlacementConfig cfg{};
	PlacementSamplers samplers;
	samplers.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	samplers.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	samplers.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

	const uint32_t key = pack_model_tile_key(0x00100000, 0x00200000);

	// With a huge radius every one of the 36 candidates passes the gates; the
	// generator must stop at the 21-instance cap (the retail draw buffers are
	// sized for exactly 21 [orig: Foliage_InitModelTileBuffers @ 0x5ffcd0]).
	const auto capped = generate_model_tile_instances(0, key, 0x00180000, 0x00280000,
	                                                  0x40000000, cfg, samplers);
	if (!expect(capped.count == MODEL_TILE_CAP, "permissive tile must cap at 21 instances")) {
		std::fprintf(stderr, "  count = %d\n", capped.count);
		return 1;
	}

	// At the retail radius (0x40000 around the tile-center anchor) the 6x6
	// grid can never reach the cap: the +-4u window spans at most 4 grid
	// columns/rows. Pin the derived count for the reference tile (9, from the
	// float32-emulated hand computation).
	const auto retail = generate_model_tile_instances(0, key, 0x00180000, 0x00280000,
	                                                  MODEL_CANDIDATE_RADIUS, cfg, samplers);
	if (!expect(retail.count == 9, "retail-radius tile-center anchor accepts the hand-computed 9")) {
		std::fprintf(stderr, "  count = %d\n", retail.count);
		return 1;
	}
	if (!expect(retail.count <= MODEL_TILE_CAP, "count never exceeds the cap")) return 1;

	std::printf("OK: model tile placement caps at %d instances\n", MODEL_TILE_CAP);
	return 0;
}
