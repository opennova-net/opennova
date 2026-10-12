#pragma once

// The terrain foliage map sampled at a fixed-point mission position: which of
// the .trn's four foliage definitions grow there.
//
// The game reads the .trn's polytrn_foliagemap through its 8-bit PCX reader,
// keeps only the indices (each a foliage code, its palette unused) and the
// width's log2, and turns every texel at load into the mask of the definition
// slots whose `match` codes hold it (Foliage_RemapPixelToDefMask: code 0, and a
// slot whose graphic is empty, select nothing) [orig: Foliage_LoadFoliageMapPCX
// @ 0x605AD0, the copy @ 0x605B3A, the log2 @ 0x605B44..0x605B60, the remap
// @ 0x605B73..0x605B8A]. Its sampler routes a position through the sector grid
// as the char map's does (surface_type_map.h), the same cell masks included,
// and returns 0 with no map loaded or on an empty sector [orig:
// Foliage_SampleFoliageMapMask @ 0x606620]; the detail tier's flat lookup at a
// cell's atlas position lands on the same texel (foliage-re.md, D-FOLIAGE-12).
// What a definition then places there is the foliage runtime's (its 16-unit
// cells, the placed-tile blocker unless `forceon`), not this sampler's.
//
// Godot-agnostic: raw pointer + scalars, as SurfaceTypeMap.

#include <cstdint>

#include <runtime/terrain_query/surface_type_map.h>

namespace opennova::terrain {

struct FoliageMaskMap {
	// The remapped raster (each texel a definition-slot mask), row-major,
	// `width` the rows' length and the power of two the sample shifts by.
	const uint8_t *data = nullptr;
	int32_t width = 0;
	int32_t height = 0;
	// The 16x16 sector grid, its origin and the .trn's wraps, as the char
	// map's (SurfaceTypeMap).
	const int *sector_grid = nullptr;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
	bool wrap_x = false;
	bool wrap_z = false;

	TerrainAtlasRouting routing() const { return TerrainAtlasRouting{sector_grid, origin_x, origin_y, wrap_x, wrap_z}; }
};

// The definition-slot mask at a fixed-point (16.16) mission position: bit n
// set where definition n grows. 0 with no foliage map loaded, on an empty
// sector cell, and where the texel's code selects no definition.
// [orig: Foliage_SampleFoliageMapMask @ 0x606620]
inline int32_t foliage_mask_at_fixed(const FoliageMaskMap &m, int32_t x_fixed, int32_t y_fixed) {
	// No map loaded: nothing grows [orig: @ 0x606629..0x60662B].
	if (m.data == nullptr || m.width <= 0 || m.height <= 0 || m.sector_grid == nullptr) return 0;
	// The char map's routing (atlas_routing.h); an empty sector grows nothing [orig: @ 0x60667F..0x606682].
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(m.routing(), x_fixed, y_fixed);
	if (!texel.mapped) return 0;
	const int32_t shift = detail::surface_sample_shift(m.width);
	const int32_t sx = texel.x >> shift;
	const int32_t sz = texel.z >> shift;
	// The game reads past a map shorter than its width's power of two; the
	// port reads nothing there.
	if (sx < 0 || sx >= m.width || sz < 0 || sz >= m.height) return 0;
	return m.data[m.width * sz + sx];
}

} // namespace opennova::terrain
