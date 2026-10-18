#pragma once

// Terrain surface-type (charmap) sampling at a fixed-point mission position.
//
// The faithful port of the runtime charmap sampler [orig:
// Terrain_GetSurfaceTypeAtPosition @ 0x606510]: mission x/y -> 512-unit sector
// cell -> the 16x16 sector grid picks the charmap quadrant (the same 2x2
// 1024-atlas routing as coords.h; slot bit0 = +512 fine Z, bit1 = +512 fine X)
// -> one point sample of the palette-indexed raster. Off-grid cells clamp to
// the edge cell on an axis the .trn does not wrap, and wrap the grid's 16 on
// one it does (the cell masks, D-TERRAIN-16); an UNMAPPED cell returns surface
// 7, the off-island ocean default. After the charmap sample, the placed-tile override pass walks the
// mission .til array in order and the FIRST tile whose inclusive 16x16-unit
// square contains the position replaces the class with the tileset .TSD
// table's entry for its tile index — even a 0 (TSD_NULL) entry: retail never
// falls back to the charmap under a tile, and shipped JO carries no .TSD, so
// retail placed tiles all read TSD_NULL (D-SND-15, FIXED)
// [orig: the walk @ 0x6065ca-0x606601, the table read @ 0x60660c].
//
// Consumers: infantry footsteps (surface 3 = snow picks the SS*FootSnow
// slots); the ammo impact table reuses the same sampler with a +4 shift.
//
// Godot-agnostic: raw pointer + scalars; the embedder (Simulation) points this
// at TerrainData's decoded charmap and the TRN sector grid.

#include <cstdint>

#include <runtime/terrain_query/atlas_routing.h>

namespace opennova::terrain {

// One placed-tile override entry: the mission .til overlay record reduced to
// the three fields the surface walk reads [orig: the 12-B g_TerrainTileArray
// rows @ 0x319f7a4 — +0 x_fixed, +4 z_fixed, +8 the tile palette index byte].
// z_fixed is the north edge, in mission y, of the square the game draws the
// tile over (and keeps foliage off); this walk alone reads it negated, so it
// tests that square's mirror across y = 0 (docs/tiles/til-re.md D-TIL-9).
struct SurfaceTileEntry {
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
	uint8_t tile_index = 0;
};

struct SurfaceTypeMap {
	// Palette-indexed charmap raster, row-major, power-of-two square up to
	// 1024 (JO ships 256x256). The fine coordinate is >>'d down from the
	// 1024-unit atlas domain [orig: the (10 - shift) sample @ 0x6065c6].
	const uint8_t *data = nullptr;
	int32_t width = 0;
	int32_t height = 0;
	// 16x16 sector grid, row-major ints (0 = empty, 1..4 = quadrant), plus the
	// grid origin in 512-unit sector coordinates [orig: g_TerrainSectorGrid
	// @ 0x319fc10, g_TerrainSectorOriginY/X @ 0x319b2e0/0x319b2e4].
	const int *sector_grid = nullptr;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	// The .trn's polytrn_wrapx / polytrn_wrapy: an axis that wraps takes the
	// cell's low four bits as they are, one that does not clamps an off-grid
	// cell to the edge [orig: Terrain_CellOOBMaskX/Y, 0 under a wrap and -16
	// otherwise, set by PolyTrn_LoadTerrainConfig @ 0x60E4B1..0x60E4CB].
	bool wrap_x = false;
	bool wrap_z = false;

	TerrainAtlasRouting routing() const { return TerrainAtlasRouting{sector_grid, origin_x, origin_y, wrap_x, wrap_z}; }
	// The placed-tile override (D-SND-15): the mission .til array plus the
	// tileset's .TSD-fed 256-entry tile-index -> surface table. tiles == null
	// or tile_count == 0 skips the pass; tile_surface == null models retail's
	// memset-0 table (no .TSD on any shipped install -> every tile TSD_NULL)
	// [orig: g_TerrainTileArray @ 0x319f7a4 + byte_319F7D8 @ 0x319f7d8].
	const SurfaceTileEntry *tiles = nullptr;
	int32_t tile_count = 0;
	const uint8_t *tile_surface = nullptr; // 256 entries
};

// The rows (and columns) a map `width` wide is sampled over, the char map's and the foliage
// map's lookups alike: each reads (c & 1023) >> surface_sample_shift on both axes, so a map up to
// 1024 wide is read over its width's power of two below it (a map shorter than that is read past
// its last row), and one wider reads its first texel alone, the negative shift taken modulo 32
// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510; Foliage_SampleFoliageMapFlat
// @ 0x6066D0, the foliage map's log2 @ 0x605B4B..0x605B5B; Foliage_SampleFoliageMapMask
// @ 0x606620].
inline int32_t surface_sample_extent(int32_t width) {
	return (1023 >> detail::surface_sample_shift(width)) + 1;
}

// Surface type at a fixed-point (16.16) mission-frame position. Matches the
// original including its defaults: 1 with no charmap loaded, 7 (ocean) on an
// unmapped sector cell. [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]
inline int32_t surface_type_at_fixed(const SurfaceTypeMap &m, int32_t x_fixed, int32_t y_fixed) {
	if (m.data == nullptr || m.width <= 0 || m.height <= 0) return 1;
	if (m.sector_grid == nullptr) return 7;
	// The routing into the atlas (atlas_routing.h); an empty cell reads 7.
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(m.routing(), x_fixed, y_fixed);
	if (!texel.mapped) return 7;
	const int32_t shift = detail::surface_sample_shift(m.width);
	const int32_t sx = texel.x >> shift;
	const int32_t sz = texel.z >> shift;
	if (sx < 0 || sx >= m.width || sz < 0 || sz >= m.height) return 1;
	const int32_t sample = m.data[m.width * sz + sx];
	// Placed-tile override: the first containing tile (inclusive 16-unit AABB
	// from -z_fixed north: the drawn square's mirror across y = 0, as the
	// original tests it, its callers passing the world y as it stands; D-TIL-9)
	// wins, replacing the charmap class with the tileset table's entry — 0
	// (TSD_NULL) included [orig: walk @ 0x6065ca-0x606601, read @ 0x60660c].
	// The retail early returns above (no charmap -> 1, unmapped cell -> 7) skip
	// this pass exactly like the original.
	for (int32_t i = 0; m.tiles != nullptr && i < m.tile_count; ++i) {
		const SurfaceTileEntry &e = m.tiles[i];
		if (e.x_fixed <= x_fixed && x_fixed <= e.x_fixed + 0x100000 &&
		    -e.z_fixed <= y_fixed && y_fixed <= 0x100000 - e.z_fixed)
			return m.tile_surface != nullptr ? m.tile_surface[e.tile_index] : 0;
	}
	return sample;
}

} // namespace opennova::terrain
