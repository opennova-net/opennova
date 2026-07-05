#include "terrain_mesh.h"
#include "lod.h"
#include "trace.h"

#include <cstring>
#include <cmath>

namespace opennova {

// Globals matching TrnGen.exe:
// dword_42E274[4] — pointers to 4 base mesh quadrant MeshData objects
static MeshData* g_base_meshes[4] = {};
struct CornerLockFlags {
    int x = 0;
    int y = 0;
};
static CornerLockFlags g_corner_locks[4] = {};

// Forward declaration
extern float g_curvature_weight; // flt_42A460

// Sets globals used by build_mesh_lod_vertices (no direct IDA function)
void set_mesh_corner_locks(int tl_x, int tl_y, int tr_x, int tr_y,
                           int bl_x, int bl_y, int br_x, int br_y) {
    g_corner_locks[0] = {tl_x, tl_y};
    g_corner_locks[1] = {tr_x, tr_y};
    g_corner_locks[2] = {bl_x, bl_y};
    g_corner_locks[3] = {br_x, br_y};
}

// [orig: sub_402D20 @ 0x402D20]
// Ported from sub_402D20
void generate_base_terrain_meshes() {
    LODMeshData lodmesh;

    for (int quadrant = 0; quadrant < 4; quadrant++) {
        if (g_base_meshes[quadrant]) {
            delete g_base_meshes[quadrant];
            g_base_meshes[quadrant] = nullptr;
        }

        LODMeshData_Init(&lodmesh);
        LODMeshData_Resize(&lodmesh, 4225, 0x2000);

        // Fill 65×65 vertex grid
        int vi = 0;
        for (int row = 0; row <= 64; row++) {
            float frow = static_cast<float>(row);
            float texV = frow * (1.0f / 1024.0f);
            float quarterRow = frow * 0.25f;

            for (int col = 0; col <= 64; col++) {
                float fcol = static_cast<float>(col);
                float vertex[10];
                vertex[0] = fcol;
                vertex[1] = ((col & 3) == 2 && (row & 3) == 2) ? 0.1f : 0.0f;
                vertex[2] = frow;
                vertex[3] = 0.0f;
                vertex[4] = 1.0f;
                vertex[5] = 0.0f;
                vertex[6] = fcol * (1.0f / 1024.0f);
                vertex[7] = texV;
                vertex[8] = fcol * 0.25f;
                vertex[9] = quarterRow;

                LODMeshData_SetVertex(&lodmesh, vi, vertex);
                vi++;
            }
        }

        // Ported from sub_402D20 lines 141-319
        int x_flag = quadrant & 1;   // v50, v9
        int y_flag = quadrant & 2;   // v51

        // Interior region boundaries (IDA lines 143-152)
        int col_start = x_flag ? 0 : 3;   // v10, v54
        int row_start = y_flag ? 0 : 3;   // v12, v45
        int col_end   = x_flag ? 61 : 64; // v13
        int row_end   = y_flag ? 61 : 64; // v14

        // Interior triangulation (IDA lines 153-188)
        int fi = 0;
        if (row_start < row_end) {
            int16_t v15 = row_start * 65 + col_start + 66;
            int v56 = row_start * 65 + col_start + 66;
            for (int r = row_start; r < row_end; r++) {
                int v48 = col_start;
                if (col_start < col_end) {
                    do {
                        if (((r ^ v48) & 1) != 0) {
                            LODMeshData_SetFace(&lodmesh, fi++, v15 - 65, v15 - 1, v15);
                            LODMeshData_SetFace(&lodmesh, fi++, v15 - 65, v15 - 66, v15 - 1);
                        } else {
                            LODMeshData_SetFace(&lodmesh, fi++, v15 - 66, v15, v15 - 65);
                            LODMeshData_SetFace(&lodmesh, fi++, v15 - 66, v15 - 1, v15);
                        }
                        ++v15;
                        ++v48;
                    } while (v48 < col_end);
                }
                v15 = v56 + 65;
                v56 += 65;
            }
        }

        // Edge stitching (IDA lines 191-319)
        // Generates boundary triangles at 2 LOD levels (step=4,half=2 and step=2,half=1)
        int margin = 0;
        int inner_bound = 64;

        for (int level = 2; level >= 1; level--) {
            int step = 1 << level;     // 4, 2
            int half = step >> 1;      // 2, 1

            // Compute boundaries for this level
            // IDA: v22 = (v9==0) ? v61 : 0 → x_start = !x_flag ? margin : 0
            int x_start = x_flag ? 0 : margin;
            int y_start = y_flag ? 0 : margin;
            int x_end   = x_flag ? inner_bound : 64;
            int y_end   = y_flag ? inner_bound : 64;

            int row_step_65 = 65 * step;
            int off_66h = 66 * half;   // v26: offset to bottom-right corner
            int off_64h = 64 * half;   // v59: offset to bottom-left corner
            int neg_64h = -64 * half;  // v28: offset to top-right corner
            int off_65h = 65 * half;   // v72: offset to bottom-mid

            // Iterate through cells at this step size
            // IDA: v46 = (v51==0) ? v61 : 0 → y_pos = !y_flag ? margin : 0
            int y_pos = y_flag ? 0 : margin;
            if (y_start < y_end) {
                int row_off = 65 * y_start;

                do {
                    int x_pos = x_start;
                    if (x_start < x_end) {
                        int y_end_minus_step = y_end - step;
                        int x_end_minus_step = x_end - step;
                        int16_t corner = x_start + row_off;  // v0: top-left of cell
                        int16_t mid = x_start + off_66h + row_off; // v27: midpoint

                        while (true) {
                            // Top edge: y_pos == y_start AND !y_flag
                            if (y_pos == y_start && !y_flag) {
                                LODMeshData_SetFace(&lodmesh, fi++, corner, mid, mid + neg_64h);
                                if (x_pos > x_start || x_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, corner, mid - half, mid);
                                }
                                if (x_pos < x_end_minus_step || !x_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid - off_64h, mid, mid + half);
                                }
                            }

                            // Bottom edge: y_pos >= y_end - step AND y_flag
                            if (y_pos >= y_end_minus_step && y_flag) {
                                LODMeshData_SetFace(&lodmesh, fi++, mid, mid + off_64h, mid + off_66h);
                                if (x_pos > x_start || x_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid - half, mid + off_64h, mid);
                                }
                                if (x_pos < x_end_minus_step || !x_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid, mid + off_66h, mid + half);
                                }
                            }

                            // Left edge: x_pos == x_start AND !x_flag
                            if (x_pos == x_start && !x_flag) {
                                LODMeshData_SetFace(&lodmesh, fi++, corner, mid + off_64h, mid);
                                if (y_pos > y_start || y_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, corner, mid, mid - off_65h);
                                }
                                if (y_pos < y_end_minus_step || !y_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid + off_64h, mid + off_65h, mid);
                                }
                            }

                            // Right edge: x_pos >= x_end - step AND x_flag
                            if (x_pos >= x_end_minus_step && x_flag) {
                                LODMeshData_SetFace(&lodmesh, fi++, mid, mid + off_66h, mid - off_64h);
                                if (y_pos > y_start || y_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid, mid - off_64h, mid - off_64h - half);
                                }
                                if (y_pos < y_end_minus_step || !y_flag) {
                                    LODMeshData_SetFace(&lodmesh, fi++, mid, mid + off_65h, mid + off_66h);
                                }
                            }

                            mid += step;
                            x_pos += step;
                            corner += step;
                            if (x_pos >= x_end) break;
                        }
                    }

                    y_pos += step;
                    row_off += row_step_65;
                } while (y_pos < y_end);
            }

            margin += half;
            inner_bound -= half;
        }

        g_curvature_weight = 0.01f;
        LODMeshData_GenerateLOD(&lodmesh, 4225, 4224);

        auto* compact = new MeshData(LODMeshData_CreateCompact(&lodmesh));
        g_base_meshes[quadrant] = compact;

        LODMeshData_Free(&lodmesh);
    }

    trace_log("{\"event\":\"base_meshes\",\"count\":4}");
}

// Accessor for g_base_meshes (dword_42E274)
MeshData* get_base_mesh(int quadrant) {
    if (quadrant >= 0 && quadrant < 4) return g_base_meshes[quadrant];
    return nullptr;
}

// Ported from sub_4042B0
// Expands a compact MeshData into a LODMeshData with height data from depth buffer.
void build_mesh_lod_vertices(MeshData* mesh_in, LODMeshData* lodmesh_out,
                             const uint16_t* depth_buffer, int color_param, int extra_param) {
    if (!mesh_in || !lodmesh_out || !depth_buffer) return;

    int vtx_count = static_cast<int>(mesh_in->vertex_data.size());
    if (vtx_count <= 0) return;

    const CornerLockFlags& lock =
        g_corner_locks[(mesh_in->tile_x >= 0x200 ? 1 : 0) + (mesh_in->tile_y >= 0x200 ? 2 : 0)];

    // Determine lock/wrap boundaries from the per-quadrant lock flags.
    int x_mask = 1023;
    int y_mask = 1023;
    int x_offset = 0;
    int y_offset = 0;
    if (lock.x) {
        x_mask = 511;
        x_offset = mesh_in->tile_x & 0x200;
    }
    if (lock.y) {
        y_mask = 511;
        y_offset = mesh_in->tile_y & 0x200;
    }

    // Expand each compact vertex (4 bytes = packed uint16 x,y offsets)
    // into a 40-byte vertex record with height data
    for (int i = 0; i < vtx_count; i++) {
        uint32_t packed = mesh_in->vertex_data[i];
        uint16_t rel_x = static_cast<uint16_t>(packed & 0xFFFF);
        uint16_t rel_y = static_cast<uint16_t>(packed >> 16);

        int abs_x = mesh_in->tile_x + rel_x;
        int abs_y = mesh_in->tile_y + rel_y;

        // Look up height from depth buffer with wrapping
        int buf_x = (x_offset + (abs_x & x_mask)) & 0x3FF;
        int buf_y = (y_offset + (abs_y & y_mask)) & 0x3FF;
        uint16_t height = depth_buffer[(buf_y << 10) + buf_x];

        float vertex[10];
        vertex[0] = static_cast<float>(abs_x);                    // x position
        vertex[1] = static_cast<float>(height) * 0.00390625f;     // height / 256.0
        vertex[2] = static_cast<float>(abs_y);                    // y position
        vertex[3] = 0.0f;
        // IDA: v25 = a4 & 0xFFFFFF; v27 = 0.5f; if (v25 != 0x7F7F7F) v27 = 1.0f;
        int masked_color = color_param & 0xFFFFFF;
        vertex[4] = (masked_color != 0x7F7F7F) ? 1.0f : 0.5f;
        vertex[5] = 0.0f;
        vertex[6] = static_cast<float>(abs_x) * (1.0f / 1024.0f); // texU
        vertex[7] = static_cast<float>(abs_y) * (1.0f / 1024.0f); // texV
        vertex[8] = static_cast<float>(abs_x) * 0.25f;            // quarterX
        vertex[9] = static_cast<float>(abs_y) * 0.25f;            // quarterY

        LODMeshData_SetVertex(lodmesh_out, i, vertex);
    }

    // Copy LOD section data from MeshData into LODMeshData
    // MeshData has 16 entries, LODMeshData has 16 LOD levels
    for (int lod = 0; lod < 16; lod++) {
        const auto& entry = mesh_in->entries[lod];
        auto& level = lodmesh_out->levels[lod];

        if (entry.is_strip()) {
            // Triangle strip data
            int idx_count = static_cast<int>(entry.index_data.size());
            level._field_8 = idx_count;
            level._data2 = std::malloc(idx_count * 2);
            std::memcpy(level._data2, entry.index_data.data(), idx_count * 2);
        } else {
            // Triangle list data (face triplets)
            int face_count = entry.face_count;
            level.face_count = face_count;
            level.face_index_data = static_cast<uint16_t*>(std::malloc(face_count * 6));
            std::memcpy(level.face_index_data, entry.index_data.data(), face_count * 6);
        }

        level.vertex_count = entry.max_index;

        // Store vertex count in LODMeshData's level structure
        // (offset +60 in original = levels[0].vertex_count for the first level)
    }
}

// Leaf handling from QuadtreeNode_Process (0x403470)
void build_leaf_mesh(QuadtreeNode& node, const QuadtreeContext& ctx) {
    if (!ctx.depth_buffer) return;

    // Select base mesh quadrant based on node position
    // Original: v11 = (x >> 6) & 1, v12 = (y >> 5) & 2, quadrant = v11 + v12
    int quadrant = ((node.x >> 6) & 1) + (((node.y >> 5) & 2));
    MeshData* base = get_base_mesh(quadrant);
    if (!base) return;

    // Save original tile position and set to node's position
    uint16_t saved_tile_x = base->tile_x;
    uint16_t saved_tile_y = base->tile_y;
    base->tile_x = static_cast<uint16_t>(node.x);
    base->tile_y = static_cast<uint16_t>(node.y);

    // Create temp LODMeshData, expand base mesh with height data
    LODMeshData temp;
    LODMeshData_Init(&temp);
    build_mesh_lod_vertices(base, &temp, ctx.depth_buffer, -8421505, 0);

    // Copy into node's mesh data
    LODMeshData_LoadSection(node.mesh_data.get(), &temp, 0);

    // Clean up
    LODMeshData_Free(&temp);

    // Restore base mesh tile position
    base->tile_x = saved_tile_x;
    base->tile_y = saved_tile_y;
}

// Ported from sub_402A20
// Rasterize terrain mesh triangles into 1024x1024 depth buffer.
// For each face in levels[0], computes plane equation and fills pixels within triangle.
void rasterize_terrain_depth(const QuadtreeNode& node,
                              std::vector<uint16_t>& depth_out) {
    if (depth_out.size() != 1024 * 1024) {
        depth_out.assign(1024 * 1024, 0);
    }

    auto* mesh = node.mesh_data.get();
    if (!mesh || !mesh->vertex_data) return;

    const auto& level0 = mesh->levels[0];
    if (!level0.face_index_data || level0.face_count <= 0) return;

    const uint8_t* vdata = mesh->vertex_data; // 40 bytes per vertex
    const uint16_t* fdata = level0.face_index_data;
    int face_count = level0.face_count;

    for (int fi = 0; fi < face_count; fi++) {
        // Get 3 vertex positions (x at +0, y/height at +4, z at +8 in 40-byte record)
        int vi0 = fdata[fi * 3];
        int vi1 = fdata[fi * 3 + 1];
        int vi2 = fdata[fi * 3 + 2];

        float v0x, v0y, v0z, v1x, v1y, v1z, v2x, v2y, v2z;
        std::memcpy(&v0x, vdata + 40 * vi0, 4);
        std::memcpy(&v0y, vdata + 40 * vi0 + 4, 4);
        std::memcpy(&v0z, vdata + 40 * vi0 + 8, 4);
        std::memcpy(&v1x, vdata + 40 * vi1, 4);
        std::memcpy(&v1y, vdata + 40 * vi1 + 4, 4);
        std::memcpy(&v1z, vdata + 40 * vi1 + 8, 4);
        std::memcpy(&v2x, vdata + 40 * vi2, 4);
        std::memcpy(&v2y, vdata + 40 * vi2 + 4, 4);
        std::memcpy(&v2z, vdata + 40 * vi2 + 8, 4);

        // Bounding box in x (column) and z (row), clamped to [0, 1023]
        int min_x = static_cast<int>(v0x);
        int max_x = min_x;
        { int t = static_cast<int>(v1x); if (t < min_x) min_x = t; if (t > max_x) max_x = t; }
        { int t = static_cast<int>(v2x); if (t < min_x) min_x = t; if (t > max_x) max_x = t; }

        int min_z = static_cast<int>(v0z);
        int max_z = min_z;
        { int t = static_cast<int>(v1z); if (t < min_z) min_z = t; if (t > max_z) max_z = t; }
        { int t = static_cast<int>(v2z); if (t < min_z) min_z = t; if (t > max_z) max_z = t; }

        if (min_x < 0) min_x = 0;
        if (min_z < 0) min_z = 0;
        if (max_x > 1023) max_x = 1023;
        if (max_z > 1023) max_z = 1023;

        // Edge vectors: e1 = v1 - v0, e2 = v2 - v0
        float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
        float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;

        // Cross product → normal (nx, ny, nz)
        float nx = e2z * e1y - e2y * e1z;  // IDA FPU sequence
        float ny = e2x * e1z - e2z * e1x;
        float nz = e2y * e1x - e2x * e1y;  // note: not used for height interp

        // Plane offset: d = -(nx*v0x + ny*v0y + nz*v0z)
        // Actually IDA computes: d_val = -(v0z*nz + v0y*ny + v0x*nx)
        float d_val = -(v0z * nz + v0y * ny + v0x * nx);

        // Rasterize pixels in bounding box
        for (int px = min_x; px <= max_x; px++) {
            float fpx = static_cast<float>(px);
            for (int pz = min_z; pz <= max_z; pz++) {
                float fpz = static_cast<float>(pz);

                // Height interpolation: h = -(nx*px + nz*pz + d_val) / ny
                float h = -(fpz * nz + fpx * nx + d_val) / ny;

                // Point-in-triangle test using 3 edge cross products (2D in x-z plane)
                // Edge v0→v1 cross with v0→p
                float cross1 = (fpx - v0x) * (v1z - v0z) - (fpz - v0z) * (v1x - v0x);
                if (cross1 < 0.0f) continue;

                // Edge v1→v2 cross with v1→p
                float cross2 = (fpx - v1x) * (v2z - v1z) - (fpz - v1z) * (v2x - v1x);
                if (cross2 < 0.0f) continue;

                // Edge v2→v0 cross with v2→p
                float cross3 = (v0z - v2z) * (fpx - v2x) - (v0x - v2x) * (fpz - v2z);
                if (cross3 < 0.0f) continue;

                // Height range check [0, 128.0]
                // Use !(h >= 0) instead of (h < 0) to also reject NaN.
                // Degenerate faces (ny==0) produce h=NaN; on x87, fcom NaN
                // sets C0=1 which triggers the skip. C++ (h < 0) returns false
                // for NaN, so we need the negated form to match.
                if (!(h >= 0.0f)) continue;
                if (h > 128.0f) continue;

                // Write depth: uint16(h * 256.0)
                depth_out[pz * 1024 + px] = static_cast<uint16_t>(h * 256.0);
            }
        }
    }
}

} // namespace opennova

