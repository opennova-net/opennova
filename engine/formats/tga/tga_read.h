// The game's TGA reader: the decode every texture loader runs on a .tga (and .mdt)
// file, models, terrain, HUD, scars, sky and cinematics through
// CTerrainTileData_LoadTGAFromArchive and the menus through its twin CUIImage_LoadTGA;
// the particle manager's CTextureData_LoadTGA runs the same decode on its loose leg
// with its own flip (tga_read.cpp holds the witnesses). Witness record:
// docs/render/render-material-re.md ("The TGA reader").
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

// Which reader decodes the file.
enum class TgaReaderForm : uint8_t {
	// CTerrainTileData_LoadTGAFromArchive (and CUIImage_LoadTGA): the sides read
	// signed, every row flipped.
	Archive,
	// CTextureData_LoadTGA's loose leg (a particle graphic found as a loose file): the
	// sides read unsigned, and the flip steps the bottom rows by the HEIGHT instead of
	// the width, so a non-square image comes out garbled.
	ParticleLoose,
};

// The most pixels any file decodes to: Godot's own image limit (Image::MAX_PIXELS).
inline constexpr size_t kMaxTgaPixels = size_t(1) << 28;

// The sides `data` decodes to. False, with `error`, when the header is short, a side
// is not positive, or the header names more pixels than the file's bytes can describe
// (32 per byte after the header and image ID, the most a run-length packet expands)
// or than kMaxTgaPixels: retail's allocation of a header-sized buffer would fail
// there, a load failure (code 2), and the port never allocates that buffer.
bool tga_retail_size(const uint8_t *data, size_t size, int &width, int &height, std::string &error,
		TgaReaderForm form = TgaReaderForm::Archive);

// Decodes `data` into `rgba`, width x height x 4 bytes as tga_retail_size gave them:
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
// 0 (retail reads past its buffer). False when tga_retail_size is.
bool tga_decode_retail_into(const uint8_t *data, size_t size, uint8_t *rgba, std::string &error,
		TgaReaderForm form = TgaReaderForm::Archive);

// tga_retail_size, then tga_decode_retail_into a vector.
bool tga_decode_retail(const uint8_t *data, size_t size, TgaImage &out, std::string &error,
		TgaReaderForm form = TgaReaderForm::Archive);

} // namespace opennova::tga
