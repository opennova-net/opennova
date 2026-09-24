#include <runtime/terrain/terrain_tile_composition_cache.h>

// [orig: PolyTrn_RenderTile @ 0x60DA70; the 128-record cache dword_319A2E0;
// docs/terrain/terrain-re.md]

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <iterator>

namespace opennova {

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

void TerrainTileCompositionCache::begin_frame(uint32_t tod_epoch) noexcept {
	++frame_;
	tod_epoch_ = tod_epoch;
}

bool TerrainTileCompositionCache::same_identity(const Slot &slot,
		const TerrainTileCompositionRequest &request) noexcept {
	return slot.lod_valid && slot.resident &&
			same_page(slot.page, request.page) &&
			slot.source_origin_x == request.source_origin_x &&
			slot.source_origin_z == request.source_origin_z;
}

TerrainTilePageBinding TerrainTileCompositionCache::binding(
		const Slot &slot, uint16_t layer) const noexcept {
	return TerrainTilePageBinding{slot.page, layer, slot.generation, slot.ready};
}

void TerrainTileCompositionCache::retire(Slot &slot) noexcept {
	slot.resident = false;
	slot.ready = false;
	++slot.generation;
}

std::optional<TerrainTileCompositionDecision> TerrainTileCompositionCache::request(
		const TerrainTileCompositionRequest &request_value) {
	const int span = page_world_span(request_value.page.page_lod_level);
	if (span == 0) {
		return std::nullopt;
	}
	// The hit compare keys on the whole record identity: level, packed source
	// coordinate and routed sector; a hit refreshes the last use only.
	// [orig: PolyTrn_RenderTile @ 0x60DAC0..0x60DAD1, hit stamp
	// @ 0x60DDB8..0x60DDC0]
	for (uint16_t layer = 0; layer < kCapacity; ++layer) {
		Slot &slot = slots_[layer];
		if (!same_identity(slot, request_value)) continue;
		slot.last_use = frame_;
		return TerrainTileCompositionDecision{binding(slot, layer), std::nullopt};
	}

	// A miss claims the record with the largest last-use age, first in record
	// order on a tie, and only one whose age exceeds 1: a record used in this
	// frame or the previous one never yields. None -> the page stays
	// uncomposed and the patch draws with no t0.
	// [orig: @ 0x60DAE4..0x60DB45, empty-handed return @ 0x60DDC6]
	int32_t oldest_age = 1;
	int claimed = -1;
	for (int layer = 0; layer < kCapacity; ++layer) {
		const int32_t age =
				static_cast<int32_t>(frame_ - slots_[static_cast<size_t>(layer)].last_use);
		if (age > oldest_age) {
			oldest_age = age;
			claimed = layer;
		}
	}
	if (claimed < 0) {
		return std::nullopt;
	}

	// [orig: the claim stamps @ 0x60DB56..0x60DBF0 — last use and compose
	// frame = frame, TOD epoch = Env_TodMinutesElapsed, then lod, packed
	// coordinate and sector]
	Slot &slot = slots_[static_cast<size_t>(claimed)];
	slot.lod_valid = true;
	slot.resident = true;
	slot.ready = false;
	slot.page = request_value.page;
	slot.tile_index = request_value.tile_index;
	slot.source_origin_x = request_value.source_origin_x;
	slot.source_origin_z = request_value.source_origin_z;
	slot.last_use = frame_;
	slot.compose_frame = frame_;
	slot.tod_epoch = tod_epoch_;
	++slot.generation;

	const uint16_t layer = static_cast<uint16_t>(claimed);
	TerrainTileCompositionJob job;
	job.target = binding(slot, layer);
	job.tile_index = request_value.tile_index;
	job.source_origin_x = request_value.source_origin_x;
	job.source_origin_z = request_value.source_origin_z;
	job.layout.texture_dimension = kDimension;
	job.layout.world_span = span;
	job.layout.texels_per_world_unit =
			static_cast<float>(kDimension) / static_cast<float>(span);
	return TerrainTileCompositionDecision{job.target, job};
}

bool TerrainTileCompositionCache::evict_one_tod_stale() noexcept {
	// [orig: terrain_cache_evict_lru @ 0x604600 — candidates @ 0x604620..
	// 0x6046AA (lod and coordinate valid, TOD stamp != Env_TodMinutesElapsed,
	// compose age above the best so far, starting at 1), retire @ 0x6046C5..
	// 0x6046EA (lod = coordinate = -1, last use = frame - 0x10000)]
	int32_t oldest_age = 1;
	int evicted = -1;
	for (int layer = 0; layer < kCapacity; ++layer) {
		const Slot &slot = slots_[static_cast<size_t>(layer)];
		if (!slot.lod_valid || !slot.resident || slot.tod_epoch == tod_epoch_) {
			continue;
		}
		const int32_t age = static_cast<int32_t>(frame_ - slot.compose_frame);
		if (age > oldest_age) {
			oldest_age = age;
			evicted = layer;
		}
	}
	if (evicted < 0) {
		return false;
	}
	Slot &slot = slots_[static_cast<size_t>(evicted)];
	slot.lod_valid = false;
	retire(slot);
	slot.last_use = frame_ - 0x10000u;
	return true;
}

std::vector<TerrainTileCompositionJob> TerrainTileCompositionCache::sweep(
		const std::vector<TerrainTileCompositionRequest> &visible) {
	// [orig: PolyTrn_RenderFrame @ 0x60EAC0 — the PolyTrn_RenderTile sweep
	// @ 0x60F080..0x60F0A7 ORs each "composed" result; a sweep composing
	// nothing calls terrain_cache_evict_lru @ 0x60F0AD and re-sweeps the list
	// @ 0x60F0C0..0x60F0E3]
	std::vector<TerrainTileCompositionJob> jobs;
	const auto run = [&]() {
		for (const TerrainTileCompositionRequest &entry : visible) {
			std::optional<TerrainTileCompositionDecision> decision = request(entry);
			if (decision.has_value() && decision->job.has_value()) {
				jobs.push_back(*decision->job);
			}
		}
	};
	run();
	if (jobs.empty()) {
		evict_one_tod_stale();
		run();
	}
	return jobs;
}

std::optional<TerrainTilePageBinding> TerrainTileCompositionCache::bind(
		const TerrainTileCompositionRequest &request_value) noexcept {
	// [orig: PolyTrn_BindStageTextures @ 0x604356..0x60439C — the exact
	// identity match binds slot +0x1C as t0 and stamps the last use; no match
	// leaves t0 null]
	for (uint16_t layer = 0; layer < kCapacity; ++layer) {
		Slot &slot = slots_[layer];
		if (!same_identity(slot, request_value) || !slot.ready) continue;
		slot.last_use = frame_;
		return binding(slot, layer);
	}
	return std::nullopt;
}

std::optional<TerrainTilePageBinding> TerrainTileCompositionCache::lookup(
		const TerrainTileResidentPoint &point) noexcept {
	// The consumer floors its world position to whole units; the low nine
	// bits are the sector-local coordinate the record packs, the rest the
	// routed sector origin. [orig: CRenderBatchQueue_FlushBatches floor/ftol
	// @ 0x5DA77B..0x5DA7BD; terrain_tile_cache_lookup split @ 0x604173..
	// 0x6041A2]
	const int32_t point_x = static_cast<int32_t>(std::floor(point.world_x));
	const int32_t point_z = static_cast<int32_t>(std::floor(point.world_z));
	const int32_t sector_x = point_x & ~0x1FF;
	const int32_t sector_z = point_z & ~0x1FF;
	const uint32_t point_key = (static_cast<uint32_t>(point_x & 0x1FF) << 16) |
			static_cast<uint32_t>(point_z & 0x1FF);
	// Granularity 32 << level, level 0..4; each level walks every record in
	// order and takes the first masked match in the point's sector. The flat
	// page's packed coordinate masks to zero, so it can answer at its
	// canonical sector origin.
	// [orig: terrain_tile_cache_lookup @ 0x604140, probe @ 0x6041A4..0x604206,
	// last-use stamp @ 0x60423E..0x604243, the hit's page projection
	// @ 0x604215..0x604292; Terrain_FindSectorPatchRT @ 0x6042B0..0x60430B
	// walks the same records with a ten-bit mask whose quadrant bit agrees
	// with the sector's routing]
	for (int level = 0; level < 5; ++level) {
		const uint32_t mask =
				((~0u << (level + 5)) & 0x1FFu) * 0x10001u;
		for (uint16_t layer = 0; layer < kCapacity; ++layer) {
			Slot &slot = slots_[layer];
			if (!slot.resident || !slot.ready) continue;
			const uint32_t slot_key =
					(static_cast<uint32_t>(slot.page.page_local_x & 0x1FF) << 16) |
					static_cast<uint32_t>(slot.page.page_local_z & 0x1FF);
			if ((slot_key & mask) != (point_key & mask) ||
					slot.page.sector_origin_x != sector_x ||
					slot.page.sector_origin_z != sector_z) {
				continue;
			}
			slot.last_use = frame_;
			return binding(slot, layer);
		}
	}
	return std::nullopt;
}

std::optional<TerrainTilePageBinding> TerrainTileCompositionCache::resident_layer(
		uint16_t layer) const noexcept {
	if (layer >= kCapacity || !slots_[layer].resident || !slots_[layer].ready) {
		return std::nullopt;
	}
	return binding(slots_[layer], layer);
}

bool TerrainTileCompositionCache::can_publish(
		const TerrainTileCompositionJob &job) const noexcept {
	if (job.target.layer >= kCapacity) {
		return false;
	}
	const Slot &slot = slots_[job.target.layer];
	return slot.resident && !slot.ready &&
			slot.generation == job.target.generation &&
			same_page(slot.page, job.target.page) &&
			slot.source_origin_x == job.source_origin_x &&
			slot.source_origin_z == job.source_origin_z;
}

bool TerrainTileCompositionCache::publish(
		const TerrainTileCompositionJob &job) noexcept {
	if (!can_publish(job)) {
		return false;
	}
	slots_[job.target.layer].ready = true;
	return true;
}

bool TerrainTileCompositionCache::invalidate(
		const TerrainTilePageKey &page) noexcept {
	for (Slot &slot : slots_) {
		if (!slot.resident || !same_page(slot.page, page)) {
			continue;
		}
		retire(slot);
		return true;
	}
	return false;
}

// Page extent from the per-LOD world span; the inclusive overlap both
// retail walks share [orig: Terrain_AddScorchRecord @0x605CF7..0x605D5F;
// CVertexBuffer_RemoveFromList @0x605C21..0x605C7F].
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
	// Both walks retire the coordinate and zero the last use, keeping the
	// level: the page recomposes the next time it is visible.
	// [orig: Terrain_AddScorchRecord @0x605D49..0x605D4F;
	// CVertexBuffer_RemoveFromList @0x605C72..0x605C78]
	std::size_t invalidated = 0;
	for (Slot &slot : slots_) {
		if (!slot.resident || !page_overlaps_q16(slot.page,
				minimum_x_q16, minimum_z_q16,
				maximum_x_q16, maximum_z_q16)) {
			continue;
		}
		retire(slot);
		slot.last_use = 0;
		++invalidated;
	}
	return invalidated;
}

void TerrainTileCompositionCache::invalidate_all() noexcept {
	// [orig: lod = coordinate = -1, last use = 0 @ 0x605FC0..0x605FC5]
	for (Slot &slot : slots_) {
		slot.lod_valid = false;
		retire(slot);
		slot.last_use = 0;
	}
}

} // namespace opennova
