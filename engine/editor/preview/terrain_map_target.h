#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <editor/documents/terrain_planes.h>
#include <editor/preview/terrain_brush.h>
#include <formats/trn/trn.h>

namespace opennova::editor {

// Where a terrain map's texels lie on the mission (ADR 0046 S25 B4): the game's routing of a position into the
// terrain's 1024-unit atlas (runtime/terrain_query/atlas_routing.h: the sector grid's cell, its quadrant, the
// position's offset in its sector), then the map's texel over that atlas texel. The char map and the foliage map
// are sampled at the atlas texel shifted down by the map's sample shift [orig: Terrain_GetSurfaceTypeAtPosition @
// 0x6065C6; Foliage_SampleFoliageMapMask @ 0x606620]; the colour map and the detail blend map are drawn over the
// atlas's quadrants at its texel scaled by the map's side over 1024 [orig: PolyTrn_RenderTile's UV build @
// 0x60DB62..0x60DC16, (local, local + span) / 512 in the quadrant's texture; the blend map's quadrants alike].
struct TerrainMapTarget {
	std::array<int, 256> grid{}; // the 16 x 16 sector grid, row-major (0 empty, 1..4 the quadrant)
	int32_t origin_x = 0, origin_y = 0;
	bool wrap_x = false, wrap_z = false;
	int width = 0, height = 0; // the map's texels
	bool shifted = true;       // the char and foliage maps' sample shift; else scaled over the atlas
	int32_t shift = 0;         // the sample shift (shifted)
	bool valid() const { return width > 0 && height > 0; }
	// The world units a texel spans on each axis (the step a dab walks): 2^shift, or 1024 over the side where it
	// divides it, else 1.
	int step() const;
};
TerrainMapTarget terrain_map_target(const TrnConfig &trn, TerrainPlane plane, int width, int height);

// The map texel under a mission point (metres east and north); false off the terrain (an empty sector, a texel past
// the map).
bool terrain_map_texel(const TerrainMapTarget &target, double x, double y, int &tx, int &ty);

// One texel a dab reaches: its index in the map (y x width + x) and the dab's weight there.
struct TerrainMapTexel {
	uint32_t index = 0;
	float weight = 0.0f;
};
// The map's texels a dab of `brush` at (x, y) reaches: each texel whose unit cells' centres lie within the radius
// (radius 0: the texel under the point, the original editor's one-texel click), weighed by the brush
// (terrain_brush_weight; `categorical` hard-edged), a texel the routing repeats (a tiled layout) once at its largest
// weight. In the order the texels are first reached.
std::vector<TerrainMapTexel> terrain_map_dab(const TerrainMapTarget &target, double x, double y,
                                             const TerrainBrush &brush, bool categorical);

} // namespace opennova::editor
