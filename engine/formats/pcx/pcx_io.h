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

// Decode an indexed PCX and synthesize its alpha plane from palette
// luminance — the sky/effect texture loader's RGBA expansion: per palette
// entry A[i] = (85 * (r + g + b)) >> 8, then each pixel's alpha byte is its
// palette entry's luminance [orig: Texture_LoadFromArchive @ 0x58b980 —
// table build @ 0x58bc35..0x58bca9, per-pixel A @ 0x58bcee].
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
bool decode_pcx_menu_rgba(const uint8_t *data, size_t size, RgbaImage &out, std::string &error);

} // namespace opennova
