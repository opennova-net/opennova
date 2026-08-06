#include "lod.h"
#include "mesh_simp.h"
#include "trace.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace opennova {

// Static counter for LOD trace IDs (matches hook's g_lod_id)
static int s_lod_id = 0;

// Guard realloc sizes so a corrupt face/vertex count can't overflow int
// multiplication and produce a heap smash. 256 MiB ceiling is far above
// what a 1024×1024 terrain needs — any trip here signals a data bug.
static void _guard_realloc_bytes(const char *site, int64_t bytes) {
    constexpr int64_t kMaxBytes = 1LL << 28;
    if (bytes < 0 || bytes > kMaxBytes) {
        throw std::runtime_error(std::string("lod: ") + site
                                 + ": realloc size out of range ("
                                 + std::to_string(bytes) + " bytes)");
    }
}

// Global linked list head — matches dword_42E830
static LODMeshData* g_lodmesh_list_head = nullptr;

// [orig: sub_4091D0 @ 0x4091D0, sub_4092A0 @ 0x4092A0]
// Ported from sub_4091D0
void LODMeshData_Init(LODMeshData* mesh) {
    *mesh = LODMeshData{};
    mesh->next = g_lodmesh_list_head;
    g_lodmesh_list_head = mesh;
}

// Ported from sub_4092A0
int LODMeshData_Resize(LODMeshData* mesh, int vert_count, int fc_count) {
    if (vert_count != -1) {
        _guard_realloc_bytes("Resize.vertex_data",
                             static_cast<int64_t>(40) * vert_count);
        mesh->vertex_data = static_cast<uint8_t*>(std::realloc(mesh->vertex_data, 40 * vert_count));
        mesh->vertex_capacity = vert_count;
    }
    if (fc_count != -1) {
        _guard_realloc_bytes("Resize.face_data",
                             static_cast<int64_t>(6) * fc_count);
        mesh->face_data = static_cast<uint8_t*>(std::realloc(mesh->face_data, 6 * fc_count));
        mesh->face_capacity = fc_count;
    }
    if (mesh->vertex_data && mesh->face_data)
        return 0;
    return -1;
}

// Ported from sub_409310
int LODMeshData_SetVertex(LODMeshData* mesh, int index, const void* data) {
    if (!mesh->vertex_data || index >= mesh->vertex_capacity) {
        _guard_realloc_bytes("SetVertex",
                             static_cast<int64_t>(40) * (static_cast<int64_t>(index) + 1));
        mesh->vertex_data = static_cast<uint8_t*>(
            std::realloc(mesh->vertex_data, 40 * (index + 1)));
        mesh->vertex_capacity = index + 1;
    }
    if (!mesh->vertex_data)
        return -1;
    std::memcpy(mesh->vertex_data + 40 * index, data, 40);
    if (index + 1 > mesh->vertex_count)
        mesh->vertex_count = index + 1;
    return 0;
}

// Ported from sub_409380
int LODMeshData_SetFace(LODMeshData* mesh, int index, uint16_t v0, uint16_t v1, uint16_t v2) {
    if (!mesh->face_data || index >= mesh->face_capacity) {
        _guard_realloc_bytes("SetFace",
                             static_cast<int64_t>(6) * (static_cast<int64_t>(index) + 1));
        mesh->face_data = static_cast<uint8_t*>(
            std::realloc(mesh->face_data, 6 * (index + 1)));
        mesh->face_capacity = index + 1;
    }
    if (!mesh->face_data)
        return -1;
    auto* ptr = reinterpret_cast<uint16_t*>(mesh->face_data + 6 * index);
    ptr[0] = v0;
    ptr[1] = v1;
    ptr[2] = v2;
    if (index + 1 > mesh->face_count)
        mesh->face_count = index + 1;
    return 0;
}

// Ported from sub_4091F0
void LODMeshData_Free(LODMeshData* mesh) {
    if (mesh->vertex_data)
        std::free(mesh->vertex_data);
    if (mesh->face_data)
        std::free(mesh->face_data);
    mesh->vertex_data = nullptr;
    mesh->face_data = nullptr;

    // Free LOD level data
    for (int i = 0; i < NUM_LOD_LEVELS; i++) {
        auto& level = mesh->levels[i];
        if (level.face_index_data) {
            std::free(level.face_index_data);
            level.face_index_data = nullptr;
        }
        if (level._data2) {
            std::free(level._data2);
            level._data2 = nullptr;
        }
    }

    // Free extra data
    if (mesh->extra_data) {
        std::free(mesh->extra_data);
        mesh->extra_data = nullptr;
    }

    // Unlink from global list
    if (g_lodmesh_list_head == mesh) {
        g_lodmesh_list_head = mesh->next;
    } else {
        auto* cur = g_lodmesh_list_head;
        while (cur) {
            if (cur->next == mesh) {
                cur->next = mesh->next;
            }
            cur = cur->next;
        }
    }
}

// flt_424888 = 1.0/15.0 (step size for 16 LOD levels indexed 0-15)
static constexpr float LOD_STEP = 1.0f / 15.0f;

// Ported from sub_409400 — separate implementation from sub_409950.
// Key differences: no ReduceWithThreshold, no threshold computation.
// Target computation done at double precision (x87 PC=2) to match original.
void LODMeshData_GenerateLOD(LODMeshData* mesh, int max_vertices, int min_vertices) {
    if (mesh->vertex_count <= 0 && mesh->face_count <= 0) return;

    // Build temp vertex data (12 bytes per vertex)
    std::vector<uint8_t> vert_buf(12 * mesh->vertex_count);
    for (int i = 0; i < mesh->vertex_count; i++) {
        std::memcpy(vert_buf.data() + 12 * i, mesh->vertex_data + 40 * i, 12);
    }

    // Build face array (12 bytes per face: 3 int32 vertex indices)
    std::vector<uint8_t> face_buf(12 * mesh->face_count);
    for (int i = 0; i < mesh->face_count; i++) {
        auto* src = reinterpret_cast<uint16_t*>(mesh->face_data + 6 * i);
        auto* dst = reinterpret_cast<int32_t*>(face_buf.data() + 12 * i);
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }

    // Trace: lod_params event
    int lod_id = ++s_lod_id;
    trace_log("{\"event\":\"lod_params\",\"id\":%d,\"max\":%d,\"min\":%d,"
              "\"pa\":0,\"pb\":0,\"in_vtx\":%d,\"in_faces\":%d}",
              lod_id, max_vertices, min_vertices,
              mesh->vertex_count, mesh->face_count);

    MeshSimp_Init(vert_buf.data(), mesh->vertex_count,
                  face_buf.data(), mesh->face_count, 0);

    // IDA: count_step = float32(int_range * flt_424888)
    // x87: fild range; fmul LOD_STEP; fstp dword → stored as float32
    int range = max_vertices - min_vertices;
    float count_step = static_cast<float>(static_cast<double>(range) * static_cast<double>(LOD_STEP));
    // IDA: max stored as float32 via fild/fstp
    float max_float = static_cast<float>(max_vertices);

    // Generate 16 LOD levels — ReduceToTarget ONLY (no ReduceWithThreshold)
    for (int lod = 0; lod < NUM_LOD_LEVELS; lod++) {
        g_simp.current_lod_level = lod;

        // IDA: fild counter; fmul count_step; fsubr max_float — all at double precision (PC=2)
        // Then _ftol for truncation to int
        int target = static_cast<int>((double)max_float - (double)lod * (double)count_step);
        MeshSimp_ReduceToTarget(target);

        // Store results in LOD level
        auto& level = mesh->levels[lod];
        level.face_count = g_simp.face_count;
        level.vertex_count = g_simp.vertex_count;

        // Extract face indices
        _guard_realloc_bytes("level.face_index_data",
                             static_cast<int64_t>(6) * g_simp.face_count);
        level.face_index_data = static_cast<uint16_t*>(
            std::realloc(level.face_index_data, 6 * g_simp.face_count));
        for (int fi = 0; fi < g_simp.face_count; fi++) {
            MeshFace* face = g_simp.faces[fi];
            auto* out = level.face_index_data + fi * 3;
            out[0] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[0]) + 12));
            out[1] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[1]) + 12));
            out[2] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[2]) + 12));
        }
    }

    // Reorder vertices for cache coherency (same as GenerateLODParams)
    auto* remap = static_cast<int*>(std::malloc(4 * mesh->vertex_count));
    auto* inv_remap = static_cast<int*>(std::malloc(4 * mesh->vertex_count));
    std::memset(inv_remap, 0xFF, 4 * mesh->vertex_count);
    int remap_idx = 0;

    for (int lod = NUM_LOD_LEVELS - 1; lod >= 0; lod--) {
        auto& level = mesh->levels[lod];
        for (int fi = 0; fi < level.face_count * 3; fi++) {
            uint16_t vi = level.face_index_data[fi];
            if (inv_remap[vi] == -1) {
                inv_remap[vi] = remap_idx;
                remap[remap_idx] = vi;
                remap_idx++;
            }
        }
    }

    for (int lod = 0; lod < NUM_LOD_LEVELS; lod++) {
        auto& level = mesh->levels[lod];
        for (int fi = 0; fi < level.face_count * 3; fi++) {
            level.face_index_data[fi] = static_cast<uint16_t>(
                inv_remap[level.face_index_data[fi]]);
        }
    }

    auto* temp_verts = static_cast<uint8_t*>(std::malloc(40 * mesh->vertex_count));
    std::memcpy(temp_verts, mesh->vertex_data, 40 * mesh->vertex_count);
    for (int i = 0; i < mesh->vertex_count; i++) {
        int src_idx = remap[i];
        if (src_idx >= 0 && src_idx < mesh->vertex_count)
            std::memcpy(mesh->vertex_data + 40 * i, temp_verts + 40 * src_idx, 40);
    }
    std::free(temp_verts);
    std::free(remap);
    std::free(inv_remap);

    // Trace: lod_result
    if (g_trace) {
        fprintf(g_trace, "{\"event\":\"lod_result\",\"id\":%d,\"levels\":[", lod_id);
        for (int i = 0; i < NUM_LOD_LEVELS; i++) {
            if (i > 0) fputc(',', g_trace);
            fprintf(g_trace, "{\"fc\":%d,\"vc\":%d}",
                    mesh->levels[i].face_count, mesh->levels[i].vertex_count);
        }
        fprintf(g_trace, "]}\n");
        fflush(g_trace);
    }

    mesh->vertex_count = mesh->levels[0].vertex_count;
}

// Ported faithfully from sub_409950.
// Generates 16 LOD levels via progressive mesh simplification.
//
// Data flow:
// 1. Build temp vertex array (12 bytes/vertex: 3 floats from vertex_data[0], [1], [2])
//    and temp face array (12 bytes/face: 3 ints from face_data)
// 2. Pass to MeshSimp_Init which rebuilds global simplification state
// 3. For each of 16 LOD levels:
//    a. Compute target vertex count by interpolating max→min
//    b. Call MeshSimp_ReduceToTarget(target)
//    c. Compute threshold from params and call MeshSimp_ReduceWithThreshold
//    d. Store face count and vertex count in LOD level
//    e. Extract face indices: for each face in g_simp, read vertex->id (offset +12)
//       and store as uint16 triplets
// 4. Reorder vertices for cache coherency across all LOD levels
void LODMeshData_GenerateLODParams(LODMeshData* mesh, int max_vertices, int min_vertices,
                                    float param_a, float param_b) {
    if (mesh->vertex_count <= 0 && mesh->face_count <= 0) return;

    // Build temp vertex data (12 bytes per vertex: 3 floats = pos x,y,z)
    // from the 40-byte vertex records
    std::vector<uint8_t> vert_buf(12 * mesh->vertex_count);
    for (int i = 0; i < mesh->vertex_count; i++) {
        std::memcpy(vert_buf.data() + 12 * i, mesh->vertex_data + 40 * i, 12);
    }

    // Build face array (12 bytes per face: 3 int32 vertex indices)
    // from the 6-byte face records (3 uint16)
    std::vector<uint8_t> face_buf(12 * mesh->face_count);
    for (int i = 0; i < mesh->face_count; i++) {
        auto* src = reinterpret_cast<uint16_t*>(mesh->face_data + 6 * i);
        auto* dst = reinterpret_cast<int32_t*>(face_buf.data() + 12 * i);
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }

    // Trace: lod_params event (before MeshSimp_Init)
    int lod_id = ++s_lod_id;
    trace_log("{\"event\":\"lod_params\",\"id\":%d,\"max\":%d,\"min\":%d,"
              "\"pa\":%.9g,\"pb\":%.9g,\"in_vtx\":%d,\"in_faces\":%d}",
              lod_id, max_vertices, min_vertices, param_a, param_b,
              mesh->vertex_count, mesh->face_count);

    // Initialize global mesh simplification
    // IDA: param comes from dword_42E7E0 which is initialized to 0xFFFFFFFF (-1)
    // Non-zero param enables boundary-to-boundary vertex collapse with compatibility check
    MeshSimp_Init(vert_buf.data(), mesh->vertex_count,
                  face_buf.data(), mesh->face_count, 0);

    // Compute interpolation parameters
    // IDA: err_step = float32((param_a - param_b) * LOD_STEP)
    // x87: fld param_a; fsub param_b — subtraction at double precision (PC=2)
    // then fmul LOD_STEP; fstp dword — stored as float32
    int range = max_vertices - min_vertices;
    float err_step = static_cast<float>(((double)param_a - (double)param_b) * (double)LOD_STEP);
    float count_step = static_cast<float>((double)range * (double)LOD_STEP);
    float max_float = static_cast<float>(max_vertices);

    // Generate 16 LOD levels
    for (int lod = 0; lod < NUM_LOD_LEVELS; lod++) {
        g_simp.current_lod_level = lod;

        // IDA: fild counter → fst float(counter) → fmul count_step → fsubr max_float → _ftol
        // Counter loaded as integer (exact), multiply/subtract at double precision (PC=2)
        float lod_f = static_cast<float>(lod);  // IDA: fst stores counter as float32
        int target = static_cast<int>((double)max_float - (double)lod * (double)count_step);

        // First pass: reduce to target vertex count
        MeshSimp_ReduceToTarget(target);

        // IDA: threshold = float32(double(param_a) - double(float(counter)) * double(err_step))
        // Uses the float32 counter (from fst above), not the integer
        float threshold = static_cast<float>((double)param_a - (double)lod_f * (double)err_step);

        MeshSimp_ReduceWithThreshold(target, threshold);

        // Store results in LOD level
        auto& level = mesh->levels[lod];
        level.face_count = g_simp.face_count;
        level.vertex_count = g_simp.vertex_count;

        // Allocate and fill face index data (6 bytes per face = 3 uint16)
        level.face_index_data = static_cast<uint16_t*>(
            std::realloc(level.face_index_data, 6 * g_simp.face_count));

        for (int fi = 0; fi < g_simp.face_count; fi++) {
            MeshFace* face = g_simp.faces[fi];
            auto* out = level.face_index_data + fi * 3;
            // Extract vertex ID (offset +12 in MeshVertex, which is the 'id' field)
            out[0] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[0]) + 12));
            out[1] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[1]) + 12));
            out[2] = static_cast<uint16_t>(
                *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(face->v[2]) + 12));
        }
    }

    // Reorder vertices for cache coherency
    // Walk LOD levels from highest (15) to lowest (0), building a remap table
    auto* remap = static_cast<int*>(std::malloc(4 * mesh->vertex_count));
    auto* inv_remap = static_cast<int*>(std::malloc(4 * mesh->vertex_count));
    std::memset(inv_remap, 0xFF, 4 * mesh->vertex_count); // -1 = unmapped

    int remap_idx = 0;

    // Iterate levels in reverse (15 → 0), collecting unique vertex indices
    for (int lod = NUM_LOD_LEVELS - 1; lod >= 0; lod--) {
        auto& level = mesh->levels[lod];
        for (int fi = 0; fi < level.face_count * 3; fi++) {
            uint16_t vi = level.face_index_data[fi];
            if (inv_remap[vi] == -1) {
                inv_remap[vi] = remap_idx;
                remap[remap_idx] = vi;
                remap_idx++;
            }
        }
    }

    // Remap all face indices using the new ordering
    for (int lod = 0; lod < NUM_LOD_LEVELS; lod++) {
        auto& level = mesh->levels[lod];
        for (int fi = 0; fi < level.face_count * 3; fi++) {
            level.face_index_data[fi] = static_cast<uint16_t>(
                inv_remap[level.face_index_data[fi]]);
        }
    }

    // Reorder vertex data using remap table
    auto* temp_verts = static_cast<uint8_t*>(std::malloc(40 * mesh->vertex_count));
    std::memcpy(temp_verts, mesh->vertex_data, 40 * mesh->vertex_count);

    for (int i = 0; i < mesh->vertex_count; i++) {
        int src = remap[i];
        if (src >= 0 && src < mesh->vertex_count) {
            std::memcpy(mesh->vertex_data + 40 * i, temp_verts + 40 * src, 40);
        }
    }

    std::free(temp_verts);
    std::free(remap);
    std::free(inv_remap);

    // Trace: lod_result event (after vertex reordering)
    if (g_trace) {
        fprintf(g_trace, "{\"event\":\"lod_result\",\"id\":%d,\"levels\":[", lod_id);
        for (int i = 0; i < NUM_LOD_LEVELS; i++) {
            if (i > 0) fputc(',', g_trace);
            fprintf(g_trace, "{\"fc\":%d,\"vc\":%d}",
                    mesh->levels[i].face_count, mesh->levels[i].vertex_count);
        }
        fprintf(g_trace, "]}\n");
        fflush(g_trace);
    }

    // Update vertex count to match the first LOD level's vertex count
    // IDA: *(_DWORD *)this = *((_DWORD *)this + 15) where offset 60 = levels[0].vertex_count
    mesh->vertex_count = mesh->levels[0].vertex_count;

    // Temp arrays freed automatically (std::vector)
}

// Ported from sub_403AD0.
// Creates a compact MeshData from LODMeshData.
// Converts vertex positions (floats) to relative uint16 offsets from tile origin.
// Selects triangle list (flags=2) or strip format (flags=1) based on data presence.
MeshData LODMeshData_CreateCompact(const LODMeshData* mesh) {
    MeshData out;
    // MeshData owns std::vectors, so byte-zeroing the object is undefined
    // behavior and can leak nondeterminism into the compacted tile output.

    // Get vertex count from first LOD level
    int vtx_count = mesh->levels[0].vertex_count;
    out.vertex_data.resize(vtx_count);

    // Find min x and min z (row) across all vertices
    int min_x = 0x40000000;
    int min_z = 0x40000000;

    for (int i = 0; i < vtx_count; i++) {
        float fx, fz;
        std::memcpy(&fx, mesh->vertex_data + 40 * i, 4);      // pos[0] = x
        std::memcpy(&fz, mesh->vertex_data + 40 * i + 8, 4);  // pos[2] = z
        int ix = static_cast<int>(fx);
        int iz = static_cast<int>(fz);
        if (ix < min_x) min_x = ix;
        if (iz < min_z) min_z = iz;
    }

    out.tile_x = static_cast<uint16_t>(min_x);
    out.tile_y = static_cast<uint16_t>(min_z);

    // Store vertices as packed (x - tile_x, z - tile_y) uint16 pairs
    for (int i = 0; i < vtx_count; i++) {
        float fx, fz;
        std::memcpy(&fx, mesh->vertex_data + 40 * i, 4);
        std::memcpy(&fz, mesh->vertex_data + 40 * i + 8, 4);
        uint16_t rx = static_cast<uint16_t>(static_cast<int>(fx) - min_x);
        uint16_t rz = static_cast<uint16_t>(static_cast<int>(fz) - min_z);
        out.vertex_data[i] = (static_cast<uint32_t>(rz) << 16) | rx;
    }

    // Count LOD levels with face_index_data (triangle list) vs _data2 (strip data)
    int list_count = 0;
    int strip_count = 0;
    for (int i = 0; i < 8; i++) {
        const auto& level = mesh->levels[i * 2]; // check every other level
        if (level.face_index_data) list_count++;
        if (level._data2) strip_count++;
    }

    // Fill 16 MeshData entries from 16 LOD levels
    if (strip_count < list_count) {
        // Triangle strip mode (flags = 1)
        for (int i = 0; i < NUM_LOD_LEVELS; i++) {
            const auto& level = mesh->levels[i];
            auto& entry = out.entries[i];

            int face_count = level.face_count;
            entry.index_data.resize(face_count * 3);
            entry.face_count = face_count;
            entry.max_index = static_cast<uint16_t>(level.vertex_count);
            entry.flags = 1; // triangle list stored as face triplets

            if (level.face_index_data) {
                std::memcpy(entry.index_data.data(), level.face_index_data,
                            face_count * 3 * sizeof(uint16_t));
            }
        }
    } else {
        // Triangle strip mode with strip data (flags = 2)
        for (int i = 0; i < NUM_LOD_LEVELS; i++) {
            const auto& level = mesh->levels[i];
            auto& entry = out.entries[i];

            int index_count = level._field_8; // strip index count
            entry.index_data.resize(index_count);
            entry.face_count = index_count;
            entry.max_index = static_cast<uint16_t>(level.vertex_count);
            entry.flags = 2; // triangle strip

            if (level._data2) {
                std::memcpy(entry.index_data.data(), level._data2,
                            index_count * sizeof(uint16_t));
            }
        }
    }

    return out;
}

// Ported from sub_409ED0
// Like LoadSectionDedup but without deduplication. Uses levels[section_index]
// for vertex count, face data, and face count. Vertex data comes from
// src->vertex_data. Face indices get offset by dst's starting vertex count.
void LODMeshData_LoadSection(LODMeshData* dst, const LODMeshData* src, int section_index) {
    if (!src || !src->vertex_data) return;

    // IDA line 11: v4 = data + 28 * section_index + 44 = &src->levels[section_index]
    const auto& level = src->levels[section_index];
    int src_vert_count = level.vertex_count;
    int src_face_count = level.face_count;
    const uint16_t* src_face_data = level.face_index_data;

    if (src_vert_count <= 0) return;

    // IDA line 14: resize dst to accommodate
    int dst_vert_start = dst->vertex_count;
    int dst_face_start = dst->face_count;
    LODMeshData_Resize(dst, dst_vert_start + src_vert_count,
                       dst_face_start + (src_face_count > 0 ? src_face_count : 0));

    // Copy vertex data (first src_vert_count vertices from src->vertex_data)
    for (int i = 0; i < src_vert_count; i++) {
        LODMeshData_SetVertex(dst, dst_vert_start + i, src->vertex_data + 40 * i);
    }

    // Copy face data from level, offsetting indices by dst_vert_start
    // IDA lines 35-40: v5 + face_index (v5 = dst's original vertex count)
    if (src_face_data && src_face_count > 0) {
        for (int i = 0; i < src_face_count; i++) {
            uint16_t v0 = static_cast<uint16_t>(dst_vert_start + src_face_data[i * 3]);
            uint16_t v1 = static_cast<uint16_t>(dst_vert_start + src_face_data[i * 3 + 1]);
            uint16_t v2 = static_cast<uint16_t>(dst_vert_start + src_face_data[i * 3 + 2]);
            LODMeshData_SetFace(dst, dst_face_start + i, v0, v1, v2);
        }
    }
}

// Ported from sub_409FA0.
// Loads one LOD section, deduplicating only against vertices already present
// in dst when the call begins.
void LODMeshData_LoadSectionDedup(LODMeshData* dst, const LODMeshData* src, int section_index) {
    if (!src || !src->vertex_data) return;

    const auto& level = src->levels[section_index];
    int src_vert_count = level.vertex_count;
    int src_face_count = level.face_count;
    const uint16_t* src_face_data = level.face_index_data;

    if (src_vert_count <= 0) return;

    // Face indices can exceed level.vertex_count: GenerateLODParams stores
    // g_simp.vertex_count (alive count) in level.vertex_count, but the
    // subsequent reorder pass only guarantees face indices are within
    // [0, mesh->vertex_count). For older BHD terrains (e.g. DVD4) the root
    // merge ingests child sections where this gap is non-zero and the
    // undersized remap lookup segfaults. Size the remap to cover the full
    // index range the faces can reach, but keep the dedup loop iterating
    // only the original src_vert_count vertices so byte-identical output
    // against the Sample golden is preserved.
    int remap_size = src_vert_count;
    if (src_face_data) {
        for (int i = 0; i < src_face_count * 3; i++) {
            int v = src_face_data[i];
            if (v >= remap_size) remap_size = v + 1;
        }
    }

    int dst_face_start = dst->face_count;
    int dst_vert_base = dst->vertex_count;
    LODMeshData_Resize(dst, dst_vert_base + remap_size, dst_face_start + src_face_count);

    auto* remap = static_cast<int*>(std::malloc(4 * remap_size));
    if (!remap) return;
    for (int i = 0; i < remap_size; i++) remap[i] = -1;

    int next_vertex = dst_vert_base;
    for (int i = 0; i < src_vert_count; i++) {
        const float* src_vert = reinterpret_cast<const float*>(src->vertex_data + 40 * i);

        for (int j = 0; j < dst_vert_base; j++) {
            const float* dst_vert = reinterpret_cast<const float*>(dst->vertex_data + 40 * j);
            if (src_vert[0] == dst_vert[0] &&
                src_vert[1] == dst_vert[1] &&
                src_vert[2] == dst_vert[2] &&
                src_vert[6] == dst_vert[6] &&
                src_vert[7] == dst_vert[7] &&
                src_vert[8] == dst_vert[8] &&
                src_vert[9] == dst_vert[9] &&
                src_vert[3] == dst_vert[3] &&
                src_vert[4] == dst_vert[4] &&
                src_vert[5] == dst_vert[5]) {
                remap[i] = j;
                break;
            }
        }

        if (remap[i] == -1) {
            remap[i] = next_vertex;
            std::memcpy(dst->vertex_data + 40 * next_vertex, src->vertex_data + 40 * i, 40);
            next_vertex++;
        }
    }

    // Handle OOB face indices (vi >= src_vert_count). These point to real
    // vertices in src->vertex_data but past the section's advertised count —
    // copy them into dst without attempting to dedup against existing dst
    // vertices (matches the canonical path where such indices were only
    // reachable via an undefined-behavior read from uninitialised remap).
    if (src_face_data) {
        for (int fi = 0; fi < src_face_count * 3; fi++) {
            int vi = src_face_data[fi];
            if (vi >= src_vert_count && vi < remap_size && remap[vi] == -1) {
                remap[vi] = next_vertex;
                if (vi < src->vertex_count) {
                    std::memcpy(dst->vertex_data + 40 * next_vertex,
                                src->vertex_data + 40 * vi, 40);
                } else {
                    std::memset(dst->vertex_data + 40 * next_vertex, 0, 40);
                }
                next_vertex++;
            }
        }
    }

    if (next_vertex > dst->vertex_count) {
        dst->vertex_count = next_vertex;
    }
    LODMeshData_Resize(dst, next_vertex, -1);

    for (int i = 0; i < src_face_count; i++) {
        LODMeshData_SetFace(
            dst,
            dst_face_start + i,
            static_cast<uint16_t>(remap[src_face_data[i * 3]]),
            static_cast<uint16_t>(remap[src_face_data[i * 3 + 1]]),
            static_cast<uint16_t>(remap[src_face_data[i * 3 + 2]]));
    }

    std::free(remap);
}

} // namespace opennova

