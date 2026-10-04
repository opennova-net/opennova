#pragma once

#include "pcx.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

bool decode_pcx_rgb(const uint8_t *data, size_t size, RgbImage &out, std::string &error);
bool decode_pcx_indexed(const uint8_t *data, size_t size, IndexedImage8 &out, std::string &error);
bool encode_pcx_indexed(const IndexedImage8 &image, std::vector<uint8_t> &out, std::string &error);
// A 24-bit PCX (8 bits a plane, three planes): each row the red, green and blue planes of `width`
// bytes each, run-length coded a plane at a time, no palette. The shape the game's 24-bit path reads
// [orig: Texture_LoadPCXFromPFF32 @ 0x56EB31; load_pcx_to_argb @ 0x664cc0's NPlanes == 3 path]: 3 x
// BytesPerLine bytes a row, the planes `width` apart, so its bytes a line are its width exactly.
bool encode_pcx_rgb(const RgbImage &image, std::vector<uint8_t> &out, std::string &error);

// The archive loader's alpha read: the game's 8-bit PCX reader
// (Texture_LoadPCXFromPFF8Bit) then each pixel's palette-entry luminance
// (85 * (r + g + b)) >> 8 as its alpha, RGB the palette entry's colour [orig:
// Texture_LoadFromArchive @ 0x58b980 — table build @ 0x58bc35..0x58bca9, per-pixel A
// @ 0x58bcee]. The reader is as lax as retail's: the bits per pixel (byte 3) must be
// 8, NPlanes is not read, the 0x0C marker is not tested (the palette is the last 768
// bytes), the RLE runs from offset 128 through the file's end, and each row decodes
// BytesPerLine indices at a stride of `width`, so pad indices spill onto the next
// row's start and the last row's onto the palette, which the luminance then reads.
// Data the file does not hold reads as 0. False on a short header, a bits-per-pixel
// other than 8, a side that is not positive, or more pixels than the file's data can
// describe (decode_pcx_menu_rgba's bound).
bool decode_pcx_luminance_alpha(const uint8_t *data, size_t size, RgbaImage &out, std::string &error);

// The menu texture loader's PCX decode, a structural port [orig: load_pcx_to_argb
// @ 0x664cc0 via CTextureManager_LoadOrFindTexture @ 0x654980]: the header's bits
// per pixel (byte 3) must be 8; NPlanes (byte 0x41) 3 takes the 24-bit path, which
// reads 3 * BytesPerLine RLE bytes a row and takes the red, green and blue planes
// `width` bytes apart; any other NPlanes takes the 8-bit path: the palette is the
// last 768 bytes (every entry fully opaque, no colour key), file size - 896 bytes of
// RLE data from offset 128, each row decoded to BytesPerLine (byte 0x42) pixels
// starting `width` pixels after the last, so a row's padding lands on the start of
// the next row (the next row overwrites it; retail's last row writes past its
// buffer, which this port drops). Data the file does not hold reads as 0. RGBA8 out.
// The header-sized buffer is allocated only when the file's data can describe it (32
// pixels a byte after the 128-byte header, at most 2^28 pixels); a larger claim fails
// the decode, as retail's failed allocation fails the load (code 2).
//
// `indexed`, where the caller asks it: what the 8-bit path (NPlanes other than 3) made the
// image of, each texel's palette index as the rows wrote it (a row's padding landing on
// the next row's start, as the colours do) and the palette, the last 768 bytes; the
// models' reader decodes alike [orig: Texture_LoadPCXFromPFF32 @ 0x56EA30: the BPP check
// returning 3, the palette from Seek(-768, 2), file size - 896 bytes of RLE data from
// offset 128, each row to BytesPerLine], so a texel's colour is its index's entry,
// opaque. An image the 24-bit path reads leaves it not indexed.
struct PcxIndexed {
	bool indexed = false;
	std::vector<uint8_t> indices; // a texel's index, the top row first
	uint8_t palette[256][3] = {};
};
bool decode_pcx_menu_rgba(const uint8_t *data, size_t size, RgbaImage &out, std::string &error,
                          PcxIndexed *indexed = nullptr);

} // namespace opennova
