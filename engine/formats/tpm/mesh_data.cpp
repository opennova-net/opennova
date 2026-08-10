#include "tpm/mesh_data.h"
#include <cpt/crypto.h>
#include "packing.h"

#include <algorithm>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace opennova {

// [orig: MeshData_LoadFromFile @ 0x404100, MeshData_WriteToFile @ 0x403FE0]
// Ported from MeshData_LoadFromFile (0x404100).
// Reads 8 LOD sections from disk into every-other entry (0,2,4,...,14).
MeshData MeshData::read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open mesh file: " + path);
    }

    // Read 12-byte header
    struct FileHeader {
        uint32_t magic;
        uint16_t tile_x;
        uint16_t tile_y;
        uint16_t vertex_count;
        uint16_t reserved;
    } hdr{};
    file.read(reinterpret_cast<char*>(&hdr), 12);

    if (hdr.magic != MAGIC) {
        throw std::runtime_error("Invalid TPM1 magic in: " + path);
    }

    MeshData mesh;
    mesh.tile_x = hdr.tile_x;
    mesh.tile_y = hdr.tile_y;

    uint32_t vtx_count = hdr.vertex_count;
    mesh.vertex_data.resize(vtx_count);
    file.read(reinterpret_cast<char*>(mesh.vertex_data.data()), 4 * vtx_count);
    rotate_decrypt(reinterpret_cast<uint8_t*>(mesh.vertex_data.data()), 4 * vtx_count);

    // Read 8 LOD sections into entries at indices 0,2,4,...,14
    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        int entry_idx = i * 2; // every other entry

        struct SectionHeader {
            uint32_t _ptr_placeholder;
            uint32_t face_count;
            uint16_t max_index;
            uint16_t flags;
        } shdr{};
        file.read(reinterpret_cast<char*>(&shdr), 12);

        auto& entry = mesh.entries[entry_idx];
        entry.face_count = shdr.face_count;
        entry.max_index = shdr.max_index;
        entry.flags = shdr.flags;

        int total_indices = shdr.face_count;
        if (!entry.is_strip()) total_indices *= 3;

        int groups = (total_indices + 2) / 3;

        if (shdr.max_index <= 256) {
            size_t packed_size = 3 * groups;
            std::vector<uint8_t> packed(packed_size);
            file.read(reinterpret_cast<char*>(packed.data()), packed_size);
            rotate_decrypt(packed.data(), packed_size);
            entry.index_data = unpack_bytes_to_words(packed.data(), total_indices);
        } else if (shdr.max_index <= 1024) {
            size_t packed_size = 4 * groups;
            std::vector<uint32_t> packed(groups);
            file.read(reinterpret_cast<char*>(packed.data()), packed_size);
            rotate_decrypt(reinterpret_cast<uint8_t*>(packed.data()), packed_size);
            entry.index_data = unpack_10bit_to_words(packed.data(), total_indices);
        } else {
            entry.index_data.resize(total_indices);
            size_t raw_size = 2 * total_indices;
            file.read(reinterpret_cast<char*>(entry.index_data.data()), raw_size);
            rotate_decrypt(reinterpret_cast<uint8_t*>(entry.index_data.data()), raw_size);
        }
    }

    return mesh;
}

// Ported from MeshData_WriteToFile (0x403FE0).
// Writes 8 LOD sections from every-other entry (0,2,4,...,14).
void MeshData::write(const std::string& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create mesh file: " + path);
    }

    // Write 12-byte header
    struct FileHeader {
        uint32_t magic;
        uint16_t tile_x;
        uint16_t tile_y;
        uint16_t vertex_count;
        uint16_t reserved;
    } hdr{};
    hdr.magic = MAGIC;
    hdr.tile_x = tile_x;
    hdr.tile_y = tile_y;
    hdr.vertex_count = static_cast<uint16_t>(vertex_data.size());
    hdr.reserved = 0;
    file.write(reinterpret_cast<const char*>(&hdr), 12);

    // Write vertex data (encrypted)
    std::vector<uint8_t> vtx_buf(4 * vertex_data.size());
    std::memcpy(vtx_buf.data(), vertex_data.data(), vtx_buf.size());
    rotate_encrypt(vtx_buf.data(), vtx_buf.size());
    file.write(reinterpret_cast<const char*>(vtx_buf.data()), vtx_buf.size());

    // Write 8 LOD sections from entries at indices 0,2,4,...,14
    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        int entry_idx = i * 2;
        const auto& entry = entries[entry_idx];

        int total_indices = entry.face_count;
        if (!entry.is_strip()) total_indices *= 3;

        // Write section metadata (12 bytes)
        struct SectionHeader {
            uint32_t _ptr_placeholder;
            uint32_t face_count;
            uint16_t max_index;
            uint16_t flags;
        } shdr{};
        shdr._ptr_placeholder = 0;
        shdr.face_count = entry.face_count;
        shdr.max_index = entry.max_index;
        shdr.flags = entry.flags;
        file.write(reinterpret_cast<const char*>(&shdr), 12);

        // Write packed index data
        if (entry.max_index <= 256) {
            auto packed = pack_words_to_bytes(entry.index_data.data(), total_indices);
            rotate_encrypt(packed.data(), packed.size());
            file.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        } else if (entry.max_index <= 1024) {
            auto packed = pack_words_to_10bit(entry.index_data.data(), total_indices);
            rotate_encrypt(packed.data(), packed.size());
            file.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        } else {
            std::vector<uint8_t> raw(2 * total_indices);
            std::memcpy(raw.data(), entry.index_data.data(), raw.size());
            rotate_encrypt(raw.data(), raw.size());
            file.write(reinterpret_cast<const char*>(raw.data()), raw.size());
        }
    }
}

} // namespace opennova
