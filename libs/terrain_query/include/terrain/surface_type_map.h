#pragma once

// Terrain surface-type (charmap) sampling at a fixed-point mission position.
//
// The faithful port of the runtime charmap sampler [orig:
// Terrain_GetSurfaceTypeAtPosition @ 0x606510]: mission x/y -> 512-unit sector
// cell -> the 16x16 sector grid picks the charmap quadrant (the same 2x2
// 1024-atlas routing as coords.h; slot bit0 = +512 fine Z, bit1 = +512 fine X)
// -> one point sample of the palette-indexed raster. Off-grid cells clamp to
// the edge cell; an UNMAPPED cell returns surface 7, the off-island ocean
// default. The retail placed-tile override pass (road/runway tiles remapping
// the surface through the .til table @ 0x6065ca-0x60660c) is not modeled here
// — the placed-tile list is not sim-plumbed yet (see the world sound D-entry).
//
// Consumers: infantry footsteps (surface 3 = snow picks the SS*FootSnow
// slots); the ammo impact table reuses the same sampler with a +4 shift.
//
// Godot-agnostic: raw pointer + scalars; the embedder (NovaSimulation) points this
// at NovaTerrainData's decoded charmap and the TRN sector grid.

#include <cstdint>

namespace opennova::terrain {

struct SurfaceTypeMap {
	// Palette-indexed charmap raster, row-major, power-of-two square up to
	// 1024 (JO ships 256x256). The fine coordinate is >>'d down from the
	// 1024-unit atlas domain [orig: the (10 - shift) sample @ 0x6065c6].
	const uint8_t *data = nullptr;
	int32_t width = 0;
	int32_t height = 0;
	// 16x16 sector grid, row-major ints (0 = empty, 1..4 = quadrant), plus the
	// grid origin in 512-unit sector coordinates [orig: Terrain_SectorGrid
	// @ 0x319fc10, Terrain_SectorOrigin @ 0x319b2e0/0x319b2e4].
	const int *sector_grid = nullptr;
	int32_t origin_x = 0;
	int32_t origin_y = 0;
};

namespace detail {
inline int32_t surface_clamp_grid(int32_t v) {
	// Off-grid sector clamp: negative -> cell 0, past-the-end -> cell 15
	// [orig: the ~(v >> 31) byte trick behind the OOB mask test @ 0x606547].
	if ((v & ~0xF) != 0) return v < 0 ? 0 : 15;
	return v;
}
inline int32_t surface_shift(int32_t width) {
	int32_t s = 0;
	while ((1 << s) < width && s < 10) ++s;
	return s;
}
} // namespace detail

// Surface type at a fixed-point (16.16) mission-frame position. Matches the
// original including its defaults: 1 with no charmap loaded, 7 (ocean) on an
// unmapped sector cell. [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]
inline int32_t surface_type_at_fixed(const SurfaceTypeMap &m, int32_t x_fixed, int32_t y_fixed) {
	if (m.data == nullptr || m.width <= 0 || m.height <= 0) return 1;
	if (m.sector_grid == nullptr) return 7;
	const int32_t col = detail::surface_clamp_grid((x_fixed >> 25) - m.origin_x);
	const int32_t row = detail::surface_clamp_grid(((-y_fixed) >> 25) - m.origin_y);
	const int32_t slot = m.sector_grid[16 * (row & 0xF) + (col & 0xF)] - 1;
	if (slot < 0) return 7;
	int32_t fine_x = (x_fixed >> 16) & 0x1FF;
	int32_t fine_z = ((-y_fixed) >> 16) & 0x1FF;
	if ((slot & 1) != 0) fine_z += 512;
	if ((slot & 2) != 0) fine_x += 512;
	const int32_t shift = 10 - detail::surface_shift(m.width);
	const int32_t sx = fine_x >> shift;
	const int32_t sz = fine_z >> shift;
	if (sx < 0 || sx >= m.width || sz < 0 || sz >= m.height) return 1;
	return m.data[m.width * sz + sx];
}

} // namespace opennova::terrain
