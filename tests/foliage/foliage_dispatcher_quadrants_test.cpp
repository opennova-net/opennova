#include <foliage/dispatcher.h>

#include <cstdio>
#include <set>

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
	// Samplers that produce exactly one instance per cell (so we can count distinct cells).
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	// Permit only slot 0 and then clear mask by position: we need the dispatcher to visit
	// all four cells, so return 0xF unconditionally.
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

	PlacementConfig cfg{};
	cfg.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;

	Dispatcher d;
	std::vector<ZSortInstance> out;

	// Frame 0: dispatch slot 0 around world origin with huge view radius.
	// Engine stagger: ((0 + 2*0) & 7) == 0 → rebake fires on frame 0 for slot 0.
	d.dispatch(/*slot*/ 0,
	           /*centre_x*/ 0,
	           /*centre_z*/ 0,
	           /*camera_z*/ 100.0f,
	           /*radius*/ 0x40000000,
	           /*frame*/ 0,
	           cfg, s, out);

	if (!expect(d.lru_occupancy() == DISPATCHER_QUADRANTS,
	            "dispatcher must occupy exactly 4 LRU slots on first dispatch")) {
		std::fprintf(stderr, "  actual occupancy: %d\n", d.lru_occupancy());
		return 1;
	}

	// Collect the distinct cell keys the dispatcher wrote.
	std::set<uint32_t> keys_seen;
	for (const auto &entry : d.lru_entries()) {
		if (entry.occupied) {
			keys_seen.insert(entry.cell_key);
		}
	}

	if (!expect(keys_seen.size() == DISPATCHER_QUADRANTS,
	            "all 4 quadrants must have distinct cell keys")) {
		std::fprintf(stderr, "  distinct keys: %zu\n", keys_seen.size());
		return 1;
	}

	// Dispatching again at the same centre on frame 1 must HIT every slot and
	// NOT evict (occupancy stays at 4; no new keys introduced).
	out.clear();
	d.dispatch(0, 0, 0, 100.0f, 0x40000000, /*frame*/ 1, cfg, s, out);
	if (!expect(d.lru_occupancy() == DISPATCHER_QUADRANTS,
	            "second dispatch with same centre must not grow the LRU")) return 1;

	std::printf("OK: dispatcher walks 4 quadrants and caches them in the LRU\n");
	return 0;
}
