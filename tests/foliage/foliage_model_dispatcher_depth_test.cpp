// The model-tier depth gate [orig: Terrain_RenderSectorEntitiesBySide
// @ 0x5c7d50]: only anchors at view-space depth >= 38.0 dispatch model
// tiles; below the gate the walk does nothing at all (no cache traffic,
// no sampler calls).
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

} // namespace

int main() {
	ModelDispatcher dispatcher;
	ModelPlacementConfig cfg{};

	int64_t sampler_calls = 0;
	PlacementSamplers s;
	s.path_blocked = [&](Fixed16_16, Fixed16_16, int32_t) {
		++sampler_calls;
		return false;
	};
	s.slot_mask_at = [&](Fixed16_16, Fixed16_16) -> uint32_t {
		++sampler_calls;
		return 0xFu;
	};
	s.height_at = [&](Fixed16_16, Fixed16_16) -> Fixed16_16 {
		++sampler_calls;
		return 0;
	};

	// Lattice-point anchor: all 4 quadrant tiles emit (see the quadrants test).
	const Fixed16_16 ax = 0x00200000;
	const Fixed16_16 az = 0x00300000;

	// Below the gate: nothing happens.
	std::vector<ModelTileDraw> out;
	dispatcher.walk(0, ax, az, 37.9f, 1, cfg, s, out);
	if (!expect(out.empty(), "depth < 38 emits nothing")) return 1;
	if (!expect(dispatcher.cache_occupancy() == 0, "depth < 38 caches nothing")) return 1;
	if (!expect(sampler_calls == 0, "depth < 38 never samples")) return 1;
	if (!expect(dispatcher.cache_hits() == 0 && dispatcher.cache_misses() == 0,
	            "depth < 38 records no cache traffic")) return 1;

	// Exactly at the gate: the walk proceeds (>= 38.0).
	dispatcher.walk(0, ax, az, MODEL_DEPTH_GATE, 1, cfg, s, out);
	if (!expect(out.size() == 4, "depth == 38 dispatches the 4 quadrant tiles")) return 1;
	if (!expect(dispatcher.cache_occupancy() == 4, "depth == 38 fills the cache")) return 1;
	if (!expect(sampler_calls > 0, "depth == 38 generates")) return 1;

	// Invalid slots are rejected regardless of depth.
	out.clear();
	dispatcher.walk(-1, ax, az, 100.0f, 1, cfg, s, out);
	dispatcher.walk(FOLIAGE_MAX_DEFS, ax, az, 100.0f, 1, cfg, s, out);
	if (!expect(out.empty(), "invalid slots never dispatch")) return 1;

	std::printf("OK: the model walk is gated on view depth >= 38.0\n");
	return 0;
}
