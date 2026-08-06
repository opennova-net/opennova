#pragma once

#include "build_quadtree.h"
#include "lod.h"
#include "terrain/mesh_data.h"
#include <cstdint>
#include <vector>

namespace opennova {

// Ported from generate_base_terrain_meshes (0x402D20).
// Creates 4 base mesh quadrants with 65×65 vertex grids and edge stitching.
void generate_base_terrain_meshes();

// Get a base mesh quadrant (0-3). Returns nullptr if not generated.
MeshData* get_base_mesh(int quadrant);

// Configure the per-quadrant lock flags consumed by build_mesh_lod_vertices.
void set_mesh_corner_locks(int tl_x, int tl_y, int tr_x, int tr_y,
                           int bl_x, int bl_y, int br_x, int br_y);

// Ported from build_mesh_lod_vertices (0x4042B0).
// Expands a compact MeshData into a LODMeshData with height data from depth buffer.
// mesh_data_in: compact MeshData (vertices are packed uint16 x,y offsets)
// lodmesh_out: LODMeshData to fill with expanded vertex data + face indices
// depth_buffer: smoothed 1024×1024 uint16 heightmap
// color_param: usually 0xFF7F7F7F (-8421505)
// extra_param: usually 0
void build_mesh_lod_vertices(MeshData* mesh_data_in, LODMeshData* lodmesh_out,
                             const uint16_t* depth_buffer, int color_param, int extra_param);

// Build leaf mesh: selects base mesh quadrant, expands with height data
void build_leaf_mesh(QuadtreeNode& node, const QuadtreeContext& ctx);

// Rasterize terrain mesh triangles into a 1024x1024 depth buffer.
void rasterize_terrain_depth(const QuadtreeNode& node,
                              std::vector<uint16_t>& depth_out);

} // namespace opennova

