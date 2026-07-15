#pragma once

// Quadtree LOD traversal for terrain rendering.
// Engine: jodemo.exe Terrain_TraverseQuadTreeNode@0x5C89C0,
// Terrain_CollectVisibleSectors@0x5C9120, Terrain_BuildHeightMipChain@0x5C5310
// docs/engine_spec_terrain.md 5.3, 7.1

#include <cstdint>
#include <vector>

namespace opennova {

// ---------------------------------------------------------------------------
// Frustum
// ---------------------------------------------------------------------------

struct Frustum {
	float planes[6][4]; // left, right, bottom, top, near, far
	enum { P_LEFT = 0, P_RIGHT, P_BOTTOM, P_TOP, P_NEAR, P_FAR };
};

// Extract frustum planes from a column-major 4x4 MVP matrix (Gribb/Hartmann method).
Frustum extract_frustum(const float mvp[16]);

// Test if AABB is completely outside a single plane.
bool aabb_outside_plane(const float plane[4],
                        const float aabb_min[3], const float aabb_max[3]);

// ---------------------------------------------------------------------------
// Mipchain (hierarchical height min/max)
// ---------------------------------------------------------------------------

struct Mipchain {
	std::vector<uint8_t> data;
	uint8_t* levels[16] = {};
	int level_count = 0;
};

Mipchain build_mipchain(const std::vector<uint16_t>& heightmap, int atlas_size);

// ---------------------------------------------------------------------------
// Quadtree nodes
// ---------------------------------------------------------------------------

struct TileLOD {
	int index_count = 0;
};

struct TileMesh {
	int vertex_count = 0;
	TileLOD lods[8];
	float aabb_min[3], aabb_max[3];
	float center[3], radius;
	uint16_t tile_size = 0;
};

struct QuadNode {
	bool is_leaf = false;
	int lod_level = 0;       // 0=root(1024), 4=leaf(64)
	int size = 0;
	float aabb_min[3], aabb_max[3];
	float center[3], radius;
	int tile_index = -1;     // into tile_meshes, -1 if none
	int children[4] = {-1, -1, -1, -1};
};

struct VisiblePatch {
	int tile_index;
	int lod_sub;
	int lod_level;
	float distance;
	float sector_ox, sector_oz;
};

struct TraversalConfig {
	bool no_frustum = false;
	bool no_nearfar = false;
	bool no_sideplanes = false;
	bool no_partial_subdiv = false;
	bool force_leaves = false;
	bool force_lod0 = false;
	float quality = 1.0f;
};

struct TraversalStats {
	int nodes_visited = 0;
	int rej_nearfar = 0;
	int rej_left = 0, rej_right = 0, rej_bottom = 0, rej_top = 0;
	int partial_subdiv_count = 0;
	int budget_drops = 0;
	int leaf_emits = 0;
	int nonleaf_emits = 0;
	int partial_subdiv_per_level[5] = {};
	float dist_min = 1e9f;
	float dist_max = 0.0f;
	int lod_fallbacks = 0;
};

// Select one of the eight terrain mesh families from the recovered 0..15 LOD
// sublevel. [orig: render_terrain_sector_batch @ 0x6096f0]
int terrain_lod_family(int lod_sub) noexcept;

// Distance from point to AABB (used for LOD selection).
float node_distance(const float aabb_min[3], const float aabb_max[3],
                    const float center[3], float px, float py, float pz);

// Recursive quadtree traversal with frustum culling and LOD.
void traverse_quadtree(const std::vector<QuadNode>& quad_nodes,
                       const std::vector<TileMesh>& tile_meshes,
                       int node_idx,
                       const Frustum& frustum,
                       float cam_x, float cam_y, float cam_z,
                       float sector_ox, float sector_oz,
                       const TraversalConfig& config,
                       std::vector<VisiblePatch>& out_patches,
                       TraversalStats& stats);

// Convert triangle strip to triangle list.
void strip_to_list(const std::vector<uint16_t>& strip,
                   std::vector<uint32_t>& out, uint32_t base);

} // namespace opennova
