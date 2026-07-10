// Cache-hit behavior of the model tile walk: off-stagger hits serve the
// cached tile without touching the samplers; the 8-frame stagger
// ((frame + 2*slot) & 7) == 0 regenerates [orig: Foliage_UpdateModelTiles
// @ 0x601f50].
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
	// The stagger table mirrors the FAR tier's: slot 0 fires on frame % 8 == 0,
	// slot 1 on 6, slot 2 on 4, slot 3 on 2 - exactly once per 8-frame window.
	for (int32_t frame = 0; frame < 64; ++frame) {
		if (!expect(ModelDispatcher::is_staggered_regen_frame(frame, 0) == ((frame & 7) == 0),
		            "slot 0 stagger gate")) return 1;
		if (!expect(ModelDispatcher::is_staggered_regen_frame(frame, 1) == ((frame & 7) == 6),
		            "slot 1 stagger gate")) return 1;
		if (!expect(ModelDispatcher::is_staggered_regen_frame(frame, 2) == ((frame & 7) == 4),
		            "slot 2 stagger gate")) return 1;
		if (!expect(ModelDispatcher::is_staggered_regen_frame(frame, 3) == ((frame & 7) == 2),
		            "slot 3 stagger gate")) return 1;
	}
	for (int slot = 0; slot < FOLIAGE_MAX_DEFS; ++slot) {
		int fires = 0;
		for (int32_t frame = 0; frame < 8; ++frame) {
			if (ModelDispatcher::is_staggered_regen_frame(frame, slot)) {
				++fires;
			}
		}
		if (!expect(fires == 1, "each slot regenerates exactly once per 8-frame window")) return 1;
	}

	ModelDispatcher dispatcher;
	ModelPlacementConfig cfg{};

	int64_t height_calls = 0;
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [&height_calls](Fixed16_16, Fixed16_16) -> Fixed16_16 {
		++height_calls;
		return 0;
	};

	// Lattice-point anchor: all 4 quadrant tiles emit (see the quadrants test).
	const Fixed16_16 ax = 0x00200000;
	const Fixed16_16 az = 0x00300000;
	const int slot = 0;

	// Prime the cache on an off-stagger frame (frame 1 for slot 0).
	std::vector<ModelTileDraw> out;
	dispatcher.walk(slot, ax, az, MODEL_DEPTH_GATE, 1, cfg, s, out);
	if (!expect(height_calls > 0, "the priming walk generates (samples heights)")) return 1;
	if (!expect(out.size() == 4, "priming walk emits 4 tiles")) return 1;
	const auto primed = out;

	// Off-stagger hits (frames 2..7): cached content, NO sampler traffic.
	for (int32_t frame = 2; frame <= 7; ++frame) {
		height_calls = 0;
		std::vector<ModelTileDraw> hit_out;
		dispatcher.walk(slot, ax, az, MODEL_DEPTH_GATE, frame, cfg, s, hit_out);
		if (!expect(height_calls == 0, "off-stagger cache hits never re-sample")) {
			std::fprintf(stderr, "  frame %d sampled %lld times\n", frame, static_cast<long long>(height_calls));
			return 1;
		}
		if (!expect(hit_out.size() == primed.size(), "off-stagger walks emit the cached tiles")) return 1;
		for (size_t t = 0; t < hit_out.size(); ++t) {
			if (!expect(hit_out[t].result.count == primed[t].result.count,
			            "cached tile content is stable across off-stagger frames")) return 1;
			for (int i = 0; i < hit_out[t].result.count; ++i) {
				if (!expect(hit_out[t].result.instances[i].center_x_fixed ==
				                    primed[t].result.instances[i].center_x_fixed &&
				                hit_out[t].result.instances[i].center_z_fixed ==
				                    primed[t].result.instances[i].center_z_fixed,
				            "cached instances are bit-stable across off-stagger frames")) return 1;
			}
		}
	}
	if (!expect(dispatcher.regenerations() == 0, "no regenerations off-stagger")) return 1;

	// The stagger frame (8 for slot 0) regenerates all 4 cached tiles.
	height_calls = 0;
	std::vector<ModelTileDraw> regen_out;
	dispatcher.walk(slot, ax, az, MODEL_DEPTH_GATE, 8, cfg, s, regen_out);
	if (!expect(height_calls > 0, "the stagger frame re-samples")) return 1;
	if (!expect(dispatcher.regenerations() == 4, "the stagger frame regenerates the 4 hit tiles")) return 1;
	if (!expect(dispatcher.cache_misses() == 4, "regeneration reuses the entries (no new misses)")) return 1;

	std::printf("OK: model cache hits are silent off-stagger and regenerate on the 8-frame stagger\n");
	return 0;
}
