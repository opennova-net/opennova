#include <foliage/model_dispatcher.h>

#include <cstdio>
#include <vector>

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

PlacementSamplers permissive() {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };
	return s;
}

} // namespace

int main() {
	ModelDispatcher dispatcher;
	ModelPlacementConfig cfg{};
	auto samplers = permissive();

	// An anchor exactly on a 16u lattice point: its +-4u candidate box reaches
	// the margin rows/columns of all 4 quadrant tiles, so every tile emits.
	const Fixed16_16 ax = 0x00200000;
	const Fixed16_16 az = 0x00300000;

	std::vector<ModelTileDraw> out;
	dispatcher.walk(0, ax, az, MODEL_DEPTH_GATE, 1, cfg, samplers, out);

	if (!expect(out.size() == 4, "the walk emits the 4 quadrant tiles")) {
		std::fprintf(stderr, "  emitted %zu\n", out.size());
		return 1;
	}
	if (!expect(dispatcher.cache_occupancy() == 4, "4 cache entries after the first walk")) return 1;
	if (!expect(dispatcher.cache_misses() == 4 && dispatcher.cache_hits() == 0,
	            "first walk is 4 misses")) return 1;

	// The emitted keys match model_quadrant_tiles exactly, all distinct.
	const auto tiles = model_quadrant_tiles(ax, az);
	for (int i = 0; i < 4; ++i) {
		if (!expect(out[i].tile_key == tiles[i].key, "emitted keys follow the quadrant walk order")) return 1;
		if (!expect(out[i].snap_x_fixed == tiles[i].snap_x_fixed &&
		                out[i].snap_z_fixed == tiles[i].snap_z_fixed,
		            "emitted snap origins match the quadrant tiles")) return 1;
		for (int j = i + 1; j < 4; ++j) {
			if (!expect(out[i].tile_key != out[j].tile_key, "quadrant keys are distinct")) return 1;
		}
		if (!expect(out[i].result.count > 0 && out[i].result.count <= MODEL_TILE_CAP,
		            "each tile carries a bounded instance list")) return 1;
		// Every instance stays inside its own tile square and the +-4u anchor box.
		for (int k = 0; k < out[i].result.count; ++k) {
			const auto &inst = out[i].result.instances[k];
			if (!expect(inst.center_x_fixed >= out[i].snap_x_fixed &&
			                inst.center_x_fixed <= out[i].snap_x_fixed + MODEL_TILE_SIZE &&
			                inst.center_z_fixed >= out[i].snap_z_fixed &&
			                inst.center_z_fixed <= out[i].snap_z_fixed + MODEL_TILE_SIZE,
			            "instances stay inside their quadrant tile")) return 1;
		}
	}

	// A second walk on the same anchor is 4 hits, no growth, same content.
	std::vector<ModelTileDraw> out2;
	dispatcher.walk(0, ax, az, MODEL_DEPTH_GATE, 2, cfg, samplers, out2);
	if (!expect(dispatcher.cache_occupancy() == 4, "re-walk does not grow the cache")) return 1;
	if (!expect(dispatcher.cache_hits() == 4, "re-walk is 4 hits")) return 1;
	if (!expect(out2.size() == 4, "re-walk emits the cached 4 tiles")) return 1;

	// A mid-tile anchor still WALKS 4 tiles (4 more cache entries) but only
	// the tiles whose candidate span overlaps the +-4u box draw: the walk /
	// draw-only-when-count>0 split is witnessed behavior
	// [orig: Foliage_UpdateModelTiles @ 0x601f50].
	ModelDispatcher mid;
	std::vector<ModelTileDraw> out3;
	mid.walk(0, 0x00185000, 0x00294000, MODEL_DEPTH_GATE, 1, cfg, samplers, out3);
	if (!expect(mid.cache_occupancy() == 4, "a mid-tile anchor still touches 4 cache entries")) return 1;
	if (!expect(out3.size() == 1, "only the overlapped tile draws for the mid-tile anchor")) {
		std::fprintf(stderr, "  emitted %zu\n", out3.size());
		return 1;
	}

	std::printf("OK: model walk covers 4 distinct quadrant tiles from the cache\n");
	return 0;
}
