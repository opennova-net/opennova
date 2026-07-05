#include "build_quadtree.h"
#include "terrain_mesh.h"
#include "trace.h"

#include <cstdio>
#include <cmath>

namespace opennova {

// Static counter for node trace IDs (matches hook's g_node_id)
static int s_node_id = 0;

// Float constants from IDA (QuadtreeNode_Process at 0x403470)
// [orig: QuadtreeNode_Process @ 0x403470; docs/terrain/terrain-re.md]
static constexpr float COST_SCALE_A  = 0.0005f;   // flt_4244D0
static constexpr float COST_OFFSET_LEAF  = 0.06f;  // flt_4244CC
static constexpr float COST_OFFSET_BRANCH = 0.15f; // flt_4244C8 (0x3e19999a)
static constexpr float EDGE_COST_MULT = 0.001f;    // flt_4244C4
static constexpr float EDGE_COST_ADD  = 0.30f;     // flt_4244C0
static constexpr float FINAL_SCALE    = 0.80f;     // flt_4244B0
// dbl_4244B8 = 0.8 (double) — used for v28 * 0.8 clamp
// flt_42A460 = curvature weight, set to 2.0 during node processing

extern float g_curvature_weight; // flt_42A460

// Ported from QuadtreeNode_InitRecursive (0x402520)
static void init_recursive(QuadtreeNode& node, int size, int min_size,
                           int x, int y, int depth) {
    node.x = x;
    node.y = y;
    node.size = size;
    node.depth = depth;

    if (size <= min_size) {
        node.is_leaf = true;
        return;
    }

    int half = size / 2;
    int next_depth = depth + 1;

    node.child_nw = std::make_unique<QuadtreeNode>();
    init_recursive(*node.child_nw, half, min_size, x, y, next_depth);

    node.child_ne = std::make_unique<QuadtreeNode>();
    init_recursive(*node.child_ne, half, min_size, x + half, y, next_depth);

    node.child_sw = std::make_unique<QuadtreeNode>();
    init_recursive(*node.child_sw, half, min_size, x, y + half, next_depth);

    node.child_se = std::make_unique<QuadtreeNode>();
    init_recursive(*node.child_se, half, min_size, x + half, y + half, next_depth);
}

// Ported from QuadtreeNode_InitRoot (0x4023D0)
std::unique_ptr<QuadtreeNode> build_quadtree(int tile_size, int min_tile_size) {
    auto root = std::make_unique<QuadtreeNode>();
    init_recursive(*root, tile_size, min_tile_size, 0, 0, 0);
    return root;
}

// Ported faithfully from QuadtreeNode_Process (0x403470)
static void process_node(QuadtreeNode& node, QuadtreeContext& ctx) {
    ++s_node_id;
    trace_log("{\"event\":\"node\",\"id\":%d,\"is_leaf\":%d,\"depth\":%d,"
              "\"x\":%d,\"y\":%d,\"size\":%d}",
              s_node_id, node.is_leaf ? 1 : 0, node.depth,
              node.x, node.y, node.size);

    if (!node.is_leaf) {
        if (node.child_nw) process_node(*node.child_nw, ctx);
        if (node.child_ne) process_node(*node.child_ne, ctx);
        if (node.child_sw) process_node(*node.child_sw, ctx);
        if (node.child_se) process_node(*node.child_se, ctx);
    }

    // Allocate LOD mesh data for this node
    node.mesh_data = std::make_unique<LODMeshData>();
    LODMeshData_Init(node.mesh_data.get());

    // Try to load from file (process_quadtree_leaf / sub_402730)
    // On first build, no tiles exist, so we generate from base meshes
    bool loaded_from_file = false;
    // TODO: implement file loading for incremental builds

    if (!loaded_from_file) {
        if (node.is_leaf) {
            // Leaf node: expand base mesh with height data
            // Ported from the leaf handling in QuadtreeNode_Process
            build_leaf_mesh(node, ctx);
        } else {
            // Branch node: merge children's LOD level 14 data (sub_409FA0)
            // IDA line 82: all 4 children pass section_index=14
            if (node.child_nw && node.child_nw->mesh_data)
                LODMeshData_LoadSectionDedup(node.mesh_data.get(), node.child_nw->mesh_data.get(), 14);
            if (node.child_ne && node.child_ne->mesh_data)
                LODMeshData_LoadSectionDedup(node.mesh_data.get(), node.child_ne->mesh_data.get(), 14);
            if (node.child_sw && node.child_sw->mesh_data)
                LODMeshData_LoadSectionDedup(node.mesh_data.get(), node.child_sw->mesh_data.get(), 14);
            if (node.child_se && node.child_se->mesh_data)
                LODMeshData_LoadSectionDedup(node.mesh_data.get(), node.child_se->mesh_data.get(), 14);
        }
    }

    // Compute LOD parameters (ported from QuadtreeNode_Process lines 88-141)
    int tile_size = node.size;
    int v26 = 2 * tile_size; // doubled tile size for parameter computation
    int max_verts = 5 * tile_size + ((tile_size * tile_size) >> 8);

    // Compute cost scales from float(v26)
    // IDA: fild v26, fst v26_float, fmul flt_4244D0 (0.0005), fadd flt_4244CC/C8
    float v26_float = static_cast<float>(v26);

    int v17; // used for lod_start computation
    int v16; // used for min_verts target
    float vertex_cost_scale;

    if (node.is_leaf) {
        // Leaf: v17 = v28 = max_verts (NOT actual vertex count!)
        // IDA lines 99-101: v16 = max_verts; v17 = v16; v28 = v16;
        v17 = max_verts;
        v16 = max_verts;
        vertex_cost_scale = v26_float * COST_SCALE_A + COST_OFFSET_LEAF;
    } else {
        // Branch: v17 = v28 = actual vertex count from merged mesh data
        // IDA line 112: v17 = **(_DWORD **)(this + 36) = vertex_count
        v17 = node.mesh_data->vertex_count;
        v16 = max_verts;
        vertex_cost_scale = v26_float * COST_SCALE_A + COST_OFFSET_BRANCH;
    }

    // Edge cost scale: separate computation from vertex cost scale
    // IDA lines 118-120: fld v26_float, fmul flt_4244C4 (0.001), fadd flt_4244C0 (0.3)
    float edge_cost_scale = v26_float * EDGE_COST_MULT + EDGE_COST_ADD;

    // Clamp v16 to int(v17 * 0.8)
    // IDA lines 121-126: fild v28, fmul dbl_4244B8 (0.8), ftol
    float max_fraction = static_cast<float>(v17) * 0.8; // uses double 0.8 in IDA (dbl_4244B8)
    int max_from_fraction = static_cast<int>(max_fraction);
    if (v16 > max_from_fraction) {
        v16 = max_from_fraction;
    }

    // Set curvature weight for this node's LOD generation
    // IDA line 137: flt_42A460 = 2.0
    g_curvature_weight = 2.0f;

    // Both cost scales get multiplied by 0.8 before passing to LOD generation
    // IDA lines 136,139: fmul flt_4244B0 (0.8) applied to both
    // IDA line 141: sub_409950(mesh, v17-((v17-v16)>>4), v16, vertex_cost*0.8, edge_cost*0.8)
    int lod_start = v17 - ((v17 - v16) >> 4);

    LODMeshData_GenerateLODParams(
        node.mesh_data.get(),
        lod_start,
        v16,
        vertex_cost_scale * FINAL_SCALE,
        edge_cost_scale * FINAL_SCALE);

    // Write tile file
    // sub_402920: creates compact mesh, writes to file
    char filename[512];
    std::snprintf(filename, sizeof(filename), "%sS%d_%02x_%02x.tml",
                  ctx.output_prefix.c_str(), node.depth, node.x >> 4, node.y >> 4);

    auto compact = LODMeshData_CreateCompact(node.mesh_data.get());
    compact.tile_x = static_cast<uint16_t>(node.x);
    compact.tile_y = static_cast<uint16_t>(node.y);
    compact.write(filename);

    // Rasterize depth for leaf nodes (IDA line 165-166: if is_leaf, call sub_402A20)
    if (node.is_leaf && ctx.rasterized_depth) {
        rasterize_terrain_depth(node, *ctx.rasterized_depth);
    }

    // Report progress
    if (ctx.progress_callback) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "Generated tile depth=%d %02x/%02x",
                      node.depth, node.x >> 4, node.y >> 4);
        ++ctx.completed_blocks;
        ctx.progress_callback(ctx.completed_blocks, ctx.total_blocks, msg);
    }

    // Free children's mesh data after processing (save memory)
    if (!node.is_leaf) {
        if (node.child_nw) node.child_nw->mesh_data.reset();
        if (node.child_ne) node.child_ne->mesh_data.reset();
        if (node.child_sw) node.child_sw->mesh_data.reset();
        if (node.child_se) node.child_se->mesh_data.reset();
    }
}

// Wrapper for process_node (no direct IDA counterpart)
void process_quadtree(QuadtreeNode* root, QuadtreeContext& ctx) {
    int count = 0;
    int tile = ctx.tile_size;
    int mult = 1;
    while (tile >= ctx.min_tile_size) {
        count += mult;
        tile >>= 1;
        mult *= 4;
    }
    ctx.total_blocks = count;
    ctx.completed_blocks = 0;

    process_node(*root, ctx);
}

} // namespace opennova

