#include "terrain/mesh_data.h"
#include "tristrip.h"
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

// Ported from sub_404480 (via thunk sub_404610).
// Converts face lists to triangle strips and reorders vertices for optimal cache access.
// Called by generate_multiresolution_tiles before writing .tms files.
//
// IDA flow:
// 1. For each of 8 on-disk LOD entries: if not strip, call sub_4068E0 to convert
// 2. Build vertex traversal order from strips (sub_406A70, sub_406AC0)
// 3. Build forward/inverse remap tables (sub_406B00)
// 4. Reorder vertex_data using forward remap
// 5. Remap all strip indices using inverse remap, update max_index
void MeshData::remap_vertex_ordering() {
    int vtx_count = static_cast<int>(vertex_data.size());
    if (vtx_count <= 0) return;

    // Step 1: Convert face lists to triangle strips for each of 8 on-disk LOD entries
    // IDA lines 30-49: iterate 8 entries (every other = entries[0,2,4,...,14])
    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        auto& entry = entries[i * 2];
        if (entry.is_strip()) continue; // already a strip

        if (entry.face_count <= 0 || entry.index_data.empty()) continue;

        // sub_4068E0: build triangle strips from face list
        TriStripResult strip = build_triangle_strips(
            entry.index_data.data(), entry.face_count, vtx_count);

        // Replace face data with strip data
        entry.index_data = std::move(strip.indices);
        entry.face_count = static_cast<uint32_t>(entry.index_data.size());
        entry.flags = 2; // mark as strip
    }

    // Step 2: Build sort table (IDA: sub_406A70 init + sub_406AC0 per-strip)
    // Each entry: {first_seen_position, original_vertex_index}
    // Ported from sub_406A70: init table with first_seen=-1, original_idx=i
    struct SortEntry {
        int first_seen;     // position within first strip containing this vertex
        int original_idx;   // original vertex index
    };
    std::vector<SortEntry> table(vtx_count);
    for (int i = 0; i < vtx_count; i++) {
        table[i].first_seen = -1;
        table[i].original_idx = i;
    }

    // Ported from sub_406AC0: iterate strips in reverse LOD order,
    // record position of first occurrence within each strip
    // IDA: v6 starts at this+184 (last entry), decrements by 6 DWORDs
    for (int i = FILE_LOD_ENTRIES - 1; i >= 0; i--) {
        const auto& entry = entries[i * 2];
        for (int j = 0; j < static_cast<int>(entry.index_data.size()); j++) {
            uint16_t vi = entry.index_data[j];
            if (vi < vtx_count && table[vi].first_seen == -1) {
                table[vi].first_seen = j; // position within THIS strip
            }
        }
    }

    // Step 3: Sort table by first_seen (IDA: sub_406B00)
    // Ported from sub_406B00/sub_406E00: quicksort with median-of-3 pivot
    // for large arrays, insertion sort for small arrays.
    // Sort key is first_seen only. Unseen vertices (first_seen=-1) sort first
    // since all IDA comparisons are signed (jl/jge).
    // Using insertion sort for the full range to match IDA's behavior
    // for the final pass (sub_406B00 always ends with insertion sort).
    {
        // IDA sub_406E00: quicksort for partitions > 16 entries (128 bytes)
        // We implement the EXACT same quicksort+insertion sort combination.
        // For byte-identical output, the sort must produce identical ordering.

        struct QSort {
            static void quicksort(SortEntry* lo, SortEntry* hi) {
                while ((hi - lo) > 16) {
                    // Median-of-3 pivot selection (IDA: first, mid, last)
                    int n = (int)(hi - lo);
                    int last_val = (hi - 1)->first_seen;
                    int mid_val = lo[n / 2].first_seen;
                    int first_val = lo->first_seen;
                    int pivot;

                    if (first_val >= mid_val) {
                        if (first_val < last_val) {
                            pivot = first_val;
                        } else if (mid_val >= last_val) {
                            pivot = mid_val;
                        } else {
                            pivot = last_val;
                        }
                    } else {
                        if (mid_val >= last_val) {
                            if (first_val < last_val) {
                                pivot = last_val;
                            } else {
                                pivot = first_val;
                            }
                        } else {
                            pivot = mid_val;
                        }
                    }

                    // Partition — matches IDA's for(i=lo;;i++) loop structure.
                    // The i++ after swap is critical: without it, equal-element
                    // partitions degenerate and cause infinite recursion.
                    SortEntry* i = lo;
                    SortEntry* j = hi;
                    for (;;) {
                        while (i->first_seen < pivot) i++;
                        j--;
                        while (pivot < j->first_seen) j--;
                        if (j <= i) break;
                        SortEntry tmp = *i;
                        *i = *j;
                        *j = tmp;
                        i++;  // IDA: for-loop increment after swap
                    }

                    // Recurse on smaller partition, iterate on larger
                    int right_size = (int)(hi - i);
                    int left_size = (int)(i - lo);
                    if (right_size > left_size) {
                        quicksort(lo, i);
                        lo = i;
                    } else {
                        quicksort(i, hi);
                        hi = i;
                    }
                }
            }
        };

        SortEntry* base = table.data();
        SortEntry* end = base + vtx_count;

        // Quicksort phase (for > 16 elements)
        if (vtx_count > 16) {
            QSort::quicksort(base, end);
        }

        // Insertion sort phase (IDA sub_406B00 lines 192-260)
        // First pass: sort first 16 elements with insertion sort
        SortEntry* limit = (vtx_count > 16) ? base + 16 : end;
        for (SortEntry* p = base + 1; p != limit; p++) {
            int key = p->first_seen;
            int val = p->original_idx;
            if (key < base->first_seen) {
                // Shift all elements right, insert at beginning
                for (SortEntry* q = p; q != base; q--) {
                    q[0] = q[-1];
                }
                base->first_seen = key;
                base->original_idx = val;
            } else {
                SortEntry* q = p;
                while (key < q[-1].first_seen) {
                    q[0] = q[-1];
                    q--;
                }
                q->first_seen = key;
                q->original_idx = val;
            }
        }

        // Second pass: insertion sort remaining elements
        for (SortEntry* p = limit; p != end; p++) {
            int key = p->first_seen;
            int val = p->original_idx;
            SortEntry* q = p;
            while (key < q[-1].first_seen) {
                q[0] = q[-1];
                q--;
            }
            q->first_seen = key;
            q->original_idx = val;
        }
    }

    // Build forward/inverse remap from sorted table (IDA sub_406B00 lines 262-271)
    std::vector<uint16_t> forward_remap(vtx_count);   // new_idx → old_idx
    std::vector<uint16_t> inverse_remap(vtx_count);    // old_idx → new_idx
    for (int i = 0; i < vtx_count; i++) {
        forward_remap[i] = static_cast<uint16_t>(table[i].original_idx);
    }
    for (int i = 0; i < vtx_count; i++) {
        inverse_remap[forward_remap[i]] = static_cast<uint16_t>(i);
    }

    // Step 4: Reorder vertex_data using forward remap
    // IDA lines 61-84: alloc temp, copy, reorder using lpMem (forward remap)
    std::vector<uint32_t> old_data = vertex_data;
    for (int i = 0; i < vtx_count; i++) {
        vertex_data[i] = old_data[forward_remap[i]];
    }

    // Step 5: Remap all strip indices using inverse remap, update max_index
    // IDA lines 85-107: for each of 8 entries, remap indices and find max
    for (int i = 0; i < FILE_LOD_ENTRIES; i++) {
        auto& entry = entries[i * 2];
        uint16_t max_idx = 0;
        for (auto& idx : entry.index_data) {
            idx = inverse_remap[idx];
            if (idx > max_idx) max_idx = idx;
        }
        entry.max_index = max_idx + 1; // IDA line 103: max_index = max + 1
    }
}

} // namespace opennova

