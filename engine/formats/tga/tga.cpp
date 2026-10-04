// The TGA writer and header size (tga.h).

#include <formats/tga/tga.h>

#include <base/io/le.h>

#include <algorithm>

namespace opennova::tga {

// The shape the game's three TGA readers take alike: the 18-byte header's image type at
// byte 2, its width and height at 12 and 14, its bits a pixel at 16, the pixels right after
// the image ID (none here); a 32-bit image is copied as it is, B, G, R, A, and every image
// is turned upright afterwards whatever the descriptor's origin bit says, so the rows go
// from the bottom up [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570, the models' and
// the particles' archive reader: the copy @ 0x56E7A5, the flip @ 0x56E997..0x56E9EE;
// CUIImage_LoadTGA @ 0x6647D0, the menus': the copy @ 0x664A8D, the flip @ 0x66499D..0x6649F8;
// CTextureData_LoadTGA @ 0x5F7B20, a particle's loose file]. The descriptor says bottom-left
// with its 8 alpha bits, so a reader that honours the bit reads the same rows.
namespace {

// The writer of both depths: the header, then the rows from the bottom up, B, G, R and, at 32 bits, A.
bool write_true_colour(const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t bits, std::vector<uint8_t> &out,
                       std::string &error) {
	out.clear();
	if (rgba == nullptr || width == 0 || height == 0) {
		error = "A TGA needs at least one pixel.";
		return false;
	}
	if (width > 0xFFFFu || height > 0xFFFFu) {
		error = "A TGA side is at most 65535 pixels.";
		return false;
	}
	const bool alpha = bits == 32;
	out.reserve(TGA_HEADER_SIZE + size_t(width) * height * (alpha ? 4 : 3));
	io::append_u8(out, 0);                   // no image ID
	io::append_u8(out, 0);                   // no colour map
	io::append_u8(out, TGA_TYPE_TRUE_COLOR); // uncompressed true colour
	for (int i = 0; i < 5; ++i) io::append_u8(out, 0); // the colour map's spec, unused
	io::append_u16_le(out, 0);               // x origin
	io::append_u16_le(out, 0);               // y origin
	io::append_u16_le(out, uint16_t(width));
	io::append_u16_le(out, uint16_t(height));
	io::append_u8(out, bits);
	io::append_u8(out, alpha ? TGA_DESCRIPTOR_ALPHA_8 : 0);
	for (uint32_t y = height; y-- > 0;) {
		const uint8_t *row = rgba + size_t(y) * width * 4;
		for (uint32_t x = 0; x < width; ++x) {
			const uint8_t *p = row + size_t(x) * 4;
			out.push_back(p[2]);
			out.push_back(p[1]);
			out.push_back(p[0]);
			if (alpha) out.push_back(p[3]);
		}
	}
	return true;
}

} // namespace

bool tga_write_rgba32(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                      std::string &error) {
	return write_true_colour(rgba, width, height, 32, out, error);
}

// A 24-bit image reads opaque in every game reader: the true-colour copy of three bytes a pixel, A set
// to 255 [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570, the switch @ 0x56E6C2].
bool tga_write_rgb24(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                     std::string &error) {
	return write_true_colour(rgba, width, height, 24, out, error);
}

// The header the game's readers take (above): the image type at byte 2, the sides at 12
// and 14.
bool tga_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height) {
	if (bytes == nullptr || size < TGA_HEADER_SIZE) return false;
	const uint8_t type = bytes[2];
	if (type != 1 && type != 2 && type != 3 && type != 9 && type != 10 && type != 11) return false;
	width = io::read_u16_le(bytes + 12);
	height = io::read_u16_le(bytes + 14);
	return true;
}

bool tga_read_header(const uint8_t *bytes, size_t size, TgaHeader &out) {
	out = TgaHeader();
	if (bytes == nullptr || size < TGA_HEADER_SIZE) return false;
	out.id_length = bytes[0];
	out.colour_map_type = bytes[1];
	out.image_type = bytes[2];
	out.map_first = io::read_u16_le(bytes + 3);
	out.map_length = io::read_u16_le(bytes + 5);
	out.map_entry_bits = bytes[7];
	out.x_origin = io::read_u16_le(bytes + 8);
	out.y_origin = io::read_u16_le(bytes + 10);
	out.width = io::read_u16_le(bytes + 12);
	out.height = io::read_u16_le(bytes + 14);
	out.bits = bytes[16];
	out.descriptor = bytes[17];
	return true;
}

} // namespace opennova::tga
