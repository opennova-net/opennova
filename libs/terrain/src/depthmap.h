#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Load a raw 1024x1024 8-bit grayscale depth map.
std::vector<uint8_t> load_depthmap_raw(const std::string& filepath);

// Load a raw 1024x1024 16-bit depth map (pre-smoothed, 2MB file).
std::vector<uint16_t> load_depthmap_raw16(const std::string& filepath);

// Smooth 8-bit depth map to 16-bit.
// Each output pixel = 32 * average of 2x2 neighbor block, with wrapping at 1024.
// Ported from build_terrain_thread (0x4013A0) smoothing loop.
std::vector<uint16_t> smooth_depthmap(const std::vector<uint8_t>& raw);

} // namespace opennova

