#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/pcx/pcx.h>

namespace opennova::editor {

// A portable PNG reader for the image importer (ADR 0046 d10, S8): the eight-byte
// signature, the chunk walk with CRC checks, IHDR / PLTE / tRNS / IDAT / IEND, the
// zlib stream inflated through the vendored miniz, the five scanline filters, and every
// color type at every bit depth expanded to 8-bit RGBA (16-bit samples keep their high
// byte). Interlaced images are refused: the importer's sources are authored files, and
// a clear refusal beats a silent de-interlace. Nothing here is a port: PNG is a public
// format the editor reads on the way in.
bool decode_png(const std::vector<uint8_t> &bytes, RgbaImage &out, std::string &error);
bool is_png(const std::vector<uint8_t> &bytes);
// The size a PNG states in its IHDR, the chunk that follows the signature, read without
// decoding the image (the editor's texture probe measures a menu's PNG with it): false when
// the bytes are not a PNG that starts with one.
bool png_header_size(const std::vector<uint8_t> &bytes, uint32_t &width, uint32_t &height);

} // namespace opennova::editor
