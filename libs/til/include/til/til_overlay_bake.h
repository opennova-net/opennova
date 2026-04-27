#pragma once

#include "til.h"

#include <cstdint>
#include <vector>

namespace opennova {

// Builds a transparent RGBA overlay texture from .til entries and a tilestrip
// RGBA atlas. Output coordinates follow the terrain colormap convention:
// x = floor(world_x), y = floor(-world_z), both wrapped to the overlay size.
//
// Engine provenance:
// - Terrain_RenderSectorTile@0x005CDAA0 iterates the .til entries per sector.
// - sub_5C42B0@0x005C42B0 applies render-time half-texel UV correction.
bool til_bake_overlay_rgba(const TilFile &til,
                           const uint8_t *atlas_rgba,
                           int atlas_width,
                           int atlas_height,
                           int overlay_width,
                           int overlay_height,
                           std::vector<uint8_t> &out_rgba);

} // namespace opennova
