#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/pcx/pcx.h>

namespace opennova::png {

// A portable PNG reader for the image import (ADR 0046 d10, S8): the eight-byte
// signature, the chunk walk with CRC checks, IHDR / PLTE / tRNS / IDAT / IEND, the
// zlib stream inflated through the vendored miniz, the five scanline filters, and every
// color type at every bit depth expanded to 8-bit RGBA (16-bit samples keep their high
// byte). Interlaced images are refused: the import's sources are authored files, and
// a clear refusal beats a silent de-interlace. Nothing here is a port: PNG is a public
// format the tools read on the way in.
//
// `indexed`, where the caller asks it: a palette image's texels as the indices they hold
// (the top row first) and its palette, an entry the PLTE leaves out black (ADR 0046 S20,
// a terrain's surface map, whose indices are its classes); any other image leaves it empty.
bool decode_png(const std::vector<uint8_t> &bytes, RgbaImage &out, std::string &error,
                IndexedImage8 *indexed = nullptr);

// A PNG as one grey value a texel, at its own depth (ADR 0046 S20, a heightmap's form): a 16-bit
// image's samples whole (`max_value` 65535), an 8-bit one's as they are (255), a sub-byte grey scaled
// to 0..255; a colour or palette image's red, green and blue averaged. Interlaced images are refused
// as decode_png refuses them.
struct GrayImage {
	int width = 0;
	int height = 0;
	uint32_t max_value = 255;
	std::vector<uint16_t> samples; // row-major, the top row first
};
bool decode_png_gray(const std::vector<uint8_t> &bytes, GrayImage &out, std::string &error);
bool is_png(const std::vector<uint8_t> &bytes);
// The size a PNG states in its IHDR, the chunk that follows the signature, read without
// decoding the image (a texture probe measures a menu's PNG with it): false when
// the bytes are not a PNG that starts with one.
bool png_header_size(const std::vector<uint8_t> &bytes, uint32_t &width, uint32_t &height);

} // namespace opennova::png
