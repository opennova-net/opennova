#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// LOD entry — 12 bytes each, 16 entries in MeshData.
// File I/O writes every OTHER entry (indices 0,2,4,...,14 = 8 entries to disk).
struct MeshLODEntry {
    std::vector<uint16_t> index_data; // runtime: variable-length index buffer
    uint32_t face_count = 0;         // on-disk at +4
    uint16_t max_index = 0;          // on-disk at +8
    uint16_t flags = 0;              // on-disk at +10, bit 1 = triangle strip

    bool is_strip() const { return (flags & 2) != 0; }
};

// MeshData — 204 bytes (0xCC) in original binary.
// Layout: 12-byte header + 16 × 12-byte LOD entries.
// File format writes only 8 of 16 entries (every other one).
struct MeshData {
    static constexpr uint32_t MAGIC = 0x314D5054; // "TPM1"
    static constexpr int NUM_LOD_ENTRIES = 16;
    static constexpr int FILE_LOD_ENTRIES = 8;  // written to disk

    uint16_t tile_x = 0;
    uint16_t tile_y = 0;
    std::vector<uint32_t> vertex_data; // 4 bytes per vertex (packed x,y offsets)
    MeshLODEntry entries[NUM_LOD_ENTRIES]; // 16 LOD levels

    static MeshData read(const std::string& path);
    void write(const std::string& path) const;

    // Ported from sub_404480 (via thunk sub_404610).
    // Converts face lists to triangle strips and reorders vertices for cache coherency.
    void remap_vertex_ordering();
};

} // namespace opennova

