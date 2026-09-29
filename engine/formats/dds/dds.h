// DDS: the DirectDraw surface file the game hands whole to D3DX, which reads the file's own
// pixel format (dds.cpp holds the witnesses). The writer makes the plainest one: A8R8G8B8
// (32 bits a pixel, alpha in the top byte), one level and no mip chain, its pixels B, G, R, A
// from the top row down. The writer and a header's size: the runtime decodes a DDS through
// its embedder (the shell's Godot decoder), and the editor reads a header's size alone
// (editor/preview/texture_header).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::dds {

inline constexpr size_t DDS_HEADER_SIZE = 128; // "DDS " and the 124-byte DDS_HEADER

// The header's flags and fields the writer sets (the DirectDraw names).
inline constexpr uint32_t DDSD_CAPS = 0x1;
inline constexpr uint32_t DDSD_HEIGHT = 0x2;
inline constexpr uint32_t DDSD_WIDTH = 0x4;
inline constexpr uint32_t DDSD_PITCH = 0x8;
inline constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
inline constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
inline constexpr uint32_t DDPF_RGB = 0x40;
inline constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;

// `rgba` holds width x height pixels, R, G, B, A each, the top row first. False, with
// `error`, for an empty image or one whose row pitch a 32-bit field cannot hold.
bool dds_write_a8r8g8b8(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                        std::string &error);

// The size a DDS header states: DirectDraw's layout, the "DDS " magic and then the
// DDS_HEADER, whose height and width are the dwords at file offsets 12 and 16. False when
// `size` is shorter than those fields or the magic is not there. A side may be 0.
bool dds_header_size(const uint8_t *bytes, size_t size, uint32_t &width, uint32_t &height);

} // namespace opennova::dds
