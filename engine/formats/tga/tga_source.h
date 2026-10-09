// A TGA as the format defines it, read the way an image program reads it (ADR 0046 S18: an import
// source, the picture a modder brings): not the game's reader (formats/tga tga_read.h, which honours no
// origin bit and zeroes the forms it has no case for), but every form the Truevision TGA 2.0
// specification names. An import's writers then make the file the game reads. Tooling, not a port.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <formats/tga/tga_read.h>

namespace opennova::tga {

// Decodes `data` into `out` (RGBA, the top row first as the image is shown):
// - image types 1 (colour-mapped), 2 (true colour) and 3 (grey), and their run-length forms 9, 10 and
//   11;
// - true colour at 15 or 16 bits (5 bits a channel, the top bit the alpha where the descriptor gives one
//   alpha bit), 24 bits and 32 bits (its fourth byte the alpha); grey at 8 bits, or 16 (grey then alpha);
//   indices of 8 or 16 bits into a map of 15-, 16-, 24- or 32-bit entries, from its first entry index;
// - the pixels after the header, the image ID and the colour map;
// - the descriptor's origin honoured: bit 5 the first row the top one, bit 4 the first pixel of a row the
//   right one.
// False, with `error`, for a header cut short, a side of 0, a form the specification does not define, an
// index past the map, or a file that ends before its pixels do; and, as the game's reader is bounded
// (tga::tga_retail_size), for more pixels than the file's bytes can describe or than tga::kMaxTgaPixels.
bool decode_tga_source(const uint8_t *data, size_t size, TgaImage &out, std::string &error);

} // namespace opennova::tga
