#pragma once

// A painted region of a terrain's colour map carried into the quadrant textures the tile cache's pages sample, and
// the pages that sample it (ADR 0046 S25, the editor's paint strokes): the levels made again over the texels the
// change reaches alone, the same bytes a build of the painted map holds, and the test that retires a page by what it
// samples, wherever the frame drew it. Nothing here is a port of its own: the bytes are the quadrant build's
// (terrain_tile_composer.h, build_terrain_tile_quadrant_source) and the sampling is compose_terrain_tile_page's.

#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain_query/coords.h>

#include <array>
#include <cstdint>
#include <vector>

namespace opennova::terrain {

// What a painted region made again of a quadrant source: for each quadrant, each level's texels (half-open, in that
// level's texels; empty for none).
struct TerrainTileQuadrantChange {
	struct Rect {
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
	};
	std::array<std::vector<Rect>, 4> levels;
	unsigned quadrants = 0; // a bit each: the quadrants made again
};

// The levels of `source` a changed rect of `atlas` (the painted map, atlas texels x, y, w, h) reaches, made again from
// it over those texels alone: the base texels, each DXT level's 4x4 blocks they fall in, and each later level's texels
// whose box filter reads a changed one, encoded again block by block where the level is compressed (DXT blocks are
// encoded alone, the box filter reads a 2x2 footprint). Each changed quadrant's chain is a new one (the others stay
// shared). The bytes equal what build_terrain_tile_quadrant_source makes of `atlas`. A chain the device halved keeps
// no blocks and is made again whole. `changed` (optional) says what was made. 0, nothing made, for an atlas of another
// size than the source's or a rect that misses it; else the quadrants made, a bit each.
unsigned update_terrain_tile_quadrants(TerrainTileQuadrantSource &source, const Rgba8Image &atlas, int x, int y, int w,
		int h, TerrainTileQuadrantChange *changed = nullptr);
// The same over a map held as `width` x `height` RGBA8 texels at `rgba` (a device's live map: only the texels the
// change reaches are read).
unsigned update_terrain_tile_quadrants(TerrainTileQuadrantSource &source, const uint8_t *rgba, uint32_t width,
		uint32_t height, int x, int y, int w, int h, TerrainTileQuadrantChange *changed = nullptr);
// Several changed rects of `atlas` at once (an editor's sculpt: the boxes of the normals a region made again,
// refresh_heightfield_normal_map's), each made again as the single rect's form makes it, its change kept apart in
// `changed` (one a rect, so a page tests each: terrain_tile_page_samples_any; two far apart in one quadrant never
// widen into the span between them). The quadrants made, a bit each; 0, nothing made, where any rect is refused.
unsigned update_terrain_tile_quadrants(TerrainTileQuadrantSource &source, const Rgba8Image &atlas,
		const std::vector<CoordsRect> &rects, std::vector<TerrainTileQuadrantChange> &changed);

// Whether a page samples a texel `change` made again: the quadrant its source origin routes to (bit 9 of each half),
// the level its footprint picks (page_world_span(page_lod_level) units over `dimension` pixels, as
// compose_terrain_tile_page picks it over `source`'s levels), and the texels its bilinear taps reach over the
// 512-unit quadrant's span it draws; the flat page (level 0 of the page key) reads texel (0, 0) of its quadrant.
// [orig: PolyTrn_RenderTile UV build @ 0x60DB62..0x60DC16, flat page @ 0x60DC08..0x60DC16]
bool terrain_tile_page_samples(const TerrainTileQuadrantChange &change, const TerrainTileQuadrantSource &source,
		uint8_t page_lod_level, int32_t source_origin_x, int32_t source_origin_z,
		int dimension = TerrainTileCompositionCache::kDimension);
// Whether a page samples a texel any of `changes` made again (terrain_tile_page_samples over each).
bool terrain_tile_page_samples_any(const std::vector<TerrainTileQuadrantChange> &changes,
		const TerrainTileQuadrantSource &source, uint8_t page_lod_level, int32_t source_origin_x,
		int32_t source_origin_z, int dimension = TerrainTileCompositionCache::kDimension);

} // namespace opennova::terrain
