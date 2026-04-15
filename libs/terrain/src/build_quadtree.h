#pragma once

#include "lod.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace opennova {

// Ported from QuadtreeNode struct (0x28 = 40 bytes) and related functions.
// Manages recursive terrain subdivision for multi-resolution mesh generation.

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
};

struct QuadtreeContext {
    int tile_size = 0;        // root tile size
    int min_tile_size = 0;    // leaf size threshold
    int total_blocks = 0;     // total block count
    int completed_blocks = 0; // processed block count

    const uint16_t* depth_buffer = nullptr;  // smoothed 1024x1024 heightmap (read-only, for vertex expansion)
    std::vector<uint16_t>* rasterized_depth = nullptr; // written by rasterizer for leaf nodes
    std::string output_prefix;               // output path prefix

    // Progress callback (block_index, total_blocks, status_message)
    std::function<void(int, int, const std::string&)> progress_callback;
};

// Build a quadtree from tile_size down to min_tile_size
std::unique_ptr<QuadtreeNode> build_quadtree(int tile_size, int min_tile_size);

// Process the quadtree: generate meshes, LODs, write tile files
void process_quadtree(QuadtreeNode* root, QuadtreeContext& ctx);

} // namespace opennova

