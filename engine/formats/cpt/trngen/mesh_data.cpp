#include "mesh_data.h"

#include <algorithm>
#include <cstring>

#include <base/io/le.h>
#include <formats/cpt/crypto.h>

namespace opennova::trngen {

namespace {

// The index codecs, by the entry's max_index: <= 256 one byte per index, <= 1024 three 10-bit
// indices per dword, else raw u16. [orig: TrnGen.exe pack_words_to_bytes @ 0x403CD0,
// pack_words_to_10bit @ 0x403DD0, unpack_bytes_to_words @ 0x403E70, unpack_10bit_to_words @
// 0x403EF0]

// Reads the low byte of each 16-bit value, 3 bytes per group.
std::vector<uint8_t> pack_words_to_bytes(const uint16_t *src, int count) {
    int groups = (count + 2) / 3;
    std::vector<uint8_t> out(3 * groups, 0);
    for (int i = 0; i < count; i++) {
        out[i] = static_cast<uint8_t>(src[i]);
    }
    return out;
}

// Packs 3 values per 32-bit DWORD: 10 + 11 + 11 bits.
std::vector<uint8_t> pack_words_to_10bit(const uint16_t *src, int count) {
    int groups = (count + 2) / 3;
    std::vector<uint8_t> out(4 * groups, 0);
    for (int i = 0, g = 0; g < groups; g++, i += 3) {
        uint32_t packed = 0;
        if (i < count) packed = src[i] & 0x3FF;
        if (i + 1 < count) packed |= (static_cast<uint32_t>(src[i + 1]) & 0x7FF) << 10;
        if (i + 2 < count) packed |= static_cast<uint32_t>(src[i + 2]) << 21;
        io::write_u32_le(out.data() + 4 * g, packed);
    }
    return out;
}

std::vector<uint16_t> unpack_bytes_to_words(const uint8_t *src, int count) {
    std::vector<uint16_t> out(count);
    for (int i = 0; i < count; i++) {
        out[i] = src[i];
    }
    return out;
}

std::vector<uint16_t> unpack_10bit_to_words(const uint8_t *src, int count) {
    std::vector<uint16_t> out(count);
    int groups = (count + 2) / 3;
    int idx = 0;
    for (int g = 0; g < groups && idx < count; g++) {
        uint32_t packed = io::read_u32_le(src + 4 * g);
        if (idx < count) out[idx++] = packed & 0x3FF;
        if (idx < count) out[idx++] = (packed >> 10) & 0x3FF;
        if (idx < count) out[idx++] = (packed >> 21) & 0x3FF;
    }
    return out;
}

void append(std::vector<uint8_t> &out, std::vector<uint8_t> bytes) {
    rotate_encrypt(bytes.data(), bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace

// [orig: TrnGen.exe MeshData_WriteToFile @ 0x403FE0]
// Writes 8 LOD sections from every-other entry (0,2,4,...,14); every payload rotate-crypted.
std::vector<uint8_t> MeshData::write_bytes() const {
    std::vector<uint8_t> out;
    // 12-byte header: magic, tile_x, tile_y, vertex_count, reserved.
    io::append_u32_le(out, MAGIC);
    io::append_u16_le(out, tile_x);
    io::append_u16_le(out, tile_y);
    io::append_u16_le(out, static_cast<uint16_t>(vertex_data.size()));
    io::append_u16_le(out, 0);

    std::vector<uint8_t> vtx_buf(4 * vertex_data.size());
    for (size_t i = 0; i < vertex_data.size(); ++i) io::write_u32_le(vtx_buf.data() + 4 * i, vertex_data[i]);
    append(out, std::move(vtx_buf));

    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        const auto &entry = entries[i * 2];

        int total_indices = entry.face_count;
        if (!entry.is_strip()) total_indices *= 3;

        // Section metadata (12 bytes): a pointer placeholder, face_count, max_index, flags.
        io::append_u32_le(out, 0);
        io::append_u32_le(out, entry.face_count);
        io::append_u16_le(out, entry.max_index);
        io::append_u16_le(out, entry.flags);

        // The writer reads total_indices values whatever the buffer holds (a short buffer
        // reads as zeros here, where the original read past it).
        std::vector<uint16_t> indices(static_cast<size_t>(total_indices), 0);
        std::memcpy(indices.data(), entry.index_data.data(),
                    sizeof(uint16_t) * std::min(indices.size(), entry.index_data.size()));
        if (entry.max_index <= 256) {
            append(out, pack_words_to_bytes(indices.data(), total_indices));
        } else if (entry.max_index <= 1024) {
            append(out, pack_words_to_10bit(indices.data(), total_indices));
        } else {
            std::vector<uint8_t> raw(2 * indices.size());
            for (size_t k = 0; k < indices.size(); ++k) io::write_u16_le(raw.data() + 2 * k, indices[k]);
            append(out, std::move(raw));
        }
    }
    return out;
}

// [orig: TrnGen.exe MeshData_LoadFromFile @ 0x404100]
// Reads 8 LOD sections from disk into every-other entry (0,2,4,...,14).
bool MeshData::read_bytes(const std::vector<uint8_t> &bytes, MeshData &out) {
    size_t at = 0;
    const auto take = [&](size_t size, std::vector<uint8_t> &chunk) {
        if (bytes.size() - at < size) return false;
        chunk.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                     bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        return true;
    };
    std::vector<uint8_t> header;
    if (!take(12, header) || io::read_u32_le(header.data()) != MAGIC) return false;

    MeshData mesh;
    mesh.tile_x = io::read_u16_le(header.data() + 4);
    mesh.tile_y = io::read_u16_le(header.data() + 6);
    const uint32_t vtx_count = io::read_u16_le(header.data() + 8);

    std::vector<uint8_t> chunk;
    if (!take(4u * vtx_count, chunk)) return false;
    rotate_decrypt(chunk.data(), chunk.size());
    mesh.vertex_data.resize(vtx_count);
    for (uint32_t i = 0; i < vtx_count; ++i) mesh.vertex_data[i] = io::read_u32_le(chunk.data() + 4 * i);

    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        std::vector<uint8_t> section;
        if (!take(12, section)) return false;
        auto &entry = mesh.entries[i * 2];
        entry.face_count = io::read_u32_le(section.data() + 4);
        entry.max_index = io::read_u16_le(section.data() + 8);
        entry.flags = io::read_u16_le(section.data() + 10);

        int total_indices = static_cast<int>(entry.face_count);
        if (!entry.is_strip()) total_indices *= 3;
        const int groups = (total_indices + 2) / 3;

        if (entry.max_index <= 256) {
            if (!take(3u * groups, chunk)) return false;
            rotate_decrypt(chunk.data(), chunk.size());
            entry.index_data = unpack_bytes_to_words(chunk.data(), total_indices);
        } else if (entry.max_index <= 1024) {
            if (!take(4u * groups, chunk)) return false;
            rotate_decrypt(chunk.data(), chunk.size());
            entry.index_data = unpack_10bit_to_words(chunk.data(), total_indices);
        } else {
            if (!take(2u * total_indices, chunk)) return false;
            rotate_decrypt(chunk.data(), chunk.size());
            entry.index_data.resize(total_indices);
            for (int k = 0; k < total_indices; ++k) entry.index_data[k] = io::read_u16_le(chunk.data() + 2 * k);
        }
    }
    out = std::move(mesh);
    return true;
}

} // namespace opennova::trngen
