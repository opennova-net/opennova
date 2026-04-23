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
	// Engine gate: ((frame + 2 * slot_index) & 7) == 0.
	// Slot 0: frames {0, 8, 16, ...}
	// Slot 1: frames {6, 14, 22, ...}  (needs (frame + 2) % 8 == 0 → frame % 8 == 6)
	// Slot 2: frames {4, 12, 20, ...}
	// Slot 3: frames {2, 10, 18, ...}

	for (int32_t frame = 0; frame < 64; ++frame) {
		const bool s0 = Dispatcher::is_staggered_bake_frame(frame, 0);
		const bool s1 = Dispatcher::is_staggered_bake_frame(frame, 1);
		const bool s2 = Dispatcher::is_staggered_bake_frame(frame, 2);
		const bool s3 = Dispatcher::is_staggered_bake_frame(frame, 3);

		if (!expect(s0 == ((frame & 7) == 0), "slot 0 gate mismatch")) return 1;
		if (!expect(s1 == ((frame & 7) == 6), "slot 1 gate mismatch")) return 1;
		if (!expect(s2 == ((frame & 7) == 4), "slot 2 gate mismatch")) return 1;
		if (!expect(s3 == ((frame & 7) == 2), "slot 3 gate mismatch")) return 1;

		// Exactly one slot can fire per (frame % 2) boundary; in particular, across
		// any 8 consecutive frames each slot fires exactly once.
	}

	// Count bakes across 8-frame window per slot: exactly one each.
	for (int slot = 0; slot < FOLIAGE_MAX_DEFS; ++slot) {
		int bakes = 0;
		for (int32_t frame = 0; frame < 8; ++frame) {
			if (Dispatcher::is_staggered_bake_frame(frame, slot)) {
				++bakes;
			}
		}
		if (!expect(bakes == 1, "each slot must bake exactly once per 8-frame window")) {
			std::fprintf(stderr, "  slot %d baked %d times in 8 frames\n", slot, bakes);
			return 1;
		}
	}

	std::printf("OK: 8-frame stagger gate fires each slot exactly once per window\n");
	return 0;
}
