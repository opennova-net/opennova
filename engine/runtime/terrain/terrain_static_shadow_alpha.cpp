#include <terrain/terrain_static_shadow_alpha.h>

// [orig: Terrain_CollectAndRenderTileModels temp-blue result
// @0x60D5BF..0x60DA4F; PSDepthAlpha A-only page composite
// @0x60E0C6..0x60E19D]

#include <cstddef>
#include <limits>

namespace opennova::terrain {
namespace {

constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr uint8_t kShadowStampDomain[] = {
		't', 'e', 'r', 'r', 'a', 'i', 'n', '-',
		's', 't', 'a', 't', 'i', 'c', '-', 's',
		'h', 'a', 'd', 'o', 'w', '-', 'a', 'l',
		'p', 'h', 'a', '-', 'v', '1'};

bool same_page(const TerrainTilePageKey &left,
		const TerrainTilePageKey &right) noexcept {
	return left.sector_origin_x == right.sector_origin_x &&
			left.sector_origin_z == right.sector_origin_z &&
			left.page_local_x == right.page_local_x &&
			left.page_local_z == right.page_local_z &&
			left.page_lod_level == right.page_lod_level;
}

bool pixel_count(uint32_t width, uint32_t height,
		std::size_t &result) noexcept {
	if (width == 0 || height == 0 ||
			width > std::numeric_limits<std::size_t>::max() / height) {
		return false;
	}
	result = static_cast<std::size_t>(width) * height;
	return true;
}

uint64_t mix(uint64_t hash, uint8_t value) noexcept {
	return (hash ^ value) * kFnvPrime;
}

} // namespace

bool TerrainStaticShadowAlphaPage::is_valid() const noexcept {
	std::size_t count = 0;
	return TerrainTileCompositionCache::page_world_span(page.page_lod_level) > 0 &&
			pixel_count(width, height, count) && alpha.size() == count;
}

TerrainStaticShadowAlphaPage begin_terrain_static_shadow_alpha_page(
		const TerrainTileCompositionJob &job,
		const Rgba8Image &composed_page,
		TerrainTileContentStamp raster_content) {
	TerrainStaticShadowAlphaPage result;
	const int dimension = job.layout.texture_dimension;
	if (dimension <= 0 || !composed_page.is_valid() ||
			composed_page.width != static_cast<uint32_t>(dimension) ||
			composed_page.height != static_cast<uint32_t>(dimension) ||
			TerrainTileCompositionCache::page_world_span(
					job.target.page.page_lod_level) <= 0) {
		return result;
	}
	result.page = job.target.page;
	result.content = raster_content;
	result.width = composed_page.width;
	result.height = composed_page.height;
	const std::size_t count = static_cast<std::size_t>(result.width) * result.height;
	result.alpha.resize(count);
	for (std::size_t pixel = 0; pixel < count; ++pixel) {
		result.alpha[pixel] = composed_page.pixels[pixel * 4u + 3u];
	}
	return result;
}

bool apply_terrain_static_shadow_alpha_page(
		const TerrainTileCompositionJob &job,
		const TerrainStaticShadowAlphaPage &shadow_page,
		Rgba8Image &composed_page) noexcept {
	const int dimension = job.layout.texture_dimension;
	if (dimension <= 0 || !shadow_page.is_valid() ||
			!same_page(job.target.page, shadow_page.page) ||
			!composed_page.is_valid() ||
			composed_page.width != shadow_page.width ||
			composed_page.height != shadow_page.height ||
			composed_page.width != static_cast<uint32_t>(dimension) ||
			composed_page.height != static_cast<uint32_t>(dimension)) {
		return false;
	}
	for (std::size_t pixel = 0; pixel < shadow_page.alpha.size(); ++pixel) {
		composed_page.pixels[pixel * 4u + 3u] = shadow_page.alpha[pixel];
	}
	return true;
}

TerrainTileContentStamp mix_terrain_static_shadow_content_stamp(
		TerrainTileContentStamp base,
		TerrainTileContentStamp shadow) noexcept {
	uint64_t hash = base.value;
	for (uint8_t byte : kShadowStampDomain) hash = mix(hash, byte);
	for (int shift = 0; shift < 64; shift += 8) {
		hash = mix(hash, static_cast<uint8_t>(shadow.value >> shift));
	}
	return TerrainTileContentStamp{hash};
}

} // namespace opennova::terrain
