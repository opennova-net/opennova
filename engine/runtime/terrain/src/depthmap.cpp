#include "depthmap.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace opennova {

static constexpr int MAP_SIZE = 1024;

// File I/O wrapper (no direct IDA counterpart)
std::vector<uint8_t> load_depthmap_raw(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open depth map: " + filepath);
    }

    std::vector<uint8_t> data(MAP_SIZE * MAP_SIZE);
    file.read(reinterpret_cast<char*>(data.data()), data.size());

    if (file.gcount() != static_cast<std::streamsize>(data.size())) {
        throw std::runtime_error("Depth map file too small: " + filepath);
    }

    return data;
}

std::vector<uint16_t> load_depthmap_raw16(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open depth map: " + filepath);
    }

    std::vector<uint16_t> data(MAP_SIZE * MAP_SIZE);
    file.read(reinterpret_cast<char*>(data.data()), data.size() * 2);

    if (file.gcount() != static_cast<std::streamsize>(data.size() * 2)) {
        throw std::runtime_error("16-bit depth map file too small: " + filepath);
    }

    return data;
}

// [orig: build_terrain_thread @ 0x4013A0]
// Ported from build_terrain_thread (0x4013A0) lines 52-66.
// For each pixel (row, col):
//   smoothed[row][col] = 32 * (raw[(row+1)&0x3FF][(col+1)&0x3FF]
//                             + raw[row&0x3FF][(col+1)&0x3FF]
//                             + raw[(row+1)&0x3FF][col&0x3FF]
//                             + raw[row&0x3FF][col&0x3FF])
std::vector<uint16_t> smooth_depthmap(const std::vector<uint8_t>& raw) {
    std::vector<uint16_t> out(MAP_SIZE * MAP_SIZE);

    for (int row = 0; row < MAP_SIZE; row++) {
        int row_next = (row + 1) & 0x3FF;
        int row_cur = row & 0x3FF;
        for (int col = 0; col < MAP_SIZE; col++) {
            int col_next = (col + 1) & 0x3FF;
            int col_cur = col & 0x3FF;
            uint16_t val = 32 * (raw[row_next * MAP_SIZE + col_next]
                               + raw[row_cur  * MAP_SIZE + col_next]
                               + raw[row_next * MAP_SIZE + col_cur]
                               + raw[row_cur  * MAP_SIZE + col_cur]);
            out[row * MAP_SIZE + col] = val;
        }
    }

    return out;
}

} // namespace opennova

