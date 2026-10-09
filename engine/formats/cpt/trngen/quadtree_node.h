#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lod_mesh_data.h"
#include "mesh_data.h"

namespace opennova::trngen {

// TrnGen.exe's quadtree bake (QuadtreeNode, 0x28 bytes, and its walk): the terrain cut from the
// 1024 root down to the 64 leaves, every node's mesh simplified into its LOD levels and written as
// its tile file.

struct QuadtreeNode {
    bool is_leaf = false;
    int depth = 0;
    int x = 0;
    int y = 0;
    int size = 0;

    std::unique_ptr<QuadtreeNode> child_nw;
    std::unique_ptr<QuadtreeNode> child_ne;
    std::unique_ptr<QuadtreeNode> child_sw;
    std::unique_ptr<QuadtreeNode> child_se;

    std::unique_ptr<LODMeshData> mesh_data;
    // The node's tile file, made with its mesh, until the walk files it.
    std::string tile_name;
    std::vector<uint8_t> tile_bytes;

    ~QuadtreeNode();
};

// The output directory TrnGen.exe writes its tile files into, kept in memory: a file name to its
// bytes (`<prefix>S<depth>_<xx>_<yy>.tml`). A bake starts with it empty, so no tile is taken from
// an earlier run (TrnGen trusts tiles already on disk as a cache; the port reads none).
using TileFiles = std::map<std::string, std::vector<uint8_t>>;

// One quadrant's lock flags (the .tpj's lock_* pairs): a locked axis keeps a leaf's height taps
// inside its own 512 quadrant.
struct CornerLockFlags {
    int x = 0;
    int y = 0;
};

// The base meshes (dword_42E274[4]) and the lock flags the leaves are made with: TrnGen.exe's
// globals, one per bake here.
struct BaseMeshSet {
    std::array<MeshData, 4> meshes;
    bool generated = false;
    std::array<CornerLockFlags, 4> locks{}; // top-left, top-right, bottom-left, bottom-right
};

struct QuadtreeContext {
    int tile_size = 0;        // root tile size
    int min_tile_size = 0;    // leaf size threshold
    int total_blocks = 0;     // total block count
    int completed_blocks = 0; // processed block count

    const uint16_t* depth_buffer = nullptr;  // smoothed 1024x1024 heightmap (read-only, for vertex expansion)
    std::vector<uint16_t>* rasterized_depth = nullptr; // written by rasterizer for leaf nodes
    std::string output_prefix;               // the tile files' name prefix
    BaseMeshSet* base = nullptr;
    TileFiles* files = nullptr;
};

// Frees a node's LOD mesh (its buffers, then the mesh).
void free_node_mesh(std::unique_ptr<LODMeshData>& mesh);

// Build a quadtree from tile_size down to min_tile_size
std::unique_ptr<QuadtreeNode> build_quadtree(int tile_size, int min_tile_size);

// Process the quadtree: generate meshes, LODs, write tile files. TrnGen walks it depth first,
// children before their parent; a node reads only its own children and the depth map, so here the
// nodes of one level are made side by side on up to `threads` threads (each thread's simplifier
// state its own), the levels from the leaves up, and what the walk orders (the tile files, the
// leaves rasterized over the depth atlas, which overlap on their shared edges) is done in the
// walk's order: the same files and atlas as the walk. False, with `error`, for the first node whose
// mesh a refused buffer stopped (LODMeshData::failure); the level's other nodes are made first.
bool process_quadtree(QuadtreeNode* root, QuadtreeContext& ctx, unsigned threads, std::string& error);

} // namespace opennova::trngen
