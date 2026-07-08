// Evict-oldest at the 1000-entry capacity [orig: Foliage_UpdateModelTiles
// @ 0x601f50 - on miss, the max-age entry (smallest last-touch frame stamp)
// is evicted and regenerated].
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

	// Anchors spaced 32u apart never share tiles: each walk adds exactly 4
	// distinct new keys. 250 walks fill the cache to exactly 1000.
	std::vector<ModelTileDraw> out;
	int32_t frame = 1;
	for (int i = 0; i < 250; ++i) {
		const Fixed16_16 ax = 0x00085000 + i * 0x200000;
		const Fixed16_16 az = 0x00094000;
		out.clear();
		dispatcher.walk(0, ax, az, MODEL_DEPTH_GATE, frame++, cfg, samplers, out);
	}
	if (!expect(dispatcher.cache_occupancy() == MODEL_CACHE_ENTRIES,
	            "250 disjoint walks fill the cache to exactly 1000")) {
		std::fprintf(stderr, "  occupancy %d\n", dispatcher.cache_occupancy());
		return 1;
	}
	if (!expect(dispatcher.cache_misses() == MODEL_CACHE_ENTRIES, "1000 misses to fill")) return 1;

	// One more disjoint walk: occupancy stays at capacity - 4 evictions.
	out.clear();
	dispatcher.walk(0, 0x00085000 + 250 * 0x200000, 0x00094000, MODEL_DEPTH_GATE,
	                frame++, cfg, samplers, out);
	if (!expect(dispatcher.cache_occupancy() == MODEL_CACHE_ENTRIES,
	            "the cache never exceeds 1000 entries")) return 1;
	if (!expect(dispatcher.cache_misses() == MODEL_CACHE_ENTRIES + 4,
	            "the overflow walk is 4 more misses")) return 1;

	// The evicted entries are the OLDEST (the first walk's tiles): re-walking
	// anchor 0 misses all 4 again...
	const int64_t misses_before = dispatcher.cache_misses();
	out.clear();
	dispatcher.walk(0, 0x00085000, 0x00094000, MODEL_DEPTH_GATE, frame++, cfg, samplers, out);
	if (!expect(dispatcher.cache_misses() == misses_before + 4,
	            "the oldest (first-walk) tiles were the ones evicted")) {
		std::fprintf(stderr, "  misses %lld -> %lld\n",
		             static_cast<long long>(misses_before),
		             static_cast<long long>(dispatcher.cache_misses()));
		return 1;
	}
	// ...while a recently-touched anchor still hits.
	const int64_t hits_before = dispatcher.cache_hits();
	out.clear();
	dispatcher.walk(0, 0x00085000 + 249 * 0x200000, 0x00094000, MODEL_DEPTH_GATE,
	                frame++, cfg, samplers, out);
	if (!expect(dispatcher.cache_hits() == hits_before + 4,
	            "recently-touched tiles survive the eviction")) return 1;

	std::printf("OK: model cache evicts oldest-by-frame-stamp at the 1000 capacity\n");
	return 0;
}
