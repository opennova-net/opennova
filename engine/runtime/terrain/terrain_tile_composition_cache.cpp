#include <runtime/terrain/terrain_tile_composition_cache.h>

// [orig: PolyTrn_RenderTile @ 0x60DA70; 128-slot cache dword_319A2E4;
// docs/tiles/til-re.md]

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <iterator>

namespace opennova {

namespace {

bool same_page(const TerrainTilePageKey &a, const TerrainTilePageKey &b) noexcept {
	return a.sector_origin_x == b.sector_origin_x &&
			a.sector_origin_z == b.sector_origin_z &&
			a.page_local_x == b.page_local_x &&
			a.page_local_z == b.page_local_z &&
			a.page_lod_level == b.page_lod_level;
}

} // namespace

TerrainTileCompositionDemandEnqueueResult
TerrainTileCompositionDemandQueue::enqueue(
		const TerrainTileCompositionDemand &demand,
		std::size_t maximum_size) {
	TerrainTileCompositionDemandEnqueueResult result;
	for (const TerrainTileCompositionDemand &queued : demands_) {
		if (queued.layer == demand.layer &&
				queued.generation > demand.generation) {
			result.rejected_stale = true;
			return result;
		}
	}
	demands_.erase(std::remove_if(demands_.begin(), demands_.end(),
			[&](const TerrainTileCompositionDemand &queued) {
				if (queued.layer != demand.layer) return false;
				result.removed_sequences.push_back(queued.sequence);
				return true;
			}), demands_.end());
	if (demands_.size() >= maximum_size) return result;
	demands_.push_back(demand);
	result.accepted = true;
	return result;
}

std::vector<uint64_t>
TerrainTileCompositionDemandQueue::remove_older_generations(
		uint16_t layer, uint64_t generation) {
	std::vector<uint64_t> removed;
	demands_.erase(std::remove_if(demands_.begin(), demands_.end(),
			[&](const TerrainTileCompositionDemand &queued) {
				if (queued.layer != layer ||
						queued.generation >= generation) {
					return false;
				}
				removed.push_back(queued.sequence);
				return true;
			}), demands_.end());
	return removed;
}

std::optional<TerrainTileCompositionDemand>
TerrainTileCompositionDemandQueue::take_next() {
	if (demands_.empty()) return std::nullopt;
	auto selected = demands_.begin();
	for (auto candidate = std::next(demands_.begin());
			candidate != demands_.end(); ++candidate) {
		if (candidate->frame_id > selected->frame_id ||
				(candidate->frame_id == selected->frame_id &&
						candidate->sequence < selected->sequence)) {
			selected = candidate;
		}
	}
	const TerrainTileCompositionDemand result = *selected;
	demands_.erase(selected);
	return result;
}

int TerrainTilePageLayout::texel_footprint(int world_units) const noexcept {
	return static_cast<int>(
			std::lround(static_cast<float>(world_units) * texels_per_world_unit));
}

// The c7/c8 page projection retail uploads per page [orig: Foliage_RenderFarPatches
//  @0x60a220..0x60a34f; Foliage_SetupVertexShaderConstants @0x6006ab..0x600704].
std::array<float, 2> TerrainTilePageProjection::project(
		float world_x, float world_z) const noexcept {
	return {
			(world_x - world_origin_x) * inverse_world_span,
			(world_z - world_origin_z) * inverse_world_span,
	};
}

std::optional<TerrainTilePageProjection>
TerrainTileCompositionCache::page_projection(
		const TerrainTilePageKey &page, bool zero_primary_uv) noexcept {
	const int span = page_world_span(page.page_lod_level);
	if (span <= 0) return std::nullopt;
	const int64_t origin_x = static_cast<int64_t>(page.sector_origin_x) +
			page.page_local_x;
	const int64_t origin_z = static_cast<int64_t>(page.sector_origin_z) +
			page.page_local_z;
	return TerrainTilePageProjection{
			static_cast<float>(origin_x),
			static_cast<float>(origin_z),
			// [orig: decode_terrain_tile_vertices @ 0x602AA0, UV stores @ 0x602DCC..0x602DCF]
			zero_primary_uv ? 0.0f : 1.0f / static_cast<float>(span),
			static_cast<float>(span),
	};
}

void TerrainTileCompositionCache::begin_frame(uint64_t frame_id) noexcept {
	if (frame_active_ && frame_id_ == frame_id) {
		return;
	}
	frame_active_ = true;
	frame_id_ = frame_id;
	for (uint16_t layer = 0; layer < used_; ++layer) {
		slots_[layer].pinned = false;
		slots_[layer].selected_in_frame = false;
	}
}

std::optional<TerrainTileCompositionDecision> TerrainTileCompositionCache::request(
		const TerrainTileCompositionRequest &request_value) {
	const int span = page_world_span(request_value.page.page_lod_level);
	if (span == 0) {
		return std::nullopt;
	}
	const uint64_t touch = ++touch_clock_;

	for (uint16_t layer = 0; layer < used_; ++layer) {
		Slot &slot = slots_[layer];
		if (!slot.occupied || !same_page(slot.page, request_value.page)) {
			continue;
		}
		slot.last_touch = touch;
		if (frame_active_) {
			slot.pinned = true;
			slot.selected_frame_id = frame_id_;
			slot.selected_in_frame = true;
		}
		if (slot.tile_index == request_value.tile_index &&
				slot.source_origin_x == request_value.source_origin_x &&
				slot.source_origin_z == request_value.source_origin_z &&
				slot.content.value == request_value.content.value) {
			if (slot.ready || slot.pending) {
				return TerrainTileCompositionDecision{
						TerrainTilePageBinding{slot.page, layer,
								slot.generation, slot.ready, slot.stale},
						std::nullopt};
			}
		}

		// The spatial page remains in its layer while changed source inputs or
		// contributor content compile a new generation. A ready payload keeps
		// serving, marked stale, until the replacement publishes — a one-frame-
		// stale shadow is invisible; a missing one is not.
		slot.stale = slot.ready;
		slot.pending = true;
		slot.tile_index = request_value.tile_index;
		slot.source_origin_x = request_value.source_origin_x;
		slot.source_origin_z = request_value.source_origin_z;
		slot.content = request_value.content;
		++slot.generation;
		const TerrainTilePageBinding binding{slot.page, layer,
				slot.generation, slot.ready, slot.stale};
		const TerrainTileCompositionJob job{
				binding,
				request_value.tile_index,
				request_value.source_origin_x,
				request_value.source_origin_z,
				request_value.content,
				TerrainTilePageLayout{
						kDimension, span,
						static_cast<float>(kDimension) / static_cast<float>(span)}};
		return TerrainTileCompositionDecision{binding, job};
	}

	uint16_t layer = 0;
	if (used_ < kCapacity) {
		layer = used_++;
	} else {
		std::optional<uint16_t> eviction_layer;
		for (uint16_t candidate = 0; candidate < kCapacity; ++candidate) {
			if (slots_[candidate].pinned) {
				continue;
			}
			if (!eviction_layer.has_value() ||
					slots_[candidate].last_touch <
						slots_[*eviction_layer].last_touch) {
				eviction_layer = candidate;
			}
		}
		if (!eviction_layer.has_value()) {
			return std::nullopt;
		}
		layer = *eviction_layer;
	}

	TerrainTilePageBinding binding;
	binding.page = request_value.page;
	binding.layer = layer;
	Slot &slot = slots_[layer];
	slot.occupied = true;
	slot.ready = false;
	slot.pending = true;
	// An evicted layer's payload belongs to another spatial page — never
	// stale-serve it under the new page.
	slot.stale = false;
	slot.page = request_value.page;
	slot.tile_index = request_value.tile_index;
	slot.source_origin_x = request_value.source_origin_x;
	slot.source_origin_z = request_value.source_origin_z;
	slot.content = request_value.content;
	++slot.generation;
	slot.last_touch = touch;
	slot.pinned = frame_active_;
	slot.selected_frame_id = frame_id_;
	slot.selected_in_frame = frame_active_;
	binding.generation = slot.generation;

	TerrainTileCompositionJob job;
	job.target = binding;
	job.tile_index = request_value.tile_index;
	job.source_origin_x = request_value.source_origin_x;
	job.source_origin_z = request_value.source_origin_z;
	job.content = request_value.content;
	job.layout.texture_dimension = kDimension;
	job.layout.world_span = span;
	job.layout.texels_per_world_unit =
			static_cast<float>(kDimension) /
			static_cast<float>(job.layout.world_span);

	return TerrainTileCompositionDecision{binding, job};
}

bool TerrainTileCompositionCache::can_publish(
		const TerrainTileCompositionJob &job) const noexcept {
	if (job.target.layer >= used_) {
		return false;
	}
	const Slot &slot = slots_[job.target.layer];
	if (!slot.occupied || !slot.pending ||
			slot.generation != job.target.generation ||
			!same_page(slot.page, job.target.page) ||
			slot.tile_index != job.tile_index ||
			slot.source_origin_x != job.source_origin_x ||
			slot.source_origin_z != job.source_origin_z ||
			slot.content.value != job.content.value) {
		return false;
	}
	return true;
}

bool TerrainTileCompositionCache::publish(
		const TerrainTileCompositionJob &job) noexcept {
	if (!can_publish(job)) {
		return false;
	}
	Slot &slot = slots_[job.target.layer];
	slot.ready = true;
	slot.pending = false;
	slot.stale = false;
	return true;
}

bool TerrainTileCompositionCache::invalidate(
		const TerrainTilePageKey &page) noexcept {
	for (uint16_t layer = 0; layer < used_; ++layer) {
		Slot &slot = slots_[layer];
		if (!slot.occupied || !same_page(slot.page, page)) {
			continue;
		}
		slot.ready = false;
		slot.pending = false;
		slot.stale = false;
		++slot.generation;
		return true;
	}
	return false;
}

// Page extent from the per-LOD world span [orig: PolyTrn_RenderTile @0x60da70 page LOD /
//  128-slot cache; the overlap invalidation walk Terrain_AddScorchRecord @0x605c90].
bool TerrainTileCompositionCache::page_overlaps_q16(
		const TerrainTilePageKey &page,
		int32_t minimum_x_q16, int32_t minimum_z_q16,
		int32_t maximum_x_q16, int32_t maximum_z_q16) noexcept {
	const int span = page_world_span(page.page_lod_level);
	if (span == 0 || minimum_x_q16 > maximum_x_q16 ||
			minimum_z_q16 > maximum_z_q16) {
		return false;
	}
	const int64_t page_minimum_x =
			(static_cast<int64_t>(page.sector_origin_x) + page.page_local_x) << 16;
	const int64_t page_minimum_z =
			(static_cast<int64_t>(page.sector_origin_z) + page.page_local_z) << 16;
	const int64_t page_maximum_x = page_minimum_x +
			(static_cast<int64_t>(span) << 16);
	const int64_t page_maximum_z = page_minimum_z +
			(static_cast<int64_t>(span) << 16);
	return static_cast<int64_t>(minimum_x_q16) <= page_maximum_x &&
			static_cast<int64_t>(maximum_x_q16) >= page_minimum_x &&
			static_cast<int64_t>(minimum_z_q16) <= page_maximum_z &&
			static_cast<int64_t>(maximum_z_q16) >= page_minimum_z;
}

std::size_t TerrainTileCompositionCache::invalidate_overlapping_q16(
		int32_t minimum_x_q16, int32_t minimum_z_q16,
		int32_t maximum_x_q16, int32_t maximum_z_q16) noexcept {
	std::size_t invalidated = 0;
	for (uint16_t layer = 0; layer < used_; ++layer) {
		Slot &slot = slots_[layer];
		if (!slot.occupied || !page_overlaps_q16(slot.page,
				minimum_x_q16, minimum_z_q16,
				maximum_x_q16, maximum_z_q16)) {
			continue;
		}
		slot.ready = false;
		slot.pending = false;
		slot.stale = false;
		++slot.generation;
		++invalidated;
	}
	return invalidated;
}

void TerrainTileCompositionCache::invalidate_all() noexcept {
	for (uint16_t layer = 0; layer < used_; ++layer) {
		Slot &slot = slots_[layer];
		if (!slot.occupied) {
			continue;
		}
		slot.ready = false;
		slot.pending = false;
		slot.stale = false;
		++slot.generation;
	}
}

void TerrainTileCompositionCache::clear() noexcept {
	for (uint16_t layer = 0; layer < used_; ++layer) {
		Slot &slot = slots_[layer];
		slot.occupied = false;
		slot.ready = false;
		slot.pending = false;
		slot.stale = false;
		slot.page = {};
		slot.tile_index = -1;
		slot.source_origin_x = 0;
		slot.source_origin_z = 0;
		slot.content = {};
		++slot.generation;
		slot.last_touch = 0;
		slot.pinned = false;
		slot.selected_frame_id = 0;
		slot.selected_in_frame = false;
	}
	used_ = 0;
	touch_clock_ = 0;
	frame_id_ = 0;
	frame_active_ = false;
}

std::optional<TerrainTilePageBinding> TerrainTileCompositionCache::best_ready(
		const TerrainTileResidentPoint &point) noexcept {
	int best_layer = -1;
	for (uint16_t layer = 0; layer < used_; ++layer) {
		const Slot &slot = slots_[layer];
		if (!slot.occupied || !slot.ready ||
				(frame_active_ && (!slot.selected_in_frame ||
						slot.selected_frame_id != frame_id_)) ||
				slot.page.sector_origin_x != point.sector_origin_x ||
				slot.page.sector_origin_z != point.sector_origin_z) {
			continue;
		}
		// The flat page can pass this probe at its canonical sector origin.
		// Retail masks the packed coordinate without rejecting its high bit.
		// [orig: terrain_tile_cache_lookup @ 0x604140, probe @ 0x6041A4..0x6041E1]
		const int span = page_world_span(slot.page.page_lod_level);
		const double minimum_x = static_cast<double>(slot.page.sector_origin_x) +
				static_cast<double>(slot.page.page_local_x);
		const double minimum_z = static_cast<double>(slot.page.sector_origin_z) +
				static_cast<double>(slot.page.page_local_z);
		const double maximum_x = minimum_x + static_cast<double>(span);
		const double maximum_z = minimum_z + static_cast<double>(span);
		if (static_cast<double>(point.world_x) < minimum_x ||
				static_cast<double>(point.world_x) >= maximum_x ||
				static_cast<double>(point.world_z) < minimum_z ||
				static_cast<double>(point.world_z) >= maximum_z) {
			continue;
		}

		if (best_layer < 0) {
			best_layer = layer;
			continue;
		}
		const Slot &best = slots_[static_cast<size_t>(best_layer)];
		if (slot.page.page_lod_level > best.page.page_lod_level ||
				(slot.page.page_lod_level == best.page.page_lod_level &&
						slot.last_touch > best.last_touch) ||
				(slot.page.page_lod_level == best.page.page_lod_level &&
						slot.last_touch == best.last_touch && layer < best_layer)) {
			best_layer = layer;
		}
	}

	if (best_layer < 0) {
		return std::nullopt;
	}
	Slot &best = slots_[static_cast<size_t>(best_layer)];
	best.last_touch = ++touch_clock_;
	if (frame_active_) {
		best.pinned = true;
	}
	return TerrainTilePageBinding{
			best.page,
			static_cast<uint16_t>(best_layer),
			best.generation,
			true,
			best.stale};
}

} // namespace opennova
