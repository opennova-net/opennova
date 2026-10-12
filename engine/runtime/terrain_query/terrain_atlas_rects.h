#pragma once

// Where a change to a terrain's atlas shows: the inverse of the atlas routing (atlas_routing.h).
//
// A change to a rect of the atlas (a brush stroke on the heights, the char map or the foliage map) shows
// at every position of the mission whose lookup reads a changed texel. Which those are depends on the
// lookup's taps around the routed texel t on each axis: the char map, the foliage map and the nearest
// height read t alone [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065C6; Foliage_SampleFoliageMapMask @
// 0x606620; Terrain_GetHeightAtPosition @ 0x606720..0x606796]; the ground (the bilinear height every body
// collides with) reads t and t + 1 [orig: Terrain_SampleHeightBilinear @ 0x6068D0..0x60694E]; the height
// gradient the motors read takes t - 1 and t + 1 [orig: Terrain_GetHeightGradient @ 0x60648F..0x60650C].
// A tap past its quadrant's edge stays in the quadrant where the .trn locks that quadrant's axis (& 511
// from its origin) and runs across the atlas where it does not (& 1023) [orig: @ 0x6068FA..0x60694E], so
// a change at a quadrant's first column also shows in the last unit of every sector drawing the
// neighbouring quadrant (or, locked, its own quadrant's last unit). The routing then repeats: every
// sector drawing a quadrant shows the quadrant's change.
//
// The rects are the plane's (x east, z the mission's -y, whole units, half-open), one per sector and
// run of units; a tiled layout gives one per repeat. Past the grid the game repeats the grid every 16
// sectors on an axis that wraps and the edge cell's quadrant in every sector past the edge on one that
// does not (the forward routing keeps the sector offset, x & 511), so the sectors walked are the bounds'
// own, routed as a position in them is.

#include <cstdint>
#include <vector>

#include <runtime/terrain_query/atlas_routing.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::terrain {

// A rectangle of the terrain's plane in whole units, half-open: x east, z the mission's -y (the plane
// the sector grid's rows run down, coords.h's).
struct TerrainWorldRect {
	int32_t x0 = 0;
	int32_t z0 = 0;
	int32_t x1 = 0;
	int32_t z1 = 0;
};

// The texels a lookup reads around its routed texel t on each axis: t - before .. t + after.
struct TerrainAtlasTaps {
	int32_t before = 0;
	int32_t after = 0;
};
// The char map, the foliage map, the nearest height: t alone.
inline constexpr TerrainAtlasTaps kTerrainNearestTaps{0, 0};
// Every height lookup together: the ground's bilinear t, t + 1 and the gradient's t - 1, t + 1.
inline constexpr TerrainAtlasTaps kTerrainHeightTaps{1, 1};

// The routing a height field's lookups take (its grid, origin and wraps).
TerrainAtlasRouting terrain_atlas_routing(const TerrainHeightField &field);

// Every rect of `bounds` (the plane's, clipped to what 16.16 positions reach: -32768 .. 32768) whose
// lookups with `taps` read a texel of `atlas` (an atlas-texel rect, half-open, clipped to [0, 1024)),
// sector by sector in row-major order, the locks deciding the taps past a quadrant's edge.
std::vector<TerrainWorldRect> terrain_atlas_world_rects(const TerrainAtlasRouting &routing, const CoordsRect &atlas,
		const TerrainWorldRect &bounds, TerrainAtlasTaps taps = kTerrainNearestTaps,
		const CoordsQuadrantLocks &locks = {});
// The same over the grid's own 16 x 16 sectors (origin_x .. origin_x + 15, origin_y .. origin_y + 15).
std::vector<TerrainWorldRect> terrain_atlas_world_rects(const TerrainAtlasRouting &routing, const CoordsRect &atlas,
		TerrainAtlasTaps taps = kTerrainNearestTaps, const CoordsQuadrantLocks &locks = {});
// A change to the heights: every height lookup's taps and the field's locks, over `bounds`.
std::vector<TerrainWorldRect> terrain_height_world_rects(const TerrainHeightField &field, const CoordsRect &atlas,
		const TerrainWorldRect &bounds);

// The atlas-texel rect a rect of a map `map_width` wide covers: each map texel the 2^shift atlas texels the
// samplers read it for (detail::surface_sample_shift), clipped to [0, 1024); a map 2048 wide or more is
// read at its texel (0, 0) alone, which covers the whole atlas and every other texel none. Empty for a
// rect of no size or a negative origin, and for a texel the samplers never reach (past the width's power
// of two below 1024).
CoordsRect terrain_map_atlas_rect(const CoordsRect &map_rect, int32_t map_width);

} // namespace opennova::terrain
