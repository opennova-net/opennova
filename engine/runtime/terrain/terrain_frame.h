#pragma once

// The terrain render frame as engine data (ADR 0033 R2). One compile turns the
// built terrain scene plus a camera into the ordered patch-draw list an
// embedding renderer uploads — the snapshot-and-view-in, typed-draw list-out seam
// renderer::ParticleFrameCompiler proved. The compiler owns every per-frame
// DECISION the shell adapter used to make in its self-driven walk: the
// 512-unit sector window over the .trn sector grid, the quadtree traversal,
// the foliage detail-cell handoff, the front-to-back order, the patch budget,
// and the LOD-family resolve. The embedder keeps only device work: building
// GPU meshes from the same CPT tiles at load time and writing the draw list onto
// its instance pool.
// [orig: Terrain_CollectVisibleSectors @ 0x5C9120 — the 512-unit sector
//  window feeding Terrain_TraverseQuadTreeNode @ 0x5C89C0;
//  render_terrain_sector_batch @ 0x6096f0 — the per-batch family select]

#include <terrain/foliage_detail_collector.h>
#include <terrain/quadtree.h>

#include <cstdint>
#include <vector>

namespace opennova {

struct CptFile;
struct TrnConfig;

// Per-tile draw constants derived once at snapshot build. The quadrant bits
// feed the shader's source-quadrant select; deriving them here removes the
// per-patch CPT tile re-index from the hot submission loop.
struct TerrainTileDrawAttributes {
	uint8_t quadrant_x = 0; // (tile_x >> 9) & 1
	uint8_t quadrant_z = 0; // (tile_y >> 9) & 1
};

// Everything the per-frame walk reads, built once per loaded terrain. TileMesh
// carries the traversal AABB/center/radius and the per-LOD index counts (after
// degenerate-strip removal), so mesh availability — and therefore the family
// fallback — is decided here, not against the embedder's GPU resources.
struct TerrainSceneSnapshot {
	std::vector<QuadNode> quad_nodes;
	std::vector<TileMesh> tile_meshes;
	std::vector<TerrainTileDrawAttributes> tile_attributes;
	Mipchain mipchain;
	int root_node = -1;
	int l1_children[4] = {-1, -1, -1, -1};
	// .trn sector routing: grid ids 0..4, authored origin, per-axis wrap.
	int sector_grid[16][16] = {};
	int origin_x = 0;
	int origin_y = 0;
	bool wrap_x = false;
	bool wrap_y = false;

	bool valid() const { return root_node >= 0 && !quad_nodes.empty(); }
};

// Build the scene snapshot from the parsed documents. Returns an invalid
// snapshot (root_node -1) when the CPT carries no tiles or a depth buffer
// that is not the 1024x1024 atlas.
TerrainSceneSnapshot build_terrain_scene_snapshot(const CptFile &cpt,
                                                  const TrnConfig &trn);

// Orthonormal camera state in world space plus the projection, both
// column-major. The compiler multiplies proj*view and extracts the frustum
// itself so the embedder never owns culling math.
struct TerrainViewInput {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float cam_z = 0.0f;
	float view[16] = {};
	float proj[16] = {};
	TraversalConfig config{};
};

// One patch submission: which tile, which resolved mesh family, where. The
// draw-list order is the draw order (front-to-back by node distance) and the
// draw-list index is the embedder's pool slot.
struct TerrainPatchDraw {
	int32_t tile_index = -1;
	int32_t lod_family = 0;
	float sector_ox = 0.0f;
	float sector_oz = 0.0f;
	float distance = 0.0f;
	uint8_t quadrant_x = 0;
	uint8_t quadrant_z = 0;
};

// Value-based diagnostics for F3 and structural tests (no Dictionary at the
// seam). lod_distribution is over emitted patches with the fallback resolved;
// visible_patches counts pre-budget traversal emissions.
struct TerrainFrameDebugCounters {
	uint64_t compile_index = 0;
	TraversalStats traversal{};
	int lod_distribution[8] = {};
	int sectors_walked = 0;
	int visible_patches = 0;
	int emitted_patches = 0;
	int empty_mesh_drops = 0;
};

struct TerrainDrawList {
	uint64_t frame_id = 0;
	std::vector<TerrainPatchDraw> patches;
	// The frustum-surviving 16-unit foliage detail cells handed off by the
	// traversal — the input the foliage frame leg consumes this same frame.
	std::vector<FoliageDetailPatch> detail_cells;
	TerrainFrameDebugCounters debug{};
};

// Deep in-process module: one call windows the sector grid, traverses each
// routed sector's quadtree, collects the foliage handoff, orders the visible
// patches front-to-back, applies the pool budget, and resolves the mesh
// family per patch. The returned draw list remains valid until the next compile
// call; retained vectors make no-allocation-after-warmup observable.
class TerrainFrameCompiler {
public:
	// The embedder-side instance-pool budget the draw list is truncated to (the
	// traversal's own per-walk emission cap sits below it in quadtree.cpp).
	static constexpr int kPatchBudget = 256;

	const TerrainDrawList &compile(const TerrainSceneSnapshot &scene,
	                                 const TerrainViewInput &view);

	// The last compiled draw list (empty before the first compile) — the cold
	// read for stats surfaces that outlive a frame.
	const TerrainDrawList &last_draw_list() const { return draw_list_; }

private:
	TerrainDrawList draw_list_;
	std::vector<VisiblePatch> visible_;
	uint64_t compile_index_ = 0;
};

} // namespace opennova
