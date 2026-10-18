#pragma once

// The 1024-unit atlas a terrain's maps lie over, and how a mission position routes into it.
//
// A terrain's heights (the .cpt's 1024 x 1024 depth atlas), its char map and its foliage map are each
// laid over one 1024-unit atlas of four 512-unit quadrants; the .trn's 16 x 16 sector grid names, for
// every 512-unit sector of the mission, the quadrant drawn there (0 empty, 1..4) [orig:
// g_TerrainSectorGrid @ 0x319FC10]. Every lookup of the game routes a position the same way: the sector
// cell (x >> 25, -y >> 25) less the grid's origin, clamped to the edge cell on an axis the .trn does not
// wrap and taken by its low four bits on one it does (the cell masks, D-TERRAIN-16), the grid's value less
// one, then the position's offset in its sector plus that value's quadrant (bit 0 +512 on the atlas's z,
// bit 1 +512 on its x) [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606530..0x60659D (the char map);
// Foliage_SampleFoliageMapMask @ 0x606620 (the foliage map); Terrain_GetHeightAtPosition @
// 0x606720..0x606796 (the nearest height); Terrain_SampleHeightBilinear @ 0x6067C4..0x6068C4 (the ground);
// Terrain_GetHeightGradient @ 0x606330..0x606385; PolyTrn_LoadTerrainConfig @ 0x60E4B1..0x60E4CB, the
// masks]. A map narrower than 1024 is sampled at the atlas texel shifted down by its sample shift
// (surface_type_map.h); what each lookup then reads around the texel (one texel, a bilinear pair, a
// centred pair) is the inverse's business (terrain_atlas_rects.h).
//
// terrain_atlas_texel_at is that routing, the one every sampler in terrain_query calls: the char map's,
// the foliage map's and the height gradient's (height_field.cpp), and an editor's brush. Godot-agnostic:
// raw pointer + scalars, header-light (it is in the world's surface lookups).

#include <cstdint>

#include <base/io/bam.h>

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
// [orig: Terrain_LoadCharmapPCX @ 0x605A82..0x605AA0; Foliage_LoadFoliageMapPCX @
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
// cell (a grid value 0 or below), where the char map reads 7, the foliage map 0 and the heights 0.
struct TerrainAtlasTexel {
	bool mapped = false;
	int32_t cell_col = 0;
	int32_t cell_row = 0;
	int32_t slot = 0;
	int32_t x = 0;
	int32_t z = 0;
};

// The atlas texel under a fixed-point (16.16) mission position, the game's NEG/SAR on the 32-bit words
// as they are (INT32_MIN included: io::bam_sub / io::bam_sar) [orig: Terrain_GetSurfaceTypeAtPosition @
// 0x606530..0x60659D; Terrain_SampleHeightBilinear @ 0x6067BD..0x6068C4].
inline TerrainAtlasTexel terrain_atlas_texel_at(const TerrainAtlasRouting &r, int32_t x_fixed, int32_t y_fixed) {
	TerrainAtlasTexel t;
	if (r.sector_grid == nullptr) return t;
	const int32_t atlas_y = io::bam_sub(0, y_fixed);
	const int32_t col = detail::surface_clamp_grid(io::bam_sub(io::bam_sar(x_fixed, 25), r.origin_x), r.wrap_x);
	const int32_t row = detail::surface_clamp_grid(io::bam_sub(io::bam_sar(atlas_y, 25), r.origin_y), r.wrap_z);
	t.cell_col = col & 0xF;
	t.cell_row = row & 0xF;
	t.slot = io::bam_sub(r.sector_grid[16 * t.cell_row + t.cell_col], 1);
	if (t.slot < 0) return t;
	t.x = io::bam_sar(x_fixed, 16) & 0x1FF;
	t.z = io::bam_sar(atlas_y, 16) & 0x1FF;
	if ((t.slot & 1) != 0) t.z += 512;
	if ((t.slot & 2) != 0) t.x += 512;
	t.mapped = true;
	return t;
}

} // namespace opennova::terrain
