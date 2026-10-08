#include <runtime/terrain/terrain_static_shadow.h>
#include <base/io/hash.h>

// [orig: Terrain_CollectAndRenderTileModels @0x60D250; static admission
// @0x60D421..0x60D450; sphere/tile reject @0x60D465..0x60D54F;
// selected LOD/all-ROBJ submission @0x60D881..0x60D971]

#include <runtime/mission/placement_traits.h>
#include <base/io/fixed.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace opennova::terrain {
namespace {

// Retail clamps the fixed vertical light to 0x4000 (= 0.25 in 16.16) before
// deriving the tile extension; the raster path clamps its float twin the
// same way [orig: fixed clamp @ 0x60d315..0x60d32c, float clamp
// @ 0x60d33f..0x60d341].
constexpr int32_t kMinimumVerticalLightFixed = 0x4000;
// One tile collects at most 0x400 casters, pools 2 and 1 together
// [orig: @ 0x60d390, @ 0x60d40e].
constexpr std::size_t kTileCasterCapacity = 0x400;

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

// The tile rectangle in mission 16.16: x runs east from the page's west
// edge, y (mission north = -Godot z) runs from the page's south edge up to
// the negated north edge [orig: tileSize = 0x400 >> lod, x0/x1
// @ 0x60d286..0x60d2b5, y1 = -(row << 16), y0 = y1 - size @ 0x60d299..0x60d2c5].
struct TileRect {
	int64_t x0 = 0;
	int64_t x1 = 0;
	int64_t y0 = 0;
	int64_t y1 = 0;
};

TileRect tile_rect(const TerrainTilePageKey &page, int span) noexcept {
	TileRect rect;
	rect.x0 = (static_cast<int64_t>(page.sector_origin_x) + page.page_local_x) *
			io::kFp16OneInt;
	rect.x1 = rect.x0 + static_cast<int64_t>(span) * io::kFp16OneInt;
	rect.y1 = -(static_cast<int64_t>(page.sector_origin_z) + page.page_local_z) *
			io::kFp16OneInt;
	rect.y0 = rect.y1 - static_cast<int64_t>(span) * io::kFp16OneInt;
	return rect;
}

// The per-axis sun slope t = l * 0.5 / l_vertical in 16.16 over the mission
// fixed light tuple (Godot x, -z, y), its vertical clamped to 0x4000; idiv
// truncates toward zero [orig: @ 0x60d35d..0x60d386].
struct TileLightSlope {
	int32_t x = 0;
	int32_t y = 0;
};

TileLightSlope tile_light_slope(
		const TerrainStaticShadowLightDirection &surface_to_light) noexcept {
	const int32_t lx = io::float_to_fp16_16_round_sat(surface_to_light.x);
	const int32_t ly = io::float_to_fp16_16_round_sat(-surface_to_light.z);
	const int32_t lz = std::max(
			io::float_to_fp16_16_round_sat(surface_to_light.y),
			kMinimumVerticalLightFixed);
	TileLightSlope slope;
	slope.x = static_cast<int32_t>(static_cast<int64_t>(lx) * 0x8000 / lz);
	slope.y = static_cast<int32_t>(static_cast<int64_t>(ly) * 0x8000 / lz);
	return slope;
}

// t * r rounded back to 16.16 (imul, + 0x8000, shrd 16) [orig: @ 0x60d47c..0x60d490].
int64_t tile_extension(int32_t slope, int32_t radius) noexcept {
	const int64_t product = static_cast<int64_t>(slope) * radius + 0x8000;
	return static_cast<int32_t>(product >> 16);
}

// The sphere/tile test: the rectangle grows toward the light by t * r on the
// side the slope points to, and the caster sphere must overlap it on both
// axes (inclusive) [orig: extensions @ 0x60d465..0x60d4fe, compares
// @ 0x60d500..0x60d54f].
bool sphere_reaches_tile(const TerrainStaticShadowCandidate &candidate,
		const TileRect &rect, const TileLightSlope &slope) noexcept {
	const int32_t r = candidate.model_radius_fixed;
	const int64_t pos_x = slope.x > 0 ? tile_extension(slope.x, r) : 0;
	const int64_t pos_y = slope.y > 0 ? tile_extension(slope.y, r) : 0;
	const int64_t neg_x = slope.x < 0 ? tile_extension(slope.x, r) : 0;
	const int64_t neg_y = slope.y < 0 ? tile_extension(slope.y, r) : 0;
	const int64_t x = candidate.position_fixed[0];
	const int64_t y = candidate.position_fixed[1];
	if (x + r < rect.x0 + neg_x) return false;
	if (x - r > rect.x1 + pos_x) return false;
	if (y + r < rect.y0 + neg_y) return false;
	if (y - r > rect.y1 + pos_y) return false;
	return true;
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

TerrainStaticShadowReach terrain_static_shadow_caster_reach(
		const TerrainStaticShadowCandidate &candidate) noexcept {
	// The sphere r on either side, plus the largest tile extension: |t| <= 2
	// (0x20000 in 16.16) once the vertical is clamped to 0x4000, and the
	// extension's rounding adds at most one unit.
	const int64_t radius = std::max<int64_t>(candidate.model_radius_fixed, 0);
	const int64_t reach = 3 * radius + 1;
	// Symmetric, so the Godot z (-y) of either edge is an int32 too.
	const auto clamp = [](int64_t value) {
		return static_cast<int32_t>(std::clamp<int64_t>(value,
				-std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()));
	};
	TerrainStaticShadowReach out;
	out.min_x = clamp(int64_t(candidate.position_fixed[0]) - reach);
	out.max_x = clamp(int64_t(candidate.position_fixed[0]) + reach);
	out.min_y = clamp(int64_t(candidate.position_fixed[1]) - reach);
	out.max_y = clamp(int64_t(candidate.position_fixed[1]) + reach);
	return out;
}

void TerrainStaticShadowCollector::replace(
		std::vector<TerrainStaticShadowCandidate> candidates) {
	candidate_count_ = candidates.size();
	admitted_.clear();
	admitted_.reserve(candidates.size());
	for (TerrainStaticShadowCandidate &candidate : candidates) {
		if (!candidate.active) continue;
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
			!std::isfinite(input.surface_to_light.z)) {
		return job;
	}
	const TileRect rect = tile_rect(input.page, span);
	const TileLightSlope slope = tile_light_slope(input.surface_to_light);

	uint64_t hash = io::kFnv1a64Offset;
	hash = io::fnv1a64_value(hash, input.page.sector_origin_x);
	hash = io::fnv1a64_value(hash, input.page.sector_origin_z);
	hash = io::fnv1a64_value(hash, input.page.page_local_x);
	hash = io::fnv1a64_value(hash, input.page.page_local_z);
	hash = io::fnv1a64_value(hash, input.page.page_lod_level);
	for (const uint8_t light_byte : input.light_epoch) {
		hash = io::fnv1a64_value(hash, light_byte);
	}

	std::size_t collected = 0;
	for (const TerrainStaticShadowCandidate &candidate : admitted_) {
		if (collected >= kTileCasterCapacity) break;
		if (!sphere_reaches_tile(candidate, rect, slope)) continue;
		++collected;
		// The position and sphere decide which pages a caster reaches, so they
		// must invalidate a resident page even if a binding has not yet
		// advanced its optional transform revision.
		hash = io::fnv1a64_value(hash, candidate.position_fixed[0]);
		hash = io::fnv1a64_value(hash, candidate.position_fixed[1]);
		hash = io::fnv1a64_value(hash, candidate.model_radius_fixed);
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
