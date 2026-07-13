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
	key_index_.reserve(MODEL_CACHE_ENTRIES * 2);
}

void ModelDispatcher::reset() noexcept {
	for (auto &entry : entries_) {
		entry = ModelCacheEntry{};
	}
	key_index_.clear();
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
// last_touched relative to now) occupied one - the retail
// evict-oldest-by-frame-stamp scan. Same-frame entries ARE eligible victims:
// retail recycles and draws immediately when the live tile set outruns the
// cache, and every ModelTileDraw view dies at the next walk() (the header
// contract), so recycling cannot dangle a retained host draw. The only
// exclusions are the entries THIS walk call already emitted into `out`
// (`used`, at most the 4 quadrant tiles) - recycling one of those would
// invalidate a view before the call even returned.
int find_evict_slot(const std::vector<ModelCacheEntry> &entries, int32_t now,
                    const int *used, int used_count) noexcept {
	int best_idx = -1;
	int32_t best_age = -1;
	const int n = static_cast<int>(entries.size());
	for (int i = 0; i < n; ++i) {
		bool in_use = false;
		for (int u = 0; u < used_count; ++u) {
			if (used[u] == i) {
				in_use = true;
				break;
			}
		}
		if (in_use) {
			continue;
		}
		if (!entries[i].occupied) {
			return i;
		}
		const int32_t age = now - entries[i].last_touched;
		if (age > best_age) {
			best_age = age;
			best_idx = i;
		}
	}
	return best_idx;  // >= 0 whenever entries.size() > used_count
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
	// Entry indices this call has touched so far - excluded from eviction so
	// an emitted view survives to the end of the call (see find_evict_slot).
	int used_entries[4] = {};
	int used_count = 0;
	for (const auto &tile : tiles) {
		ModelCacheEntry *entry = nullptr;
		int entry_index = -1;
		// Keyed lookup. The engine linear-scans its 1000-entry stripe per tile
		// [orig: the unrolled key scan in Foliage_UpdateModelTiles @ 0x601f50]
		// - a hash index over the same entries is a host data-structure choice
		// with identical hit/evict/stamp semantics (~2.3M compares/frame at
		// jungle-map anchor density otherwise).
		auto found = key_index_.find(tile.key);
		if (found != key_index_.end()) {
			entry = &entries_[found->second];
			entry_index = found->second;
			entry->last_touched = frame_counter;
			++cache_hits_;
			// Stagger regen runs once per TOUCHING ANCHOR, exactly as retail
			// [orig: Foliage_UpdateModelTiles @ 0x601f50 regenerates per hit].
			// The repeats are NOT byte-identical: generate's accept gate is
			// anchor-relative (|world - anchor| <= 0x40000), so anchors
			// sharing a tile produce different subsets — each regen
			// overwrites the entry and the LAST touching anchor's subset is
			// the frame's end state (the same last-wins the retained draw
			// keeps per D-FOLIAGE-10). A same-frame dedup here froze the
			// FIRST anchor's subset into the cache instead.
			if (should_regen) {
				entry->cached = generate_model_tile_instances(
				    slot_index, tile.key, anchor_x_fixed, anchor_z_fixed,
				    MODEL_CANDIDATE_RADIUS, config, samplers);
				entry->generation = ++generation_stamp_;
				++regenerations_;
			}
		}

		if (entry == nullptr) {
			const int victim =
			    find_evict_slot(entries_, frame_counter, used_entries, used_count);
			ModelCacheEntry &victim_entry = entries_[victim];
			if (victim_entry.occupied) {
				key_index_.erase(victim_entry.tile_key);
			}
			entry = &victim_entry;
			entry_index = victim;
			entry->tile_key = tile.key;
			entry->last_touched = frame_counter;
			entry->occupied = true;
			entry->cached = generate_model_tile_instances(
			    slot_index, tile.key, anchor_x_fixed, anchor_z_fixed,
			    MODEL_CANDIDATE_RADIUS, config, samplers);
			entry->generation = ++generation_stamp_;
			++cache_misses_;
			key_index_.emplace(tile.key, victim);
		}
		used_entries[used_count++] = entry_index;

		// Draw only when count > 0 [orig: Foliage_UpdateModelTiles @ 0x601f50].
		if (entry->cached.count > 0) {
			ModelTileDraw draw;
			draw.tile_key = tile.key;
			draw.snap_x_fixed = tile.snap_x_fixed;
			draw.snap_z_fixed = tile.snap_z_fixed;
			draw.generation = entry->generation;
			draw.instances = entry->cached.instances.data();
			draw.count = entry->cached.count;
			out.push_back(draw);
		}
	}
}

} // namespace opennova::foliage
