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
// palette entry's luminance [orig: load_texture_from_archive @ 0x58b980 —
// table build @ 0x58bc35..0x58bca9, per-pixel A @ 0x58bcee].
bool decode_pcx_luminance_alpha(const uint8_t *data, size_t size, RgbaImage &out, std::string &error);

} // namespace opennova
