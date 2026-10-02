// TGA: the true-colour image the game's model, menu and particle loaders read (tga.cpp
// holds the witnesses). The writer makes the one shape all of them take: an uncompressed
// 32-bit true-colour image (image type 2, 32 bits a pixel, 8 of them alpha, no image ID, no
// colour map), its pixels B, G, R, A from the bottom row up, the header saying so. The writer
// and a header's size: the runtime decodes a TGA through its embedder (the shell's Godot
// decoder), and the editor reads a header's size alone (editor/preview/texture_header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::tga {

inline constexpr size_t TGA_HEADER_SIZE = 18;
inline constexpr uint8_t TGA_TYPE_TRUE_COLOR = 2;
inline constexpr uint8_t TGA_DESCRIPTOR_ALPHA_8 = 8; // 8 alpha bits, the origin at the bottom left

// `rgba` holds width x height pixels, R, G, B, A each, the top row first. False, with
// `error`, for an empty image or a side past the header's 16 bits.
bool tga_write_rgba32(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                      std::string &error);

// The size a TGA's 18-byte header states: the width and height at bytes 12 and 14 (tga.cpp
// holds the witnesses). False when `size` is shorter than the header or the image type at
// byte 2 is none of the TGA format's image forms: colour-mapped (1), true-colour (2) and grey
// (3), raw, and their run-length forms (9, 10, 11). A side may be 0.
bool tga_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height);

} // namespace opennova::tga
