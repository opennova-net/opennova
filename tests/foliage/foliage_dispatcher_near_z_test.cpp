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

	// Camera-space Z below the 38.0 near plane: entire dispatch must be skipped.
	d.dispatch(0, 0, 0, /*camera_z*/ 30.0f, 0x40000000, 0, cfg, s, out);
	if (!expect(d.lru_occupancy() == 0, "near-Z reject must leave LRU empty")) return 1;
	if (!expect(out.empty(), "near-Z reject must emit no instances")) return 1;

	// Camera-space Z just above 38.0: dispatch proceeds.
	d.dispatch(0, 0, 0, /*camera_z*/ 38.5f, 0x40000000, 0, cfg, s, out);
	if (!expect(d.lru_occupancy() == DISPATCHER_QUADRANTS,
	            "dispatch at camera_z > 38 must populate 4 LRU slots")) return 1;

	// Exactly at 38.0: engine comparison is `v23 >= 38.0`, so this should pass.
	d.reset();
	out.clear();
	d.dispatch(0, 0x80000, 0, /*camera_z*/ 38.0f, 0x40000000, 0, cfg, s, out);
	if (!expect(d.lru_occupancy() == DISPATCHER_QUADRANTS,
	            "camera_z exactly 38.0 must NOT reject (engine uses >=)")) return 1;

	// Just below 38.0: must reject (asserts the gate is strict, not a loose
	// "anything < 30 rejects" heuristic).
	d.reset();
	out.clear();
	d.dispatch(0, 0x80000, 0, /*camera_z*/ 37.9999f, 0x40000000, 0, cfg, s, out);
	if (!expect(d.lru_occupancy() == 0,
	            "camera_z just below 38.0 must reject (gate is strict)")) return 1;
	if (!expect(out.empty(),
	            "camera_z just below 38.0 must emit no instances")) return 1;

	std::printf("OK: dispatcher enforces the 38.0 near-Z gate (strict, inclusive at 38.0)\n");
	return 0;
}
