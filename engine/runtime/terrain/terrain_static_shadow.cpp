#include <runtime/terrain/terrain_static_shadow.h>
#include <base/io/hash.h>

// [orig: Terrain_CollectAndRenderTileModels @0x60D250; static admission
// @0x60D421..0x60D450; projected bounds/page reject @0x60D465..0x60D54F;
// selected LOD/all-ROBJ submission @0x60D881..0x60D971]

#include <runtime/mission/placement_traits.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace opennova::terrain {
namespace {

// Retail clamps the fixed vertical light to 0x4000 (= 0.25 in 16.16) before
// deriving the projection offsets; the raster path clamps its float twin the
// same way [orig: fixed clamp @ 0x60d325..0x60d32c, float clamp
// @ 0x60d33f..0x60d341].
constexpr float kMinimumVerticalProjection = 0.25f;

// Buildings (BMS pool 2) collect before items (pool 1) [orig: pool indirection
// selects pool 2 then pool 1 @ 0x60d3a2/0x60d3b1 in
// Terrain_CollectAndRenderTileModels].
int pool_order(const TerrainStaticShadowCandidate &candidate) noexcept {
	if (candidate.entity_kind == mission::kEntityKindBuilding) return 0;
	if (candidate.entity_kind == mission::kEntityKindItem) return 1;
	return 2;
}

bool candidate_less(const TerrainStaticShadowCandidate &left,
		const TerrainStaticShadowCandidate &right) noexcept {
	const int left_pool = pool_order(left);
	const int right_pool = pool_order(right);
	if (left_pool != right_pool) return left_pool < right_pool;
	if (left.collector_order != right.collector_order)
		return left.collector_order < right.collector_order;
	if (left.bms_id != right.bms_id) return left.bms_id < right.bms_id;
	if (left.caster_key != right.caster_key)
		return left.caster_key < right.caster_key;
	return left.geometry.geometry_key < right.geometry.geometry_key;
}

struct Footprint {
	float min_x = 0.0f;
	float min_z = 0.0f;
	float max_x = 0.0f;
	float max_z = 0.0f;
};

Footprint projected_footprint(const TerrainStaticShadowBounds &bounds,
		const TerrainStaticShadowPageInput &input) noexcept {
	// The shadow travels opposite surface->light. Sweeping both vertical AABB
	// endpoints onto the receiver plane yields a conservative planar bound;
	// retaining the unswept bound also fails open when the receiver rises into
	// the caster. Retail clamps the fixed vertical component to 0x4000 before
	// deriving the same horizontal projection extent.
	const float vertical = std::max(input.surface_to_light.y,
			kMinimumVerticalProjection);
	const float min_drop = std::max(0.0f,
			bounds.min_y - input.receiver_height);
	const float max_drop = std::max(0.0f,
			bounds.max_y - input.receiver_height);
	const float shift_x0 = -input.surface_to_light.x * min_drop / vertical;
	const float shift_x1 = -input.surface_to_light.x * max_drop / vertical;
	const float shift_z0 = -input.surface_to_light.z * min_drop / vertical;
	const float shift_z1 = -input.surface_to_light.z * max_drop / vertical;
	return Footprint{
			std::min({bounds.min_x, bounds.min_x + shift_x0,
					bounds.min_x + shift_x1}),
			std::min({bounds.min_z, bounds.min_z + shift_z0,
					bounds.min_z + shift_z1}),
			std::max({bounds.max_x, bounds.max_x + shift_x0,
					bounds.max_x + shift_x1}),
			std::max({bounds.max_z, bounds.max_z + shift_z0,
					bounds.max_z + shift_z1})};
}

bool intersects_page(const Footprint &footprint,
		const TerrainTilePageKey &page, int span) noexcept {
	const float page_min_x = static_cast<float>(
			page.sector_origin_x + page.page_local_x);
	const float page_min_z = static_cast<float>(
			page.sector_origin_z + page.page_local_z);
	const float page_max_x = page_min_x + static_cast<float>(span);
	const float page_max_z = page_min_z + static_cast<float>(span);
	return footprint.max_x > page_min_x && footprint.min_x < page_max_x &&
			footprint.max_z > page_min_z && footprint.min_z < page_max_z;
}

// Retail defaults to the first shadow mesh and takes the second only when the
// page level is fine enough AND the model authors both slots [orig:
// modelData[8] default; `lodLevel <= 3 && modelData[4] >= 2` selects
// modelData[9] @ 0x60d889..0x60d894].
uint8_t selected_lod(const TerrainStaticShadowCandidate &candidate,
		uint8_t page_lod_level) noexcept {
	const bool prefer_second = page_lod_level <= 3;
	if (prefer_second && candidate.geometry.render_object_counts[1] > 0)
		return 1;
	if (candidate.geometry.render_object_counts[0] > 0) return 0;
	return candidate.geometry.render_object_counts[1] > 0 ? 1 : 0;
}

} // namespace

bool TerrainStaticShadowBounds::valid() const noexcept {
	return std::isfinite(min_x) && std::isfinite(min_y) &&
			std::isfinite(min_z) && std::isfinite(max_x) &&
			std::isfinite(max_y) && std::isfinite(max_z) &&
			min_x <= max_x && min_y <= max_y && min_z <= max_z;
}

// The STRP index-window convention the runtime decode rides: indices are
// either relative to the ROBJ's vertex window or absolute into the shared
// buffer, disambiguated by range [orig: STRP runtime decode — basic loop
// @ 0x474CAF, skinned @ 0x474B60; docs/threedi/3di-gp-format-re.md STRP/ROBJ].
bool terrain_static_shadow_strip_indices_are_valid(
		const uint16_t *indices, uint32_t index_buffer_count,
		int32_t index_offset, uint16_t index_count,
		uint32_t vertex_buffer_count, int32_t vertex_offset,
		int32_t vertex_count) noexcept {
	if (indices == nullptr || index_count == 0 || vertex_count <= 0 ||
			index_offset < 0 || vertex_offset < 0) {
		return false;
	}
	const uint64_t index_end = static_cast<uint64_t>(index_offset) +
			index_count;
	const uint64_t vertex_end = static_cast<uint64_t>(vertex_offset) +
			static_cast<uint32_t>(vertex_count);
	if (index_end > index_buffer_count || vertex_end > vertex_buffer_count) {
		return false;
	}

	const uint16_t *strip_indices = indices + index_offset;
	uint16_t minimum = std::numeric_limits<uint16_t>::max();
	uint16_t maximum = 0;
	for (uint32_t index = 0; index < index_count; ++index) {
		minimum = std::min(minimum, strip_indices[index]);
		maximum = std::max(maximum, strip_indices[index]);
	}
	const uint32_t window_offset = static_cast<uint32_t>(vertex_offset);
	const uint32_t window_count = static_cast<uint32_t>(vertex_count);
	const bool relative = maximum < window_count;
	const bool absolute = minimum >= window_offset &&
			static_cast<uint32_t>(maximum) - window_offset < window_count;
	return relative || absolute;
}

void TerrainStaticShadowCollector::replace(
		std::vector<TerrainStaticShadowCandidate> candidates) {
	candidate_count_ = candidates.size();
	admitted_.clear();
	admitted_.reserve(candidates.size());
	for (TerrainStaticShadowCandidate &candidate : candidates) {
		if (!candidate.active || !candidate.world_bounds.valid()) continue;
		if (!mission::item_casts_static_terrain_shadow(candidate.entity_kind,
				candidate.entity_attrib, candidate.item_attrib,
				candidate.item_attrib2)) {
			continue;
		}
		if (candidate.geometry.render_object_counts[0] == 0 &&
				candidate.geometry.render_object_counts[1] == 0) {
			continue;
		}
		admitted_.push_back(std::move(candidate));
	}
	std::stable_sort(admitted_.begin(), admitted_.end(), candidate_less);
}

TerrainStaticShadowPageJob TerrainStaticShadowCollector::compile(
		const TerrainStaticShadowPageInput &input) const {
	TerrainStaticShadowPageJob job;
	job.page = input.page;
	const int span = TerrainTileCompositionCache::page_world_span(
			input.page.page_lod_level);
	if (span <= 0 || !std::isfinite(input.surface_to_light.x) ||
			!std::isfinite(input.surface_to_light.y) ||
			!std::isfinite(input.surface_to_light.z) ||
			!std::isfinite(input.receiver_height)) {
		return job;
	}

	uint64_t hash = io::kFnv1a64Offset;
	hash = io::fnv1a64_value(hash, input.page.sector_origin_x);
	hash = io::fnv1a64_value(hash, input.page.sector_origin_z);
	hash = io::fnv1a64_value(hash, input.page.page_local_x);
	hash = io::fnv1a64_value(hash, input.page.page_local_z);
	hash = io::fnv1a64_value(hash, input.page.page_lod_level);
	for (const uint8_t light_byte : input.light_epoch) {
		hash = io::fnv1a64_value(hash, light_byte);
	}
	hash = io::fnv1a64_value(hash, input.receiver_height);

	for (const TerrainStaticShadowCandidate &candidate : admitted_) {
		const Footprint footprint = projected_footprint(
				candidate.world_bounds, input);
		if (!intersects_page(footprint, input.page, span)) continue;
		// Bounds participate directly in projection, so they must invalidate a
		// resident page even if an binding has not yet advanced its optional
		// transform revision.
		hash = io::fnv1a64_value(hash, candidate.world_bounds.min_x);
		hash = io::fnv1a64_value(hash, candidate.world_bounds.min_y);
		hash = io::fnv1a64_value(hash, candidate.world_bounds.min_z);
		hash = io::fnv1a64_value(hash, candidate.world_bounds.max_x);
		hash = io::fnv1a64_value(hash, candidate.world_bounds.max_y);
		hash = io::fnv1a64_value(hash, candidate.world_bounds.max_z);
		const uint8_t lod = selected_lod(candidate,
				input.page.page_lod_level);
		const uint16_t render_object_count =
				candidate.geometry.render_object_counts[lod];
		for (uint16_t render_object = 0;
				render_object < render_object_count; ++render_object) {
			TerrainStaticShadowProjectionDraw draw;
			draw.bms_id = candidate.bms_id;
			draw.caster_key = candidate.caster_key;
			draw.collector_order = candidate.collector_order;
			draw.transform_revision = candidate.transform_revision;
			draw.geometry.geometry_key = candidate.geometry.geometry_key;
			draw.geometry.lod_index = lod;
			draw.geometry.render_object_index = render_object;
			job.draws.push_back(draw);

			hash = io::fnv1a64_value(hash, draw.bms_id);
			hash = io::fnv1a64_value(hash, draw.caster_key);
			hash = io::fnv1a64_value(hash, draw.collector_order);
			hash = io::fnv1a64_value(hash, draw.transform_revision);
			hash = io::fnv1a64_value(hash, draw.geometry.geometry_key);
			hash = io::fnv1a64_value(hash, draw.geometry.lod_index);
			hash = io::fnv1a64_value(hash, draw.geometry.render_object_index);
		}
	}
	job.content.value = hash;
	return job;
}

} // namespace opennova::terrain
