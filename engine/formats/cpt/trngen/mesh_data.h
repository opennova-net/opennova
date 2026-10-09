#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::trngen {

// The TPM1 tile mesh TrnGen.exe writes between its passes (.tml from the quadtree bake, .tms
// after the strip remap; docs/terrain/terrain-re.md, "The TPM1 tile-mesh format"). The bake
// (terrain_bake.h) keeps its tile files in memory, but writes and reads each through this
// codec exactly as TrnGen does on disk: the file holds every OTHER LOD entry and its index
// codecs keep what they keep, and the .cpt is made from what a read gives back.

// LOD entry — 12 bytes each, 16 entries in MeshData.
// File I/O writes every OTHER entry (indices 0,2,4,...,14 = 8 entries to disk).
struct MeshLODEntry {
    std::vector<uint16_t> index_data; // runtime: variable-length index buffer
    uint32_t face_count = 0;         // on-disk at +4
    uint16_t max_index = 0;          // on-disk at +8
    uint16_t flags = 0;              // on-disk at +10, bit 1 = triangle strip

    bool is_strip() const { return (flags & 2) != 0; }
};

// MeshData — 204 bytes (0xCC) in the original binary.
// Layout: 12-byte header + 16 × 12-byte LOD entries.
// The file writes only 8 of the 16 entries (every other one).
struct MeshData {
    static constexpr uint32_t MAGIC = 0x314D5054; // "TPM1"
    static constexpr int NUM_LOD_ENTRIES = 16;
    static constexpr int FILE_LOD_ENTRIES = 8;  // written to disk

    uint16_t tile_x = 0;
    uint16_t tile_y = 0;
    std::vector<uint32_t> vertex_data; // 4 bytes per vertex (packed x,y offsets)
    MeshLODEntry entries[NUM_LOD_ENTRIES]; // 16 LOD levels

    // The file's bytes, and a file read back; false (out untouched) for bytes that are not a TPM1
    // file or end early.
    std::vector<uint8_t> write_bytes() const;
    static bool read_bytes(const std::vector<uint8_t> &bytes, MeshData &out);
};
// The strip-conversion + cache reorder pass over a MeshData (remap_vertex_ordering) is bake
// machinery, not format knowledge — it lives with the tristripper (tristrip.h).

} // namespace opennova::trngen
