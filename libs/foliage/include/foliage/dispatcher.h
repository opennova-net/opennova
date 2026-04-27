#pragma once

// Engine: jodemo.exe sub_5C1940@0x5C1940
// docs/engine_spec_foliage.md 4.3, docs/engine_spec_integration.md 5.3
//
// Per-slot foliage dispatcher. Walks the four +/-8u quadrants around a
// visible entity, maintains a 128-entry LRU of cell placements, and re-bakes
// cells on an 8-frame staggered gate keyed by slot index.

#include <array>
#include <cstdint>
#include <vector>

#include "foliage/placement.h"

namespace opennova::foliage {

// Engine constants - byte-exact from the decomp.
constexpr int DISPATCHER_QUADRANTS = 4;        // sub_5C1940 loop v21 = 0..3
constexpr int DISPATCHER_LRU_SLOTS = 128;      // sub_5C1940 inner v8 = 128
constexpr int DISPATCHER_STAGGER_MASK = 7;     // ((frame + 2*slot) & 7) == 0
constexpr float DISPATCHER_NEAR_Z = 38.0f;     // sub_5C1940 @ 0x5c19a3
constexpr Fixed16_16 DISPATCHER_QUADRANT_OFFSET = 0x80000;  // +/-8.0
constexpr Fixed16_16 DISPATCHER_CELL_SIZE = 0x100000;       // 16.0
constexpr Fixed16_16 DISPATCHER_CELL_MASK = 0xFFF00000;     // aligns to 16u

struct LRUEntry {
	uint32_t cell_key = 0xFFFFFFFFu;  // 0xFFFFFFFF = empty
	int32_t last_touched = -1;        // frame counter value on last hit
	PlacementResult cached;
	bool occupied = false;
};

struct ZSortInstance {
	PlacementInstance instance;
	int slot_index;
	// Fidelity: bounded deviation. This wrapper still carries the dispatch-time
	// near-Z input until the exact per-instance sort key writer is isolated.
	float camera_z;
};

// Per-slot dispatcher state. One instance per foliage slot (0..3) per visible entity.
// In the engine this is a stripe of 128 x 342 dwords inside a larger per-entity buffer
// (unk_154A6C8 + 178844 * slot); the port splits it out per-slot for clarity.
class Dispatcher {
public:
	Dispatcher();

	// Reset LRU - call on config change or when the camera teleports far.
	void reset() noexcept;

	// Dispatch placement around `view_center` for this slot. `frame_counter` advances
	// once per frame (engine: dword_154A6B0). `out_zsort` is appended to; caller owns
	// the vector across frames and clears when rendering the Z-sorted batch.
	//
	// `slot_index`: which foliage def slot (0..3) this dispatcher represents.
	// `view_camera_z`: view-space Z of the entity center. If < DISPATCHER_NEAR_Z, the
	// entire dispatch is skipped (engine @ 0x5c19a3).
	// `view_radius`: 16.16 fixed L-infinity radius; passed through to place_cell.
	void dispatch(int slot_index,
	              Fixed16_16 view_center_x,
	              Fixed16_16 view_center_z,
	              float view_camera_z,
	              int32_t view_radius,
	              int32_t frame_counter,
	              const PlacementConfig &config,
	              const PlacementSamplers &samplers,
	              std::vector<ZSortInstance> &out_zsort) noexcept;

	// For testing / inspection.
	const std::array<LRUEntry, DISPATCHER_LRU_SLOTS> &lru_entries() const noexcept { return slots_; }
	int lru_occupancy() const noexcept;

	// Predicates exposed for tests.
	static bool is_staggered_bake_frame(int32_t frame_counter, int slot_index) noexcept;

private:
	std::array<LRUEntry, DISPATCHER_LRU_SLOTS> slots_;
};

} // namespace opennova::foliage
