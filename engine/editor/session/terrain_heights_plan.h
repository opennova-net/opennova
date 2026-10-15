#pragma once

#include <vector>

#include <editor/documents/terrain_heights_brush.h>
#include <editor/preview/terrain_brush.h>
#include <formats/trn/trn.h>

namespace opennova::editor {

// A heights dab's texels (the heights' planner, session/terrain_stroke_heights.cpp; ADR 0046 S25): every whole unit of
// the mission within the brush's radius of `at` (the unit under it for a radius of 0), weighed by its distance
// (terrain_brush_weight), at the atlas texel the game routes it to through the terrain's sector grid [orig:
// Terrain_SampleHeightBilinear @ 0x6067BD..0x6068C4, runtime/terrain_query terrain_atlas_texel_at]; a texel two units
// route to (ground a tiled layout repeats) once, at its strongest weight. None off the terrain (an empty sector).
std::vector<HeightsDabTexel> heights_dab_texels(const TrnConfig &trn, const TerrainBrush &brush, TerrainPoint at);

} // namespace opennova::editor
