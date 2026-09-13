#pragma once

// The terrain render frame as engine data (ADR 0033 R2). One compile turns the
// built terrain scene plus a camera into the ordered patch-draw list an
// embedding renderer uploads — the snapshot-and-view-in, typed-draw list-out seam
// renderer::ParticleFrameCompiler proved. The compiler owns every per-frame
// DECISION the shell binding used to make in its self-driven walk: the
// 512-unit sector window over the .trn sector grid, the quadtree traversal,
// the foliage detail-cell handoff, the front-to-back order, the patch budget,
// and the LOD-family resolve. The embedder keeps only device work: building
// GPU meshes from the same CPT tiles at load time and writing the draw list onto
// its instance pool.
// [orig: Terrain_CollectVisibleSectors @ 0x5C9120 (jodemo.exe) — the 512-unit sector
//  window feeding Terrain_TraverseQuadTreeNode @ 0x5C89C0 (jodemo.exe);
//  render_terrain_sector_batch @ 0x6096f0 — the per-batch family select]

#include <runtime/terrain/foliage_detail_collector.h>
#include <runtime/terrain/quadtree.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>

#include <array>
#include <cstdint>
#include <vector>

namespace opennova {

struct CptFile;
struct TrnConfig;

// Device-ready vertex data. Atlas UVs feed retail's independent detail stream;
// the primary page coordinates are supplied by page_projection at submission.
struct TerrainTileVertex {
	std::array<float, 3> position{};
	std::array<float, 3> normal{};
	std::array<float, 2> atlas_uv{};
};

std::vector<TerrainTileVertex> build_terrain_tile_vertices(
		const CptFile &cpt, const TrnConfig &trn, int tile_index, bool zero_height);

// Per-tile draw constants derived once at snapshot build. The quadrant bits
// feed the shader's source-quadrant select; deriving them here removes the
// per-patch CPT tile re-index from the hot submission loop.
struct TerrainTileDrawAttributes {
	uint8_t quadrant_x = 0; // (tile_x >> 9) & 1
	uint8_t quadrant_z = 0; // (tile_y >> 9) & 1
	uint16_t local_page_x = 0;  // tile_x within the routed 512u sector
	uint16_t local_page_z = 0;  // tile_y within the routed 512u sector
	uint16_t source_page_x = 0; // exact 0..1023 source-atlas origin
	uint16_t source_page_z = 0; // exact 0..1023 source-atlas origin
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
	// Live water height in world units; 0 = no water this mission (the retail
	// Env_WaterHeightFixed == 0 sentinel). Feeds the below-water terrain flag.
	float water_height = 0.0f;
	// Retail view +100: the ordinary main view includes the flat fallback.
	// [orig: PolyTrn_RenderFrame @ 0x60EAC0, gate @ 0x60ECB0]
	bool skip_empty_sectors = false;
	TraversalConfig config{};
};

// One patch submission: which tile, which resolved mesh family, where. The
// draw-list order is the draw order (front-to-back by node distance) and the
// draw-list index is the embedder's pool slot.
struct TerrainPatchDraw {
	int32_t tile_index = -1;
	int32_t lod_family = 0;
	// Tile-composition page identity. The renderer must not reconstruct these
	// from lod_family: mesh-family selection and quadtree page LOD are
	// independent decisions.
	int32_t page_lod_level = 0;
	int32_t sector_x = 0;
	int32_t sector_z = 0;
	int32_t local_page_x = 0;
	int32_t local_page_z = 0;
	int32_t source_page_x = 0;
	int32_t source_page_z = 0;
	float sector_ox = 0.0f;
	float sector_oz = 0.0f;
	float distance = 0.0f;
	uint8_t quadrant_x = 0;
	uint8_t quadrant_z = 0;
	// The packed retail tile key's high bit. It changes geometry and primary
	// UVs without changing the source topology or the secondary detail UVs.
	bool zero_height = false;
};

// A flat draw uses retail's one shared LOD-0 page, independently of its mesh
// tile and world-sector origin. Ordinary pages retain their routed identity.
TerrainTileCompositionRequest terrain_tile_composition_request(
		const TerrainPatchDraw &draw, TerrainTileContentStamp content = {});

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
	// Camera below the live water surface: the terrain surface swaps its
	// stage-3 modulation input to the water noise (D-TERRAIN-8) [orig:
	// terrain_setup_view_and_lighting @ 0x60FE40 stores
	// cameraY < Env_WaterHeightFixed @ 0x60FEE0; terrain_render_visible_sectors
	// copies it to dword_319FB3C @ 0x60915F].
	bool below_water = false;
	// The frustum-surviving terrain bounds of this compile (every routed
	// sector's trackBounds walk), reset per compile; the water-active test
	// reads their height range. [orig: terrain_render_visible_sectors
	// @ 0x6090c0 resets the tracked AABB @ 0x609177..0x60919f and traverses
	// with trackBounds = 1 @ 0x609263]
	VisibleBounds visible_bounds{};
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
