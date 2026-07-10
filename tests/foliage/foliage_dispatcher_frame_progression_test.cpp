// Locks in that Dispatcher::dispatch actually re-bakes cells on the expected
// stagger frames when driven every-frame (the engine's cadence — see
// docs/engine_spec_foliage.md §4.3). Complement to
// foliage_dispatcher_stagger_test which only probes the gate helper.

#include <foliage/dispatcher.h>

#include <cstdio>
#include <cstdint>
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

// Candidate placement calls slot_mask_at during a bake. Terrain height now
// belongs to full source-mesh emission, so cache/cadence evidence must use the
// slot-mask calls rather than removed synthetic-quad height samples.
struct CountingSamplers {
	int slot_mask_calls = 0;
};

PlacementSamplers make_samplers(CountingSamplers &counters) {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [&counters](Fixed16_16, Fixed16_16) -> uint32_t {
		++counters.slot_mask_calls;
		return 0xFu;  // allow all four slots
	};
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };
	return s;
}

} // namespace

int main() {
	// Drive 16 frames of every-frame dispatch for slot 0. Engine stagger gate:
	// ((frame + 2*0) & 7) == 0 → frames 0, 8, 16, ...
	// LRU misses also force a re-bake (not gated by stagger) on the first
	// call; subsequent calls at matching cells should only re-bake at the
	// staggered frames.
	{
		Dispatcher d;
		CountingSamplers cs;
		PlacementSamplers s = make_samplers(cs);
		PlacementConfig cfg{};
		cfg.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;  // skip path_blocked path

		std::vector<ZSortInstance> out;

		// Frame 0: stagger gate fires AND first LRU miss → cells baked.
		out.clear();
		d.dispatch(0, 0, 0, 50.0f, 0x40000000, 0, cfg, s, out);
		const int hits_after_frame_0 = cs.slot_mask_calls;
		if (!expect(hits_after_frame_0 > 0, "frame 0 must bake (both LRU-miss and stagger)")) return 1;

		// Frames 1..7: same centre, LRU hit, stagger gate closed → no sampler calls.
		for (int32_t f = 1; f < 8; ++f) {
			const int before = cs.slot_mask_calls;
			out.clear();
			d.dispatch(0, 0, 0, 50.0f, 0x40000000, f, cfg, s, out);
			const int after = cs.slot_mask_calls;
			if (!expect(after == before,
			            "intra-window frames must not re-bake when stagger gate is closed")) {
				std::fprintf(stderr, "  slot=0 frame=%d slot_mask_calls delta=%d\n", f, after - before);
				return 1;
			}
			// Output still populated from the cached instances every frame.
			// (The dispatcher emits ZSortInstances on every call.)
		}

		// Frame 8: stagger gate reopens → re-bake (and we still get output).
		const int before_8 = cs.slot_mask_calls;
		out.clear();
		d.dispatch(0, 0, 0, 50.0f, 0x40000000, 8, cfg, s, out);
		if (!expect(cs.slot_mask_calls > before_8,
		            "frame 8 must re-bake on the next stagger boundary")) return 1;
	}

	// Sanity: different slot phases land on different boundary frames.
	// Slot 0 → f%8==0, slot 1 → f%8==6, slot 2 → f%8==4, slot 3 → f%8==2.
	{
		for (int slot = 0; slot < FOLIAGE_MAX_DEFS; ++slot) {
			Dispatcher d;
			CountingSamplers cs;
			PlacementSamplers s = make_samplers(cs);
			PlacementConfig cfg{};
			cfg.attrib_flags[slot] = FOLIAGE_ATTRIB_FORCE_ON;

			std::vector<ZSortInstance> out;

			// Warm the LRU on a frame where the gate is closed so the only
			// subsequent bakes are stagger-driven. For slot 0 f=1 is closed,
			// for slot 1 f=0 is closed, slot 2 f=0 closed, slot 3 f=0 closed.
			// Use f where ((f + 2*slot) & 7) != 0 to warm.
			int warm_frame = 1;
			while (Dispatcher::is_staggered_bake_frame(warm_frame, slot)) {
				++warm_frame;
			}

			out.clear();
			d.dispatch(slot, 0, 0, 50.0f, 0x40000000, warm_frame, cfg, s, out);

			// Now walk 16 frames and count stagger-driven re-bakes.
			int rebake_count = 0;
			int prev = cs.slot_mask_calls;
			for (int32_t f = warm_frame + 1; f < warm_frame + 1 + 16; ++f) {
				out.clear();
				d.dispatch(slot, 0, 0, 50.0f, 0x40000000, f, cfg, s, out);
				if (cs.slot_mask_calls > prev) {
					++rebake_count;
					prev = cs.slot_mask_calls;
				}
			}
			// Over 16 frames, stagger gate fires exactly twice.
			if (!expect(rebake_count == 2,
			            "each slot should re-bake exactly 2× over a 16-frame window")) {
				std::fprintf(stderr, "  slot %d saw %d re-bakes (expected 2)\n", slot, rebake_count);
				return 1;
			}
		}
	}

	std::printf("OK: every-frame dispatch honours the 8-frame stagger gate per slot\n");
	return 0;
}
