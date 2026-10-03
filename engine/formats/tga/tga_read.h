// The game's TGA reader: the decode every texture loader runs on a .tga (and .mdt)
// file, models, terrain, HUD, scars, sky and cinematics through
// CTerrainTileData_LoadTGAFromArchive, the menus through CUIImage_LoadTGA and the
// particle atlas through CTextureData_LoadTGA, three twins of one decode
// (tga_read.cpp holds the witnesses). Witness record: docs/render/render-material-re.md
// ("The TGA reader").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::tga {

// A decoded image: `rgba` holds width x height pixels, R, G, B, A each, the top row
// (as the texture is drawn) first.
struct TgaImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;
};

// Decodes `data` as the game does:
// - the image type (byte 2) picks the form: 1 colour-mapped with 8-bit indices into a
//   24-bit map (another map entry size reads as zeros), 2 true-colour at 24 or 32 bits
//   (another depth reads as zeros), 3 grey at 8 bits, 10 run-length true-colour at 24
//   or 32 bits; 9 and 11 read as zeros;
// - the pixels (and a type-1 map) start right after the 18-byte header and the image
//   ID (byte 0), whatever the colour-map fields say, so a true-colour file that carries
//   a colour map reads shifted;
// - 24-bit, colour-mapped and grey pixels are opaque, 32-bit ones keep their alpha;
// - the rows are always flipped: the descriptor (byte 17) is never read, so the file's
//   first row is the bottom one, origin bit or not.
// Retail leaves the buffer unfilled for a grey file of another depth or an unknown
// type (garbage); this port reads those as zeros. Bytes the file does not hold read as
// 0 (retail reads past its buffer). False, with `error`, when the header is short or
// the width (byte 12) or height (byte 14), read as signed 16-bit values, is not
// positive.
bool tga_decode_retail(const uint8_t *data, size_t size, TgaImage &out, std::string &error);

} // namespace opennova::tga
