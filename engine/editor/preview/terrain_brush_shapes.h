#pragma once

#include <editor/preview/terrain_brush.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

// What the terrain brush draws over a view (ADR 0046 S25): over the canvas, never depth-tested.

// The brush's colours: its rim, its falloff ring and its centre (the brush's own, apart from the marks' roles).
inline constexpr uint32_t kTerrainBrushRgb = 0xFFD24A;
inline constexpr uint32_t kTerrainBrushSoftRgb = 0xFFF0B0;

// In a 3D view: its rim, kTerrainRingPoints points at the radius each on the ground's height there (so it rides the
// ground a stroke patches), a fainter ring where its falloff begins, a cross at its centre; `flat` (Set, Flatten) a flat
// ring at `flat_height` in its place.
inline constexpr int kTerrainRingPoints = 64;
void terrain_brush_ring(const TerrainBrushSpace &space, const TerrainBrush &brush, TerrainPoint centre, bool flat,
                        double flat_height, OverlayList &out);
// On a planar view (the 2D map): a circle of radius x pixels-per-metre, its falloff ring and its centre.
void terrain_brush_circle(const TerrainBrushSpace &space, const TerrainBrush &brush, TerrainPoint centre, OverlayList &out);
// A tile's square (a stamp's ghost, a selected tile): the 16-unit square with `corner` its south-west corner (the
// record's, Nile's cursor), each side cut in four to follow the ground in 3D, and a tick at the corner its rotation and
// flips turn (where the tile's first texel lands).
inline constexpr double kTerrainTileUnits = 16.0;
void terrain_tile_square(const TerrainBrushSpace &space, TerrainPoint corner, int rotation, bool flip_h, bool flip_v,
                         OverlayRole role, OverlayList &out);

} // namespace opennova::editor
