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
bool tga_write_rgba32(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
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
	out.reserve(TGA_HEADER_SIZE + size_t(width) * height * 4);
	io::append_u8(out, 0);                   // no image ID
	io::append_u8(out, 0);                   // no colour map
	io::append_u8(out, TGA_TYPE_TRUE_COLOR); // uncompressed true colour
	for (int i = 0; i < 5; ++i) io::append_u8(out, 0); // the colour map's spec, unused
	io::append_u16_le(out, 0);               // x origin
	io::append_u16_le(out, 0);               // y origin
	io::append_u16_le(out, uint16_t(width));
	io::append_u16_le(out, uint16_t(height));
	io::append_u8(out, 32);
	io::append_u8(out, TGA_DESCRIPTOR_ALPHA_8);
	for (uint32_t y = height; y-- > 0;) {
		const uint8_t *row = rgba + size_t(y) * width * 4;
		for (uint32_t x = 0; x < width; ++x) {
			const uint8_t *p = row + size_t(x) * 4;
			out.push_back(p[2]);
			out.push_back(p[1]);
			out.push_back(p[0]);
			out.push_back(p[3]);
		}
	}
	return true;
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

// The game's TGA reader, a structural port of the models' and particles' archive reader [orig:
// CTerrainTileData_LoadTGAFromArchive @ 0x56E570], whose decode the menus' reader repeats [orig:
// CUIImage_LoadTGA @ 0x6647D0] (it reads the file as stored, where the archive reader unpacks a BFC1
// file first [orig: AudioFile_DecompressBFC_Aligned @ 0x75AFB0]): the sides are the header's 16-bit
// words read signed, the buffer width x height texels of four bytes, the pixels from the end of the
// image ID (byte 18 + its length). Image type 1 with a 24-bit map (byte 7): each texel's byte indexes
// the map read from the ID's end, three bytes an entry, past the map's first entry field (bytes 3..4,
// never read), its indices after map length x 3 bytes, alpha 0xFF; another entry size zeroes the
// buffer. Type 2: 24 bits a texel copied with alpha 0xFF, 32 copied whole, any other depth zeroes it.
// Type 3: 8 bits a texel as grey with alpha 0xFF; another depth leaves the buffer unset. Types 9 and
// 11 zero it. Type 10: run-length packets of 24 or 32 bits a texel (a header byte, its top bit a run
// of one value, its low seven one less than the count), the decode ending as the texels are filled;
// another depth zeroes it. Any other type leaves the buffer unset. Then every row is swapped with its
// mirror (row i with height - 1 - i, for i below height / 2, while the height is 2 or more), whatever
// the descriptor says of the origin. The texels are B, G, R, A in memory (A8R8G8B8); they are R, G, B,
// A here. The reader checks nothing against the file's size: bytes past the end read as 0.
bool tga_decode_game(const uint8_t *bytes, size_t size, TgaImage &out, std::string &error) {
	out = TgaImage();
	if (!tga_read_header(bytes, size, out.header)) {
		error = "A TGA is at least its 18-byte header.";
		return false;
	}
	const TgaHeader &header = out.header;
	const auto at = [&](size_t offset) -> uint8_t { return offset < size ? bytes[offset] : 0; };
	const int width = int16_t(header.width), height = int16_t(header.height);
	const int64_t total = int64_t(width) * height;
	if (width <= 0 || height <= 0 || total <= 0) {
		// No texel: the buffer is empty whatever the reader would write.
		out.pixels = TgaPixels::Blank;
		return true;
	}
	out.width = width;
	out.height = height;
	const size_t count = size_t(total);
	// The buffer as B, G, R, A dwords, zero where the reader writes nothing (or leaves it unset).
	std::vector<uint8_t> bgra(count * 4, 0);
	const size_t pixels_at = size_t(header.id_length) + TGA_HEADER_SIZE;
	TgaPixels made = TgaPixels::Decoded;
	switch (header.image_type) {
	case 1: {
		if (header.map_entry_bits != 24) {
			made = TgaPixels::Blank;
			break;
		}
		const size_t palette = pixels_at;
		const size_t indices = palette + size_t(header.map_length) * 3;
		out.indices.resize(count);
		for (size_t i = 0; i < count; ++i) {
			const uint8_t index = at(indices + i);
			out.indices[i] = index;
			const size_t entry = palette + size_t(index) * 3;
			bgra[i * 4 + 0] = at(entry);
			bgra[i * 4 + 1] = at(entry + 1);
			bgra[i * 4 + 2] = at(entry + 2);
			bgra[i * 4 + 3] = 0xFF;
		}
		out.palette.resize(size_t(header.map_length) * 3);
		for (size_t e = 0; e < header.map_length; ++e) {
			out.palette[e * 3 + 0] = at(palette + e * 3 + 2);
			out.palette[e * 3 + 1] = at(palette + e * 3 + 1);
			out.palette[e * 3 + 2] = at(palette + e * 3);
		}
		break;
	}
	case 2:
		if (header.bits == 24) {
			for (size_t i = 0; i < count; ++i) {
				bgra[i * 4 + 0] = at(pixels_at + i * 3);
				bgra[i * 4 + 1] = at(pixels_at + i * 3 + 1);
				bgra[i * 4 + 2] = at(pixels_at + i * 3 + 2);
				bgra[i * 4 + 3] = 0xFF;
			}
		} else if (header.bits == 32) {
			for (size_t i = 0; i < count * 4; ++i) bgra[i] = at(pixels_at + i);
		} else {
			made = TgaPixels::Blank;
		}
		break;
	case 3:
		if (header.bits == 8) {
			for (size_t i = 0; i < count; ++i) {
				const uint8_t grey = at(pixels_at + i);
				bgra[i * 4 + 0] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = grey;
				bgra[i * 4 + 3] = 0xFF;
			}
		} else {
			made = TgaPixels::Unset;
		}
		break;
	case 9:
	case 11: made = TgaPixels::Blank; break;
	case 10: {
		if (header.bits != 24 && header.bits != 32) {
			made = TgaPixels::Blank;
			break;
		}
		const size_t depth = header.bits / 8;
		size_t source = pixels_at, texel = 0;
		while (texel < count) {
			const uint8_t packet = at(source++);
			const size_t run = size_t(packet & 0x7F) + 1;
			if (packet & 0x80) {
				for (size_t n = 0; n < run && texel < count; ++n, ++texel) {
					bgra[texel * 4 + 0] = at(source);
					bgra[texel * 4 + 1] = at(source + 1);
					bgra[texel * 4 + 2] = at(source + 2);
					bgra[texel * 4 + 3] = depth == 4 ? at(source + 3) : 0xFF;
				}
				source += depth;
			} else {
				for (size_t n = 0; n < run && texel < count; ++n, ++texel, source += depth) {
					bgra[texel * 4 + 0] = at(source);
					bgra[texel * 4 + 1] = at(source + 1);
					bgra[texel * 4 + 2] = at(source + 2);
					bgra[texel * 4 + 3] = depth == 4 ? at(source + 3) : 0xFF;
				}
			}
		}
		break;
	}
	default: made = TgaPixels::Unset; break;
	}
	out.pixels = made;
	// The flip, whatever the descriptor says.
	if (height >= 2) {
		const size_t row = size_t(width) * 4;
		for (int y = 0; y < height / 2; ++y) {
			uint8_t *top = bgra.data() + size_t(y) * row;
			uint8_t *bottom = bgra.data() + size_t(height - 1 - y) * row;
			std::swap_ranges(top, top + row, bottom);
		}
		if (!out.indices.empty())
			for (int y = 0; y < height / 2; ++y) {
				uint8_t *top = out.indices.data() + size_t(y) * size_t(width);
				std::swap_ranges(top, top + width, out.indices.data() + size_t(height - 1 - y) * size_t(width));
			}
	}
	out.rgba.resize(count * 4);
	for (size_t i = 0; i < count; ++i) {
		out.rgba[i * 4 + 0] = bgra[i * 4 + 2];
		out.rgba[i * 4 + 1] = bgra[i * 4 + 1];
		out.rgba[i * 4 + 2] = bgra[i * 4 + 0];
		out.rgba[i * 4 + 3] = bgra[i * 4 + 3];
	}
	return true;
}

} // namespace opennova::tga
