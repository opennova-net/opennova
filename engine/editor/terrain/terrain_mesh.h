#pragma once

#include <cstdint>
#include <vector>

#include "lod_mesh_data.h"
#include "mesh_data.h"
#include "quadtree_node.h"

namespace opennova::editor::trngen {

// [orig: TrnGen.exe generate_base_terrain_meshes @ 0x402D20]
// Creates the 4 base mesh quadrants with 65×65 vertex grids and edge stitching, into `base`.
void generate_base_terrain_meshes(BaseMeshSet& base);

// [orig: TrnGen.exe build_mesh_lod_vertices @ 0x4042B0]
// Expands a compact MeshData into a LODMeshData with height data from depth buffer.
// mesh_data_in: compact MeshData (vertices are packed uint16 x,y offsets)
// tile_x, tile_y: the tile origin the vertices are offsets from (TrnGen sets the base mesh's own
//   for the call and puts it back; here the base mesh is never written)
// lodmesh_out: LODMeshData to fill with expanded vertex data + face indices
// depth_buffer: smoothed 1024×1024 uint16 heightmap
// locks: the quadrant lock flags, chosen by the tile's quadrant
// color_param: usually 0xFF7F7F7F (-8421505)
// extra_param: usually 0
void build_mesh_lod_vertices(const MeshData* mesh_data_in, uint16_t tile_x, uint16_t tile_y,
                             LODMeshData* lodmesh_out,
                             const uint16_t* depth_buffer, const std::array<CornerLockFlags, 4>& locks,
                             int color_param, int extra_param);

// Build leaf mesh: selects base mesh quadrant, expands with height data
void build_leaf_mesh(QuadtreeNode& node, const QuadtreeContext& ctx);

// [orig: TrnGen.exe sub_402A20 @ 0x402A20]
// Rasterize terrain mesh triangles into a 1024x1024 depth buffer.
void rasterize_terrain_depth(const QuadtreeNode& node,
                              std::vector<uint16_t>& depth_out);

} // namespace opennova::editor::trngen
