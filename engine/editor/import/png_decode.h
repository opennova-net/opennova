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

} // namespace opennova::editor
