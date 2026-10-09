#pragma once

#include <cstdint>
#include <vector>

namespace opennova::png {

// A portable PNG writer (ADR 0046 S18): an 8-bit RGBA image, rows top first, as a PNG of colour type 6
// (the signature, IHDR, one IDAT of the rows each behind filter 0 deflated through the vendored miniz,
// IEND, every chunk's CRC). Nothing here is a port: PNG is a public format the tools write (the image
// import's `png` form, a thumbnail, a texture's source handed to a paint program). Empty for an empty
// image or one whose pixels do not fill `width` x `height`.
std::vector<uint8_t> encode_png_rgba(const uint8_t *rgba, uint32_t width, uint32_t height);

} // namespace opennova::png
