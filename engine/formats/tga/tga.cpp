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

constexpr char kTgaSignature[18] = "TRUEVISION-XFILE.";

// The writer of both depths: the header, then the rows from the bottom up, B, G, R and, at 32 bits, A,
// then the TGA 2.0 footer where the form has one. `keep_alpha`: a 24-bit form over pixels not all opaque
// is refused rather than written without their alpha.
bool write_true_colour(const uint8_t *rgba, uint32_t width, uint32_t height, const TgaForm &form,
                       std::vector<uint8_t> &out, std::string &error, bool keep_alpha) {
	out.clear();
	if (rgba == nullptr || width == 0 || height == 0) {
		error = "A TGA needs at least one pixel.";
		return false;
	}
	if (width > 0xFFFFu || height > 0xFFFFu) {
		error = "A TGA side is at most 65535 pixels.";
		return false;
	}
	if (form.bits != 24 && form.bits != 32) {
		error = "A TGA is written at 24 or 32 bits a pixel.";
		return false;
	}
	const uint8_t bits = form.bits;
	const bool alpha = bits == 32;
	if (!alpha && keep_alpha) {
		for (size_t i = 0, n = size_t(width) * height; i < n; ++i)
			if (rgba[i * 4 + 3] != 255) {
				error = "A 24-bit TGA holds no alpha: the image's alpha would be lost (write it at 32 bits).";
				return false;
			}
	}
	out.reserve(TGA_HEADER_SIZE + size_t(width) * height * (alpha ? 4 : 3) + (form.footer ? TGA_FOOTER_SIZE : 0));
	io::append_u8(out, 0);                   // no image ID
	io::append_u8(out, 0);                   // no colour map
	io::append_u8(out, TGA_TYPE_TRUE_COLOR); // uncompressed true colour
	for (int i = 0; i < 5; ++i) io::append_u8(out, 0); // the colour map's spec, unused
	io::append_u16_le(out, 0);               // x origin
	io::append_u16_le(out, 0);               // y origin
	io::append_u16_le(out, uint16_t(width));
	io::append_u16_le(out, uint16_t(height));
	io::append_u8(out, bits);
	io::append_u8(out, form.descriptor);
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
	if (form.footer) {
		io::append_u32_le(out, 0); // the extension area's offset: none
		io::append_u32_le(out, 0); // the developer area's offset: none
		out.insert(out.end(), kTgaSignature, kTgaSignature + sizeof(kTgaSignature));
	}
	return true;
}

} // namespace

bool tga_write(const uint8_t *rgba, uint32_t width, uint32_t height, const TgaForm &form, std::vector<uint8_t> &out,
               std::string &error) {
	return write_true_colour(rgba, width, height, form, out, error, true);
}

bool tga_read_form(const uint8_t *bytes, size_t size, TgaForm &out) {
	out = TgaForm();
	TgaHeader header;
	if (!tga_read_header(bytes, size, header)) return false;
	if (header.id_length != 0 || header.colour_map_type != 0 || header.image_type != TGA_TYPE_TRUE_COLOR ||
	    header.map_first != 0 || header.map_length != 0 || header.map_entry_bits != 0 || header.x_origin != 0 ||
	    header.y_origin != 0 || header.width == 0 || header.height == 0 || (header.bits != 24 && header.bits != 32))
		return false;
	// In 64 bits: a 32-bit size_t (a wasm32 build) wraps near the sides' 65535.
	const uint64_t pixels = TGA_HEADER_SIZE + uint64_t(header.width) * header.height * (header.bits / 8);
	if (uint64_t(size) == pixels) {
		out.footer = false;
	} else if (uint64_t(size) == pixels + TGA_FOOTER_SIZE && io::read_u32_le(bytes + pixels) == 0 &&
	           io::read_u32_le(bytes + pixels + 4) == 0 &&
	           std::equal(kTgaSignature, kTgaSignature + sizeof(kTgaSignature), bytes + pixels + 8)) {
		out.footer = true;
	} else {
		return false;
	}
	out.bits = header.bits;
	out.descriptor = header.descriptor;
	return true;
}

bool tga_write_rgba32(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                      std::string &error) {
	return write_true_colour(rgba, width, height, TgaForm{32, TGA_DESCRIPTOR_ALPHA_8, false}, out, error, false);
}

// A 24-bit image reads opaque in every game reader: the true-colour copy of three bytes a pixel, A set
// to 255 [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570, the switch @ 0x56E6C2].
bool tga_write_rgb24(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                     std::string &error) {
	return write_true_colour(rgba, width, height, TgaForm{24, 0, false}, out, error, false);
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
