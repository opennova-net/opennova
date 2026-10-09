#pragma once

// A terrain's two 8-bit maps made from a modder's picture (the image import's sources,
// runtime/renderer/texture_authoring.h decode_image_source): the char map (the .trn's polytrn_charmap,
// retail's `<stem>_m.pcx`) and the foliage map (polytrn_foliagemap, `<stem>_f.pcx`), each as the indices the
// game's 8-bit PCX reader keeps and the palette the PCX writer stores beside them (formats/pcx
// encode_pcx_indexed). Each is laid over the 1024-unit heightmap as the colour map is, at a side the game samples
// whole: square, its side a power of two of at most 1024 (terrain_query surface_sample_extent), as the game keeps
// the width alone, the rows' length and the power of two it samples the heightmap by.

#include <cstdint>
#include <string>
#include <vector>

#include <formats/pcx/pcx.h>

namespace opennova::terrain {

// A char map's least side, the importer's (JO ships 512).
inline constexpr int kCharmapSourceSideMin = 256;

// The char map of a picture: its surface classes as indices, the legend (formats/trn/charmap_legend.h) its
// palette, every entry past it white as the shipped legend leaves its own. An indexed picture (an 8-bit PCX, a
// palette PNG) is read by its indices, a colour picture by its colours, each looked up in the legend exactly (no
// nearest colour; the alpha ignored). False, with `why` naming the file, for one that does not read, is not square
// with a side of 256, 512 or 1024 [orig: sub_605A10 @ 0x605A82..0x605AA0; Terrain_GetSurfaceTypeAtPosition
// @ 0x6065C6], or holds a texel of no class (an index past 19, a colour the legend lacks: the first named by its
// place, column and row from the top left).
bool decode_charmap_source(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                           std::string &why);

// The foliage map of a picture: its codes as indices, an indexed picture's palette kept (the game reads none of
// it: a viewer shows the map as it was painted), a grey picture's a grey ramp. An indexed picture (an 8-bit PCX, a
// palette PNG) is read by its indices, a picture whose every texel is grey by its levels. False, with `why` naming
// the file, for one that does not read, holds colour, or is not square with a side a power of two at most 1024
// [orig: Foliage_LoadFoliageMapPCX @ 0x605B44..0x605B60, the width's log2; Foliage_SampleFoliageMapMask
// @ 0x60669C..0x60662D, the sample shifted by 10 less it].
bool decode_foliage_map_source(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                               std::string &why);

} // namespace opennova::terrain
