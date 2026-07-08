// The foliage MODEL-tier tile walk + per-def cache.
// [orig: Foliage_UpdateModelTiles @ 0x601f50 - 4 quadrant tiles, the
// 1000-entry per-def cache with last-touch frame stamps, evict-oldest on
// miss, regenerate-on-stagger; the depth gate is the caller's
// Terrain_RenderSectorEntitiesBySide @ 0x5c7d50. See
// docs/foliage/foliage-re.md §The tile walk.]
#include "foliage/model_dispatcher.h"

namespace opennova::foliage {

ModelDispatcher::ModelDispatcher() {
	entries_.resize(MODEL_CACHE_ENTRIES);
}

void ModelDispatcher::reset() noexcept {
	for (auto &entry : entries_) {
		entry = ModelCacheEntry{};
	}
	cache_hits_ = 0;
	cache_misses_ = 0;
	regenerations_ = 0;
}

bool ModelDispatcher::is_staggered_regen_frame(int32_t frame_counter, int slot_index) noexcept {
	// [orig: Foliage_UpdateModelTiles @ 0x601f50]: regenerate a cached tile
	// only when ((frame + 2 * def) & 7) == 0 - a 2-frame per-def stagger.
	return ((frame_counter + 2 * slot_index) & MODEL_STAGGER_MASK) == 0;
}

int ModelDispatcher::cache_occupancy() const noexcept {
	int n = 0;
	for (const auto &entry : entries_) {
		if (entry.occupied) {
			++n;
		}
	}
	return n;
}

namespace {

// The eviction scan: prefer an empty entry, else the max-age (smallest
// last_touched relative to now) occupied one - mirrors the quad dispatcher's
// walk and the retail evict-oldest-by-frame-stamp scan.
int find_evict_slot(const std::vector<ModelCacheEntry> &entries, int32_t now) noexcept {
	int best_idx = -1;
	int32_t best_age = -1;
	const int n = static_cast<int>(entries.size());
	for (int i = 0; i < n; ++i) {
		if (!entries[i].occupied) {
			return i;
		}
		const int32_t age = now - entries[i].last_touched;
		if (age > best_age) {
			best_age = age;
			best_idx = i;
		}
	}
	return best_idx;
}

} // namespace

void ModelDispatcher::walk(int slot_index,
                           Fixed16_16 anchor_x_fixed,
                           Fixed16_16 anchor_z_fixed,
                           float view_depth,
                           int32_t frame_counter,
                           const ModelPlacementConfig &config,
                           const PlacementSamplers &samplers,
                           std::vector<ModelTileDraw> &out) noexcept {
	if (slot_index < 0 || slot_index >= FOLIAGE_MAX_DEFS) {
		return;
	}

	// The whole walk is gated on the anchor's view-space depth
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50: only entities
	// with depth >= 38.0 dispatch model tiles - nearer ones render their
	// sector-entity geometry without a foliage cluster].
	if (view_depth < MODEL_DEPTH_GATE) {
		return;
	}

	const bool should_regen = is_staggered_regen_frame(frame_counter, slot_index);

	const auto tiles = model_quadrant_tiles(anchor_x_fixed, anchor_z_fixed);
	for (const auto &tile : tiles) {
		ModelCacheEntry *entry = nullptr;
		for (auto &candidate : entries_) {
			if (candidate.occupied && candidate.tile_key == tile.key) {
				entry = &candidate;
				entry->last_touched = frame_counter;
				++cache_hits_;
				if (should_regen) {
					entry->cached = generate_model_tile_instances(
					    slot_index, tile.key, anchor_x_fixed, anchor_z_fixed,
					    MODEL_CANDIDATE_RADIUS, config, samplers);
					++regenerations_;
				}
				break;
			}
		}

		if (entry == nullptr) {
			const int victim = find_evict_slot(entries_, frame_counter);
			if (victim < 0) {
				continue;  // unreachable with a non-empty cache; mirrors the quad walk's guard
			}
			entry = &entries_[victim];
			entry->tile_key = tile.key;
			entry->last_touched = frame_counter;
			entry->occupied = true;
			entry->cached = generate_model_tile_instances(
			    slot_index, tile.key, anchor_x_fixed, anchor_z_fixed,
			    MODEL_CANDIDATE_RADIUS, config, samplers);
			++cache_misses_;
		}

		// Draw only when count > 0 [orig: Foliage_UpdateModelTiles @ 0x601f50].
		if (entry->cached.count > 0) {
			ModelTileDraw draw;
			draw.tile_key = tile.key;
			draw.snap_x_fixed = tile.snap_x_fixed;
			draw.snap_z_fixed = tile.snap_z_fixed;
			draw.result = entry->cached;
			out.push_back(draw);
		}
	}
}

} // namespace opennova::foliage
