#pragma once

// Quadtree LOD traversal for terrain rendering.
// [orig: jodemo Terrain_TraverseQuadTreeNode @0x5C89C0, Terrain_CollectVisibleSectors @0x5C9120, Terrain_BuildHeightMipChain @0x5C5310; docs/terrain/terrain-re.md]

#include <cstdint>
#include <vector>

namespace opennova {

// ---------------------------------------------------------------------------
// The retail terrain view/cull contract
// ---------------------------------------------------------------------------

// Retail's terrain walk never reads the projection: four clip planes through
// the eye are rebuilt every frame from the horizontal FOV alone (the vertical
// half-angle is a fixed 5/6 of the horizontal one), and depth is cut by a
// mutable far scalar that defaults to 2000 and takes the frame's view
// distance when that is positive.
// [orig: sub_603DA0 @0x603DA0 (planes); PolyTrn_RenderFrame @0x60EAC0 — fov =
//  ctx[3] @0x60eaf6, far override `ctx[6] > 0` @0x60eb7e..0x60eb8d;
//  flt_8493E8 = 2000.0 static initializer]
inline constexpr float kTerrainDefaultFarDistance = 2000.0f;
// The retail camera FOV default (g_cameraFovTargetQ16 = 0x500000 = 80 deg,
// horizontal); embedders pass the live value.
inline constexpr float kTerrainDefaultFovDeg = 80.0f;
inline constexpr float kTerrainVerticalFovRatio = 0.83333331f; // @0x603dc4
inline constexpr double kTerrainDegToRad = 0.01745327777777778; // @0x603dd8

struct TerrainViewCull {
	// World-to-view, column-major, -Z forward (the Godot/GL convention; the
	// walk negates view z into retail's forward depth).
	float view[16] = {};
	// View-space planes through the eye, (x, y, depth) with d = 0, in retail
	// order: [0] (cosH, 0, sinH), [1] (-cosH, 0, sinH), [2] (0, -cosV, sinV),
	// [3] (0, cosV, sinV). [orig: flt_319FAF0 / flt_319FAE0 / flt_319FAC0 /
	//  flt_319FAD0 stores @0x603de8..0x603e4f]
	float planes[4][3] = {};
	float far_distance = kTerrainDefaultFarDistance;
};

TerrainViewCull make_terrain_view_cull(const float view[16], float fov_deg,
                                       float far_distance);

// The settings-derived LOD multiplier: Terrain_Init feeds the polygon-detail
// setting as `(detail + 1) * 0.25`, the setter clamps it to [0, 1] and remaps
// it to `x * 0.8 + 0.2`, and each frame multiplies the context's quality
// scale by that. Detail 3 (the max-quality target) yields 1.0.
// [orig: Terrain_Init @0x60fc33 -> sub_605D70 @0x605D70 (clamp, the
//  `* 0.80000001 + 0.2` store to flt_8493D8); PolyTrn_RenderFrame @0x60eb4a
//  `flt_319FB2C = ctx[5] * flt_8493D8`]
inline constexpr int kTerrainMaxPolygonDetail = 3;
float terrain_lod_quality_scale(float context_scale, int polygon_detail);

// ---------------------------------------------------------------------------
// Mipchain (hierarchical height min/max)
// ---------------------------------------------------------------------------

struct Mipchain {
	std::vector<uint8_t> data;
	uint8_t* levels[16] = {};
	int level_count = 0;
};

Mipchain build_mipchain(const std::vector<uint16_t>& heightmap);

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
	int tile_index = -1;
	int lod_sub = 0;
	int lod_level = 0;
	float distance = 0.0f;
	float sector_ox = 0.0f, sector_oz = 0.0f;
	bool zero_height = false;
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
	int rej_nearfar = 0;
	int rej_left = 0, rej_right = 0, rej_bottom = 0, rej_top = 0;
	int partial_subdiv_count = 0;
	int budget_drops = 0;
	float dist_min = 1e9f;
	float dist_max = 0.0f;
	int lod_fallbacks = 0;
};

// Select one of the eight terrain mesh families from the recovered 0..15 LOD
// sublevel: family = family_count * lod_sub / 16 over the tile's eight.
// [orig: Terrain_GetLodSlotFamily @ 0x60288E..0x6028B1, called from
// render_terrain_sector_batch @ 0x609581]
int terrain_lod_family(int lod_sub) noexcept;

// Distance from point to AABB (used for LOD selection).
float node_distance(const float aabb_min[3], const float aabb_max[3],
                    const float center[3], float px, float py, float pz);

// Recursive quadtree traversal with the retail view cull and LOD. The optional
// foliage handoff keeps every eligible emitted node independently of the
// 224-entry terrain draw cap; the cell collector applies its own 128 cap.
// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, main cap @ 0x608FBC,
// foliage handoff @ 0x60905C..0x60907C]
void traverse_quadtree(const std::vector<QuadNode>& quad_nodes,
                       const std::vector<TileMesh>& tile_meshes,
                       int node_idx,
                       const TerrainViewCull& cull,
                       float cam_x, float cam_y, float cam_z,
                       float sector_ox, float sector_oz,
                       const TraversalConfig& config,
                       std::vector<VisiblePatch>& out_patches,
                       TraversalStats& stats,
                       bool zero_height = false,
                       std::vector<VisiblePatch>* out_foliage_handoffs = nullptr);

// The frustum-surviving world AABB of one sector's quadtree: retail's
// trackBounds traversal ignores the distance emit heuristic, subdivides every
// surviving node to the LOD cap, and accumulates the terminal nodes' bounds
// into one running min/max (the water-active test reads its height range).
// [orig: Terrain_TraverseQuadtreeNode @ 0x608a00 — the `!trackBounds` emit
//  gate @ 0x608d84, the accumulation @ 0x608ddf..0x608e8c]
struct VisibleBounds {
	bool valid = false;
	float min[3] = {0.0f, 0.0f, 0.0f};
	float max[3] = {0.0f, 0.0f, 0.0f};
	void include(const float wmin[3], const float wmax[3]);
};

// Retail's per-frame g_WaterActive: the water pass (reflection prerender,
// noise pair, strip march) runs only while the lowest tracked visible terrain
// sits at or below the water height, or the previous frame's Blink walk saw
// the water; untracked bounds keep the pass live.
// [orig: terrain_setup_view_and_lighting @ 0x60fe40 (compare
//  @ 0x60ff12..0x60ff1a, the Blink force @ 0x60ff31); the tracked bounds
//  come from terrain_render_visible_sectors @ 0x6090c0 with trackBounds = 1
//  @ 0x609263]
inline bool water_pass_active(bool bounds_valid, float min_height,
                              float max_height, float water_height,
                              bool blink_water_visible) {
	return !bounds_valid || blink_water_visible || min_height <= water_height ||
	       max_height <= water_height;
}

void track_visible_bounds(const std::vector<QuadNode>& quad_nodes,
                          int node_idx,
                          const TerrainViewCull& cull,
                          float sector_ox, float sector_oz,
                          const TraversalConfig& config,
                          VisibleBounds& out_bounds,
                          bool zero_height = false);

// Convert triangle strip to triangle list.
void strip_to_list(const std::vector<uint16_t>& strip,
                   std::vector<uint32_t>& out, uint32_t base);

} // namespace opennova
