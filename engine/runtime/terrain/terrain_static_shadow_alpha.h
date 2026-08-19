#pragma once

// Typed handoff between the static-caster raster and the composed terrain
// page cache. Retail initializes a temporary shadow RT from the page's DOT3
// light term, overwrites admitted PROJSHAD coverage in that RT, then copies
// temp blue into destination alpha while preserving destination RGB. This
// carrier is therefore the final destination-resolution light alpha, never
// an opacity or a color multiplier.
// [orig: Terrain_CollectAndRenderTileModels @0x60D5BF..0x60DA4F;
// PolyTrn_RenderTile @0x60E0C6..0x60E19D]

#include <terrain/terrain_tile_composer.h>

#include <cstdint>
#include <vector>

namespace opennova::terrain {

struct TerrainStaticShadowAlphaPage {
	TerrainTilePageKey page{};
	TerrainTileContentStamp content{};
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> alpha;

	bool is_valid() const noexcept;
};

// Starts a raster page by copying the composed page's existing DOT3 alpha.
// The raster then changes only admitted coverage (opaque PROJSHAD writes zero;
// 2x resolve edges may be fractional) before apply commits the final bytes.
TerrainStaticShadowAlphaPage begin_terrain_static_shadow_alpha_page(
		const TerrainTileCompositionJob &job,
		const Rgba8Image &composed_page,
		TerrainTileContentStamp raster_content);

// Atomically replaces only destination A. Page identity, dimensions, and
// storage are validated before the first byte is changed.
bool apply_terrain_static_shadow_alpha_page(
		const TerrainTileCompositionJob &job,
		const TerrainStaticShadowAlphaPage &shadow_page,
		Rgba8Image &composed_page) noexcept;

// Domain-separated cache-content mix. A device calls this before cache lookup
// whenever its page plan contains raster draws, so caster/light/transform
// changes cannot reuse a page uploaded under a different alpha result.
TerrainTileContentStamp mix_terrain_static_shadow_content_stamp(
		TerrainTileContentStamp base,
		TerrainTileContentStamp shadow) noexcept;

} // namespace opennova::terrain
