#pragma once

// The foliage MODEL-tier tile walk + per-def cache, ported from retail
// Jointops.exe [orig: Foliage_UpdateModelTiles @ 0x601f50, driven per visible
// sector entity by Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]. Mirrors
// the FAR tier's Dispatcher shape (dispatcher.h): one instance per foliage
// def slot; the host walks it once per anchor per frame.
//
// Witnessed behavior: gate the whole walk on the anchor's view-space depth
// >= 38.0; walk the 4 quadrant tiles (16u cells overlapping anchor +-8u);
// per-def 1000-entry cache keyed by tile key with a last-touch frame stamp
// (retail: 342-dword entries at Foliage_ModelTileCachePerDef @ 0x2C266F0);
// on hit touch and REGENERATE only on the 8-frame stagger
// ((frame + 2*def) & 7) == 0; on miss evict the max-age entry and generate;
// draw only when count > 0.

#include <cstdint>
#include <vector>

#include "foliage/model_placement.h"

namespace opennova::foliage {

// Engine constants - byte-exact from the retail decomp.
constexpr int MODEL_CACHE_ENTRIES = 1000;      // per-def cache size @ 0x2C266F0
constexpr int MODEL_STAGGER_MASK = 7;          // ((frame + 2*def) & 7) == 0
constexpr float MODEL_DEPTH_GATE = 38.0f;      // view-space depth gate @ 0x5c7d50

struct ModelCacheEntry {
	uint32_t tile_key = 0xFFFFFFFFu;
	int32_t last_touched = -1;  // frame stamp on last hit (retail entry dword [1])
	ModelTileResult cached;
	bool occupied = false;
};

// One walked tile emitted to the host: the packed key, the snapped 16u tile
// origin, and a copy of the current cache content (retail draws straight from
// the cache entry per tile; the copy keeps the out list stable across later
// evictions in the same frame).
struct ModelTileDraw {
	uint32_t tile_key = 0;
	Fixed16_16 snap_x_fixed = 0;
	Fixed16_16 snap_z_fixed = 0;
	ModelTileResult result;
};

// Per-slot model-tier dispatcher state. One instance per foliage def slot
// (0..3); in the engine this is a stripe of 1000 x 342 dwords per def
// (Foliage_ModelTileCachePerDef @ 0x2C266F0). Heap-backed (the cache is
// ~1.7 MB per slot).
class ModelDispatcher {
public:
	ModelDispatcher();

	// Drop the cache and counters - call on config change / terrain reload.
	void reset() noexcept;

	// Walk the 4 quadrant tiles around `anchor` for this slot and append the
	// current per-tile instance lists to `out` (only tiles with count > 0 -
	// retail draws only when the entry count is nonzero). `frame_counter`
	// advances once per frame. `view_depth` is the anchor's view-space depth;
	// the whole walk is skipped below MODEL_DEPTH_GATE
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50].
	void walk(int slot_index,
	          Fixed16_16 anchor_x_fixed,
	          Fixed16_16 anchor_z_fixed,
	          float view_depth,
	          int32_t frame_counter,
	          const ModelPlacementConfig &config,
	          const PlacementSamplers &samplers,
	          std::vector<ModelTileDraw> &out) noexcept;

	// The 8-frame regen stagger [orig: Foliage_UpdateModelTiles @ 0x601f50]:
	// ((frame + 2 * slot) & 7) == 0 - same gate family as the FAR tier.
	static bool is_staggered_regen_frame(int32_t frame_counter, int slot_index) noexcept;

	// Introspection for hosts/tests.
	int cache_occupancy() const noexcept;
	int64_t cache_hits() const noexcept { return cache_hits_; }
	int64_t cache_misses() const noexcept { return cache_misses_; }
	int64_t regenerations() const noexcept { return regenerations_; }

private:
	std::vector<ModelCacheEntry> entries_;
	int64_t cache_hits_ = 0;
	int64_t cache_misses_ = 0;
	int64_t regenerations_ = 0;
};

} // namespace opennova::foliage
