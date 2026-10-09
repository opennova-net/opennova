#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::trngen {

// A heightmap as the bake takes it (terrain_bake.h's TerrainBakeInput): an 8-bit map's texels (TrnGen's input, at
// its own scale) or 16-bit heights (raw16, 1/256 world unit a step), exactly one filled, kDepthSide a side, the top
// row first.
struct HeightmapDepth {
	std::vector<uint8_t> depth8;
	std::vector<uint16_t> depth16;
};

// A heightmap from the file `name` holds, its white `top` world units: a PNG of kDepthSide x kDepthSide texels, grey
// at any depth or colour read as its grey (8 bits at TrnGen's own scale, kDepth8Top, go to the bake as they are; at
// another, smoothed as TrnGen smooths them and then scaled; 16 bits span 0 to `top`), or TrnGen's own `.raw` (1 MiB
// of 8-bit heights, scaled as an 8-bit PNG's) or 2 MiB of the game's own 16-bit heights, taken as they are. False,
// with `why` naming the file, for a file of another size or one that does not read.
bool decode_heightmap_depth(const std::string &name, const std::vector<uint8_t> &bytes, double top,
                            HeightmapDepth &out, std::string &why);

} // namespace opennova::trngen
