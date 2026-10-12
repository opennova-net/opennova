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
// with `why` naming the file, for a file of another size or one that does not read, or a white not above 0.
bool decode_heightmap_depth(const std::string &name, const std::vector<uint8_t> &bytes, double top,
                            HeightmapDepth &out, std::string &why);

// The samples of a 16-bit grey PNG heightmap of white `top` that decode_heightmap_depth reads back as `raw16`
// exactly (a sculpted heightmap written again, ADR 0046 S25): each round(r x 65535 / (top x 256)), stepped to its
// neighbour where that rounding misses r. False, with `why`, where a height has no such sample: one past the
// white, or, under a white past 256 units (the samples then coarser than a raw step), one between two samples;
// and for a white not above 0.
bool encode_heightmap_depth16(const std::vector<uint16_t> &raw16, double top, std::vector<uint16_t> &samples,
                              std::string &why);

} // namespace opennova::trngen
