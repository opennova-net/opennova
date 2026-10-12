#pragma once

// The 1024-unit atlas a terrain's maps lie over, and how a mission position routes into it.
//
// A terrain's heights (the .cpt's 1024 x 1024 depth atlas), its char map and its foliage map are each
// laid over one 1024-unit atlas of four 512-unit quadrants; the .trn's 16 x 16 sector grid names, for
// every 512-unit sector of the mission, the quadrant drawn there (0 empty, 1..4) [orig:
// g_TerrainSectorGrid @ 0x319FC10]. The game's lookups route a position the same way: the sector cell
// (x >> 25, -y >> 25) less the grid's origin, clamped to the edge cell on an axis the .trn does not wrap
// and taken by its low four bits on one it does (the cell masks, D-TERRAIN-16), the grid's slot, then
// the position's offset in its sector plus the slot's quadrant (bit 0 +512 on the atlas's z, bit 1 +512
// on its x) [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606530..0x60659D; Foliage_SampleFoliageMapMask
// @ 0x606620; PolyTrn_LoadTerrainConfig @ 0x60E4B1..0x60E4CB, the masks]. A map narrower than 1024 is
// sampled at the atlas texel shifted down by its sample shift (surface_type_map.h).
//
// terrain_atlas_texel_at is that forward routing, lifted out of the char map's and the foliage map's
// samplers so both, and an editor brush, take one; terrain_atlas_world_rects is its inverse over the
// grid's own cells (where a change to the atlas shows). Godot-agnostic: raw pointer + scalars.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::terrain {

// The routing every atlas lookup shares: the 16 x 16 sector grid (row-major ints, 0 empty, 1..4 the
// quadrant), its origin in 512-unit sectors, the .trn's polytrn_wrapx / polytrn_wrapy.
struct TerrainAtlasRouting {
	const int *sector_grid = nullptr;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	bool wrap_x = false;
	bool wrap_z = false;
};

inline TerrainAtlasRouting terrain_atlas_routing(const TerrainHeightField &field) {
	return TerrainAtlasRouting{field.layout.sector_grid, field.layout.origin_x, field.layout.origin_y,
	                           field.wrap_x, field.wrap_z};
}

namespace detail {
inline int32_t surface_clamp_grid(int32_t v, bool wrap) {
	// Off-grid sector clamp on an axis that does not wrap (its mask -16):
	// negative -> cell 0, past-the-end -> cell 15 [orig: the ~(v >> 31) byte
	// trick behind the OOB mask test @ 0x606547]; under a wrap the mask is 0
	// and the cell's low four bits are taken as they are.
	if (!wrap && (v & ~0xF) != 0) return v < 0 ? 0 : 15;
	return v;
}
// The width's log2 as the loader counts it (halved while above 1: the floor)
// [orig: sub_605A10 @ 0x605A82..0x605AA0; Foliage_LoadFoliageMapPCX @
// 0x605B44..0x605B60].
inline int32_t surface_shift(int32_t width) {
	int32_t s = 0;
	for (uint32_t w = static_cast<uint32_t>(width); w > 1; w >>= 1) ++s;
	return s;
}
// The sample's shift, 10 less that log2: past 1024 it is negative, which the
// processor's shift takes modulo 32 (a map that wide reads its first texel
// alone).
inline int32_t surface_sample_shift(int32_t width) {
	return (10 - surface_shift(width)) & 31;
}
} // namespace detail

// Where a position lands: the grid cell it routes to (0..15 each), the cell's slot (its grid value less
// one), and the atlas texel (x, z), each in [0, 1024). `mapped` is false with no grid and on an empty
// cell, where the char map reads 7 and the foliage map 0.
struct TerrainAtlasTexel {
	bool mapped = false;
	int32_t cell_col = 0;
	int32_t cell_row = 0;
	int32_t slot = 0;
	int32_t x = 0;
	int32_t z = 0;
};

// The atlas texel under a fixed-point (16.16) mission position [orig: Terrain_GetSurfaceTypeAtPosition
// @ 0x606530..0x60659D].
inline TerrainAtlasTexel terrain_atlas_texel_at(const TerrainAtlasRouting &r, int32_t x_fixed, int32_t y_fixed) {
	TerrainAtlasTexel t;
	if (r.sector_grid == nullptr) return t;
	const int32_t col = detail::surface_clamp_grid((x_fixed >> 25) - r.origin_x, r.wrap_x);
	const int32_t row = detail::surface_clamp_grid(((-y_fixed) >> 25) - r.origin_y, r.wrap_z);
	t.cell_col = col & 0xF;
	t.cell_row = row & 0xF;
	t.slot = r.sector_grid[16 * t.cell_row + t.cell_col] - 1;
	if (t.slot < 0) return t;
	t.x = (x_fixed >> 16) & 0x1FF;
	t.z = ((-y_fixed) >> 16) & 0x1FF;
	if ((t.slot & 1) != 0) t.z += 512;
	if ((t.slot & 2) != 0) t.x += 512;
	t.mapped = true;
	return t;
}

// A rectangle of the terrain's plane in whole units, half-open: x east, z the mission's -y (the plane
// the sector grid's rows run down, coords.h's).
struct TerrainWorldRect {
	int32_t x0 = 0;
	int32_t z0 = 0;
	int32_t x1 = 0;
	int32_t z1 = 0;
};

// Every grid cell's part of the plane whose texels route into `atlas` (an atlas-texel rect, half-open,
// clipped to [0, 1024)): one rect for each mapped cell of the 16 x 16 whose quadrant meets it, in grid
// order (row-major), an empty cell giving none. A layout that repeats a quadrant (a tiled terrain) gives
// one rect per repeat. These are the grid's own cells, where the game's sectors origin_x .. origin_x + 15
// lie; past the grid the game repeats them every 16 sectors on an axis that wraps and stretches the edge
// cells on one that does not, which a caller drawing past the grid adds itself.
inline std::vector<TerrainWorldRect> terrain_atlas_world_rects(const TerrainAtlasRouting &r, const CoordsRect &atlas) {
	std::vector<TerrainWorldRect> out;
	if (r.sector_grid == nullptr) return out;
	const int32_t ax0 = std::max(atlas.x, 0);
	const int32_t az0 = std::max(atlas.z, 0);
	const int32_t ax1 = std::min(atlas.x + atlas.w, COORDS_ATLAS_SIZE);
	const int32_t az1 = std::min(atlas.z + atlas.h, COORDS_ATLAS_SIZE);
	if (ax0 >= ax1 || az0 >= az1) return out;
	for (int32_t row = 0; row < COORDS_SECTOR_GRID_DIM; ++row) {
		for (int32_t col = 0; col < COORDS_SECTOR_GRID_DIM; ++col) {
			// The forward routing's quadrant for this cell (terrain_atlas_texel_at).
			const int32_t slot = r.sector_grid[COORDS_SECTOR_GRID_DIM * row + col] - 1;
			if (slot < 0) continue;
			const int32_t qx = (slot & 2) != 0 ? COORDS_SECTOR_SIZE : 0;
			const int32_t qz = (slot & 1) != 0 ? COORDS_SECTOR_SIZE : 0;
			const int32_t ix0 = std::max(ax0, qx), ix1 = std::min(ax1, qx + COORDS_SECTOR_SIZE);
			const int32_t iz0 = std::max(az0, qz), iz1 = std::min(az1, qz + COORDS_SECTOR_SIZE);
			if (ix0 >= ix1 || iz0 >= iz1) continue;
			const int32_t sx = (r.origin_x + col) * COORDS_SECTOR_SIZE;
			const int32_t sz = (r.origin_y + row) * COORDS_SECTOR_SIZE;
			out.push_back(TerrainWorldRect{sx + ix0 - qx, sz + iz0 - qz, sx + ix1 - qx, sz + iz1 - qz});
		}
	}
	return out;
}

// The atlas-texel rect a rect of a map `map_width` wide covers: each map texel the 2^shift atlas texels the
// samplers read it for (detail::surface_sample_shift).
inline CoordsRect terrain_map_atlas_rect(const CoordsRect &map_rect, int32_t map_width) {
	const int32_t shift = detail::surface_sample_shift(map_width);
	return CoordsRect{map_rect.x << shift, map_rect.z << shift, map_rect.w << shift, map_rect.h << shift};
}

} // namespace opennova::terrain
