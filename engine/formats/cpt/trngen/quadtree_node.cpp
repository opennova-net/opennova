#include "quadtree_node.h"
#include "mesh_simp.h"
#include "terrain_mesh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace opennova::trngen {

// Float constants from IDA (QuadtreeNode_Process at 0x403470)
// [orig: TrnGen.exe QuadtreeNode_Process @ 0x403470; docs/terrain/terrain-re.md]
static constexpr float COST_SCALE_A  = 0.0005f;   // flt_4244D0
static constexpr float COST_OFFSET_LEAF  = 0.06f;  // flt_4244CC
static constexpr float COST_OFFSET_BRANCH = 0.15f; // flt_4244C8 (0x3e19999a)
static constexpr float EDGE_COST_MULT = 0.001f;    // flt_4244C4
static constexpr float EDGE_COST_ADD  = 0.30f;     // flt_4244C0
static constexpr float FINAL_SCALE    = 0.80f;     // flt_4244B0
// dbl_4244B8 = 0.8 (double) — used for v28 * 0.8 clamp
// flt_42A460 = curvature weight (g_curvature_weight), set to 2.0 during node processing

QuadtreeNode::~QuadtreeNode() { free_node_mesh(mesh_data); }

void free_node_mesh(std::unique_ptr<LODMeshData>& mesh) {
    if (mesh) LODMeshData_Free(mesh.get());
    mesh.reset();
}

// [orig: TrnGen.exe QuadtreeNode_InitRecursive @ 0x402520]
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

// [orig: TrnGen.exe QuadtreeNode_InitRoot @ 0x4023D0]
std::unique_ptr<QuadtreeNode> build_quadtree(int tile_size, int min_tile_size) {
    auto root = std::make_unique<QuadtreeNode>();
    init_recursive(*root, tile_size, min_tile_size, 0, 0, 0);
    return root;
}

// What QuadtreeNode_Process does for one node once its children are done: the node's mesh (a
// leaf's from its base mesh and the depth map, a branch's from its children's LOD 14 sections),
// simplified into its LOD levels, its tile file made and its children's meshes freed. False, with
// `error`, when a buffer of the node's mesh was refused (LODMeshData::failure).
// [orig: TrnGen.exe QuadtreeNode_Process @ 0x403470]
static bool make_node_mesh(QuadtreeNode& node, const QuadtreeContext& ctx, std::string& error) {
    // Allocate LOD mesh data for this node
    node.mesh_data = std::make_unique<LODMeshData>();
    LODMeshData_Init(node.mesh_data.get());

    // TrnGen first tries the node's tile file from an earlier run (process_quadtree_leaf /
    // sub_402730); a bake here starts with no tile files (TileFiles), which is that function's
    // miss path: every node is made from the base meshes and its children.
    {
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
    if (!node.mesh_data->failure.empty()) {
        error = node.mesh_data->failure;
        return false;
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
    node.tile_name = filename;
    node.tile_bytes = compact.write_bytes();

    // Free children's mesh data after processing (save memory)
    if (!node.is_leaf) {
        if (node.child_nw) free_node_mesh(node.child_nw->mesh_data);
        if (node.child_ne) free_node_mesh(node.child_ne->mesh_data);
        if (node.child_sw) free_node_mesh(node.child_sw->mesh_data);
        if (node.child_se) free_node_mesh(node.child_se->mesh_data);
    }
    return true;
}

// The walk's order: children (nw, ne, sw, se) before their parent, each node listed under its depth.
static void collect_nodes(QuadtreeNode& node, std::vector<std::vector<QuadtreeNode*>>& by_depth) {
    if (!node.is_leaf) {
        if (node.child_nw) collect_nodes(*node.child_nw, by_depth);
        if (node.child_ne) collect_nodes(*node.child_ne, by_depth);
        if (node.child_sw) collect_nodes(*node.child_sw, by_depth);
        if (node.child_se) collect_nodes(*node.child_se, by_depth);
    }
    if (by_depth.size() <= static_cast<size_t>(node.depth)) by_depth.resize(node.depth + 1);
    by_depth[node.depth].push_back(&node);
}

// The nodes of one level made on up to `threads` threads; false, with the first failure's words,
// once all stop.
static bool make_level(const std::vector<QuadtreeNode*>& nodes, const QuadtreeContext& ctx, unsigned threads,
                       std::string& error) {
    std::atomic<size_t> next{0};
    bool failed = false;
    std::string failure;
    std::mutex failure_lock;
    const auto work = [&] {
        for (size_t i = next++; i < nodes.size(); i = next++) {
            std::string why;
            if (!make_node_mesh(*nodes[i], ctx, why)) {
                std::lock_guard<std::mutex> hold(failure_lock);
                if (!failed) {
                    failed = true;
                    failure = why;
                }
            }
        }
        MeshSimp_FreeAll();
    };
    const unsigned count = static_cast<unsigned>(std::min<size_t>(std::max(threads, 1u), nodes.size()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < count; ++t) pool.emplace_back(work);
    work();
    for (std::thread& each : pool) each.join();
    if (failed) error = failure;
    return !failed;
}

bool process_quadtree(QuadtreeNode* root, QuadtreeContext& ctx, unsigned threads, std::string& error) {
    std::vector<std::vector<QuadtreeNode*>> by_depth;
    collect_nodes(*root, by_depth);
    ctx.total_blocks = 0;
    for (const auto& level : by_depth) ctx.total_blocks += static_cast<int>(level.size());
    ctx.completed_blocks = 0;

    for (size_t depth = by_depth.size(); depth-- > 0;) {
        const std::vector<QuadtreeNode*>& nodes = by_depth[depth];
        if (!make_level(nodes, ctx, threads, error)) return false;
        for (QuadtreeNode* node : nodes) {
            (*ctx.files)[node->tile_name] = std::move(node->tile_bytes);
            node->tile_bytes.clear();
            // Rasterize depth for leaf nodes (IDA line 165-166: if is_leaf, call sub_402A20)
            if (node->is_leaf && ctx.rasterized_depth) rasterize_terrain_depth(*node, *ctx.rasterized_depth);
            ++ctx.completed_blocks;
        }
    }
    return true;
}

} // namespace opennova::trngen

