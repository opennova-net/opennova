// The format's own TGA decode (tga_source.h): Truevision TGA 2.0's image types, depths, colour maps and
// origins. Tooling for import sources, not a port: the game's reader is formats/tga tga_read.cpp.
#include <formats/tga/tga_source.h>

#include <algorithm>
#include <vector>

#include <formats/tga/tga.h>

namespace opennova::tga {

namespace {

// The most pixels a byte describes: a run-length packet of one count byte and one 8-bit pixel repeats 128.
constexpr size_t kMaxPixelsPerByte = 64;

// A 15- or 16-bit pixel, A RRRRR GGGGG BBBBB from its two bytes, little end first; its top bit the alpha
// where the image gives one alpha bit, else opaque.
void put16(uint8_t *dst, uint8_t lo, uint8_t hi, bool alpha_bit) {
	const uint16_t v = uint16_t(lo | hi << 8);
	const auto five = [](uint16_t c) { return uint8_t((c << 3) | (c >> 2)); };
	dst[0] = five((v >> 10) & 0x1F);
	dst[1] = five((v >> 5) & 0x1F);
	dst[2] = five(v & 0x1F);
	dst[3] = alpha_bit ? ((v & 0x8000) ? 255 : 0) : 255;
}

// One pixel of `bits` from `p` as RGBA: true colour, or grey (`grey`).
bool put_pixel(uint8_t *dst, const uint8_t *p, uint8_t bits, bool grey, bool alpha_bit) {
	if (grey) {
		dst[0] = dst[1] = dst[2] = p[0];
		dst[3] = bits == 16 ? p[1] : 255;
		return bits == 8 || bits == 16;
	}
	switch (bits) {
	case 15:
	case 16: put16(dst, p[0], p[1], alpha_bit); return true;
	case 24:
		dst[0] = p[2];
		dst[1] = p[1];
		dst[2] = p[0];
		dst[3] = 255;
		return true;
	case 32:
		dst[0] = p[2];
		dst[1] = p[1];
		dst[2] = p[0];
		dst[3] = p[3];
		return true;
	default: return false;
	}
}

} // namespace

bool decode_tga_source(const uint8_t *data, size_t size, TgaImage &out, std::string &error) {
	out = TgaImage{};
	TgaHeader h;
	if (data == nullptr || !tga_read_header(data, size, h)) {
		error = "The TGA header is cut short.";
		return false;
	}
	const uint8_t base = h.image_type >= 9 ? uint8_t(h.image_type - 8) : h.image_type;
	if (base < 1 || base > 3) {
		error = "Image type " + std::to_string(h.image_type) + " is none the TGA format defines.";
		return false;
	}
	if (h.width == 0 || h.height == 0) {
		error = "The TGA has no pixels.";
		return false;
	}
	const bool mapped = base == 1, grey = base == 3, rle = h.image_type >= 9;
	const size_t pixels = size_t(h.width) * h.height;
	size_t at = TGA_HEADER_SIZE + h.id_length;
	// The colour map, its entries after the image ID whatever the image type.
	std::vector<uint8_t> map;
	const size_t entry_bytes = (size_t(h.map_entry_bits) + 7) / 8;
	if (h.colour_map_type == 1) {
		if (mapped && h.map_entry_bits != 15 && h.map_entry_bits != 16 && h.map_entry_bits != 24 && h.map_entry_bits != 32) {
			error = "Its colour map's entries are of " + std::to_string(h.map_entry_bits) + " bits, which the TGA format does not define.";
			return false;
		}
		const size_t bytes = size_t(h.map_length) * entry_bytes;
		if (at + bytes > size) {
			error = "The TGA ends inside its colour map.";
			return false;
		}
		if (mapped) {
			map.resize(size_t(h.map_length) * 4);
			for (size_t i = 0; i < h.map_length; ++i)
				put_pixel(&map[i * 4], data + at + i * entry_bytes, h.map_entry_bits, false, h.alpha_bits() == 1);
		}
		at += bytes;
	} else if (mapped) {
		error = "A colour-mapped TGA holds no colour map.";
		return false;
	}
	const uint8_t pixel_bits = h.bits;
	if (mapped ? (pixel_bits != 8 && pixel_bits != 16)
	           : grey ? (pixel_bits != 8 && pixel_bits != 16)
	                  : (pixel_bits != 15 && pixel_bits != 16 && pixel_bits != 24 && pixel_bits != 32)) {
		error = "A TGA of image type " + std::to_string(h.image_type) + " at " + std::to_string(pixel_bits) +
		        " bits is none the TGA format defines.";
		return false;
	}
	const size_t present = size > at ? size - at : 0;
	if (pixels > kMaxTgaPixels || pixels > present * kMaxPixelsPerByte) {
		error = "The TGA names more pixels than its data can describe.";
		return false;
	}
	const size_t bpp = (size_t(pixel_bits) + 7) / 8;
	// The pixels in file order, RGBA each.
	std::vector<uint8_t> stored(pixels * 4);
	const auto decode = [&](size_t index, const uint8_t *p) {
		uint8_t *dst = &stored[index * 4];
		if (mapped) {
			const size_t entry = (bpp == 2 ? size_t(p[0] | p[1] << 8) : p[0]);
			if (entry < h.map_first || entry - h.map_first >= h.map_length) return false;
			std::copy_n(&map[(entry - h.map_first) * 4], 4, dst);
			return true;
		}
		return put_pixel(dst, p, pixel_bits, grey, h.alpha_bits() == 1);
	};
	const auto short_file = [&] {
		error = "The TGA ends before its pixels do.";
		return false;
	};
	if (!rle) {
		if (at + pixels * bpp > size) return short_file();
		for (size_t i = 0; i < pixels; ++i)
			if (!decode(i, data + at + i * bpp)) {
				error = "A pixel's colour index is past its colour map.";
				return false;
			}
	} else {
		size_t pixel = 0;
		while (pixel < pixels) {
			if (at >= size) return short_file();
			const uint8_t packet = data[at++];
			const size_t count = std::min<size_t>((packet & 0x7Fu) + 1u, pixels - pixel);
			if (packet & 0x80u) {
				if (at + bpp > size) return short_file();
				for (size_t n = 0; n < count; ++n)
					if (!decode(pixel + n, data + at)) {
						error = "A pixel's colour index is past its colour map.";
						return false;
					}
				at += bpp;
			} else {
				if (at + count * bpp > size) return short_file();
				for (size_t n = 0; n < count; ++n)
					if (!decode(pixel + n, data + at + n * bpp)) {
						error = "A pixel's colour index is past its colour map.";
						return false;
					}
				at += count * bpp;
			}
			pixel += count;
		}
	}
	// The rows put top first and each row left first, as the descriptor's origin says they are stored.
	const bool top_first = (h.descriptor & 0x20) != 0, right_first = (h.descriptor & 0x10) != 0;
	out.width = h.width;
	out.height = h.height;
	out.rgba.resize(pixels * 4);
	for (size_t y = 0; y < h.height; ++y) {
		const size_t from_row = top_first ? y : h.height - 1 - y;
		for (size_t x = 0; x < h.width; ++x) {
			const size_t from_x = right_first ? h.width - 1 - x : x;
			std::copy_n(&stored[(from_row * h.width + from_x) * 4], 4, &out.rgba[(y * h.width + x) * 4]);
		}
	}
	return true;
}

} // namespace opennova::tga
