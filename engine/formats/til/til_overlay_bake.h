#pragma once

#include "til.h"

#include <cstdint>
#include <vector>

namespace opennova {

// Builds a transparent RGBA overlay texture from .til entries and a tilestrip
// RGBA atlas. Output coordinates follow the terrain colormap convention:
// x = floor(world_x), y = floor(-world_z), both wrapped to the overlay size.
//
// [orig: PolyTrn_RenderTile @0x60df0d — the ordered .til entry loop @0x60ddd4..0x60df1b ->
//  render_water_quad @0x604700 (flip/rotate flags + the +-half-texel bias); atlas cells from
//  Terrain_LoadTileSetAtlas @0x604a90; docs/tiles/til-re.md]
// (jodemo: Terrain_RenderSectorTile @0x5CDAA0 iterated the entries per sector and
//  sub_5C42B0 @0x5C42B0 applied the half-texel correction.)
bool til_bake_overlay_rgba(const TilFile &til,
                           const uint8_t *atlas_rgba,
                           int atlas_width,
                           int atlas_height,
                           int overlay_width,
                           int overlay_height,
                           std::vector<uint8_t> &out_rgba);

} // namespace opennova
