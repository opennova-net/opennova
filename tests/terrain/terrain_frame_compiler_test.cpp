// TerrainFrameCompiler (ADR 0033 R2) — the engine-owned terrain frame: scene
// snapshot invariants, the sector-window walk, front-to-back order, the shared
// emission budget, the LOD-family fallback, the foliage detail-cell handoff,
// and the engine-side MVP/frustum path.
#include <terrain/terrain_frame.h>

#include <cpt/cpt.h>
#include <trn/trn.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

opennova::CptTile make_tile(uint16_t x, uint16_t y) {
	opennova::CptTile tile;
	tile.tile_x = x;
	tile.tile_y = y;
	tile.tile_size = 64;
	tile.vertex_count = 4;
	// Corner vertices spanning the closing edge row, as baked tiles do — the
	// foliage handoff derives the node rect from the tile center, so a tile
	// must cover its full 64-unit cell.
	tile.vertex_indices = {0, 0, 64, 0, 0, 64, 64, 64};
	return tile;
}

void give_lod0_list(opennova::CptTile &tile) {
	tile.lods[0].indices = {0, 1, 2, 1, 3, 2};
	tile.lods[0].is_strip = false;
}

opennova::CptFile make_cpt_base() {
	opennova::CptFile cpt;
	cpt.depth_buffer.assign(1024 * 1024, 0x800); // 2048/256 = 8.0 world units
	return cpt;
}

void identity(float m[16]) {
	for (int i = 0; i < 16; ++i) m[i] = 0.0f;
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void perspective_gl(float m[16], float fovy_rad, float aspect, float near_p,
                    float far_p) {
	for (int i = 0; i < 16; ++i) m[i] = 0.0f;
	const float f = 1.0f / std::tan(fovy_rad * 0.5f);
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (far_p + near_p) / (near_p - far_p);
	m[11] = -1.0f;
	m[14] = 2.0f * far_p * near_p / (near_p - far_p);
}

struct V3 {
	float x, y, z;
};

V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
	const float l = std::sqrt(dot(a, a));
	return {a.x / l, a.y / l, a.z / l};
}

// Column-major world-to-view matrix for a camera at eye looking at target.
void look_at(float m[16], V3 eye, V3 target, V3 up) {
	const V3 fwd = norm(sub(target, eye));
	const V3 right = norm(cross(fwd, up));
	const V3 up2 = cross(right, fwd);
	m[0] = right.x; m[4] = right.y; m[8] = right.z;  m[12] = -dot(right, eye);
	m[1] = up2.x;   m[5] = up2.y;   m[9] = up2.z;    m[13] = -dot(up2, eye);
	m[2] = -fwd.x;  m[6] = -fwd.y;  m[10] = -fwd.z;  m[14] = dot(fwd, eye);
	m[3] = 0.0f;    m[7] = 0.0f;    m[11] = 0.0f;    m[15] = 1.0f;
}

} // namespace

int main() {
	using opennova::TerrainFrameCompiler;
	using opennova::TerrainViewInput;

	// --- Scene: three quadrant-0 leaf tiles with distinct LOD-0 payloads ----
	opennova::CptFile cpt = make_cpt_base();
	{
		opennova::CptTile a = make_tile(0, 0);
		give_lod0_list(a); // 6 indices
		opennova::CptTile b = make_tile(64, 0);
		b.lods[0].indices = {0, 1, 2, 2, 3}; // strip; one degenerate window
		b.lods[0].is_strip = true;           // -> 3 converted indices
		opennova::CptTile c = make_tile(128, 0); // every LOD empty
		opennova::CptTile d = make_tile(576, 512); // quadrant (1,1) attribute probe
		give_lod0_list(d);
		cpt.tiles = {a, b, c, d};
	}
	// Route only camera sector (0,0) to quadtree child 0. The window's
	// out-of-range clamp re-reads grid row/column 0, so the routed cell sits
	// at [2][2] with origin -2 — a grid border of zeros keeps clamped window
	// cells unrouted (the retail maps' authored layout).
	opennova::TrnConfig trn;
	trn.origin_x = -2;
	trn.origin_y = -2;
	trn.sector_grid[2][2] = 1;

	const opennova::TerrainSceneSnapshot scene =
			opennova::build_terrain_scene_snapshot(cpt, trn);

	// --- Snapshot invariants ------------------------------------------------
	if (!expect(scene.valid(), "snapshot builds valid")) return 1;
	// 1024 -> 64 subdivision: 1 + 4 + 16 + 64 + 256 nodes.
	if (!expect(static_cast<int>(scene.quad_nodes.size()) == 341,
			"full 1024->64 tree has 341 nodes")) return 1;
	for (int i = 0; i < 4; ++i) {
		if (!expect(scene.l1_children[i] >= 0, "level-1 children resolved")) return 1;
	}
	if (!expect(scene.tile_meshes[0].lods[0].index_count == 6,
			"list LOD keeps whole triangles")) return 1;
	if (!expect(scene.tile_meshes[1].lods[0].index_count == 3,
			"strip LOD count skips degenerate windows")) return 1;
	if (!expect(scene.tile_meshes[2].lods[0].index_count == 0,
			"empty LOD counts zero")) return 1;
	if (!expect(scene.tile_attributes[0].quadrant_x == 0 &&
					scene.tile_attributes[0].quadrant_z == 0 &&
					scene.tile_attributes[3].quadrant_x == 1 &&
					scene.tile_attributes[3].quadrant_z == 1,
			"tile attributes carry the source quadrant bits")) return 1;
	if (!expect(std::fabs(scene.tile_meshes[0].center[0] - 32.0f) < 1e-3f &&
					std::fabs(scene.tile_meshes[0].center[1] - 8.0f) < 1e-3f,
			"tile AABB center from the depth buffer")) return 1;

	// --- Compile, culling disabled: order, fallback, handoff ---------------
	TerrainFrameCompiler compiler;
	TerrainViewInput view;
	view.cam_x = 32.0f;
	view.cam_y = 10.0f;
	view.cam_z = 32.0f;
	identity(view.view);
	identity(view.proj);
	view.config.no_frustum = true;
	view.config.quality = 1.0f;

	{
		const opennova::TerrainDrawList &pkt = compiler.compile(scene, view);
		if (!expect(pkt.frame_id == 1, "frame id counts compiles")) return 1;
		if (!expect(pkt.debug.sectors_walked == 1, "one routed sector walked")) return 1;
		if (!expect(pkt.debug.visible_patches == 3,
				"three tile leaves emitted pre-budget")) return 1;
		if (!expect(static_cast<int>(pkt.patches.size()) == 2 &&
						pkt.debug.emitted_patches == 2,
				"the empty-LOD tile is dropped from the draw_list")) return 1;
		if (!expect(pkt.debug.empty_mesh_drops == 1, "the drop is counted")) return 1;
		if (!expect(pkt.patches[0].tile_index == 0 && pkt.patches[1].tile_index == 1,
				"patches are ordered front-to-back")) return 1;
		if (!expect(pkt.patches[0].distance <= pkt.patches[1].distance,
				"distance is ascending")) return 1;
		if (!expect(pkt.patches[0].lod_family == 0 && pkt.patches[1].lod_family == 0,
				"near leaves resolve family 0")) return 1;
		if (!expect(pkt.patches[0].sector_ox == 0.0f && pkt.patches[0].sector_oz == 0.0f,
				"sector origin rides the patch")) return 1;
		if (!expect(!pkt.detail_cells.empty(),
				"leaf emissions hand off detail cells near the camera")) return 1;

		const size_t cells = pkt.detail_cells.size();
		const opennova::TerrainDrawList &pkt2 = compiler.compile(scene, view);
		if (!expect(pkt2.frame_id == 2 && pkt2.patches.size() == 2 &&
						pkt2.detail_cells.size() == cells,
				"recompile is deterministic")) return 1;
	}

	// --- Below-water flag (D-TERRAIN-8): the bare strict < ------------------
	{
		TerrainViewInput wv = view;
		wv.water_height = 8.0f;
		wv.cam_y = 4.0f;
		if (!expect(compiler.compile(scene, wv).below_water,
				"camera below the water height sets below_water")) return 1;
		wv.cam_y = 12.0f;
		if (!expect(!compiler.compile(scene, wv).below_water,
				"camera above the water height clears below_water")) return 1;
		wv.cam_y = 8.0f;
		if (!expect(!compiler.compile(scene, wv).below_water,
				"camera exactly at the height reads dry (strict <)")) return 1;
		wv.water_height = 0.0f;
		wv.cam_y = -5.0f;
		if (!expect(compiler.compile(scene, wv).below_water,
				"the compare is unguarded: a sub-zero eye on a zero height "
				"still reads below (retail has no zero sentinel; terrain "
				"heights keep it unreachable in practice)")) return 1;
	}

	// --- Compile with the engine-side MVP/frustum path ----------------------
	{
		TerrainViewInput fv;
		fv.cam_x = 32.0f;
		fv.cam_y = 40.0f;
		fv.cam_z = -20.0f;
		perspective_gl(fv.proj, 1.2f, 4.0f / 3.0f, 0.1f, 2000.0f);
		look_at(fv.view, {32.0f, 40.0f, -20.0f}, {32.0f, 8.0f, 32.0f},
				{0.0f, 1.0f, 0.0f});
		fv.config.quality = 1.0f;
		const opennova::TerrainDrawList &pkt = compiler.compile(scene, fv);
		bool saw_a = false;
		for (const opennova::TerrainPatchDraw &p : pkt.patches) {
			if (p.tile_index == 0) saw_a = true;
		}
		if (!expect(saw_a, "a camera over the tile keeps it in frustum")) return 1;

		look_at(fv.view, {32.0f, 40.0f, -20.0f}, {32.0f, 120.0f, -200.0f},
				{0.0f, 1.0f, 0.0f});
		const opennova::TerrainDrawList &away = compiler.compile(scene, fv);
		if (!expect(away.patches.empty() && away.detail_cells.empty(),
				"a camera facing away culls everything")) return 1;
	}

	// --- The shared emission budget across routed sectors -------------------
	{
		opennova::CptFile big = make_cpt_base();
		for (int z = 0; z < 8; ++z) {
			for (int x = 0; x < 8; ++x) {
				opennova::CptTile t = make_tile(
						static_cast<uint16_t>(x * 64), static_cast<uint16_t>(z * 64));
				give_lod0_list(t);
				big.tiles.push_back(t);
			}
		}
		opennova::TrnConfig routing;
		routing.origin_x = -2;
		routing.origin_y = -2;
		routing.sector_grid[2][2] = 1;
		routing.sector_grid[2][3] = 1;
		routing.sector_grid[3][2] = 1;
		routing.sector_grid[3][3] = 1;
		const opennova::TerrainSceneSnapshot dense =
				opennova::build_terrain_scene_snapshot(big, routing);
		if (!expect(dense.valid(), "dense snapshot builds")) return 1;

		TerrainViewInput dv;
		dv.cam_x = 256.0f;
		dv.cam_y = 10.0f;
		dv.cam_z = 256.0f;
		identity(dv.view);
		identity(dv.proj);
		dv.config.no_frustum = true;
		dv.config.force_leaves = true;
		dv.config.quality = 1.0f;
		const opennova::TerrainDrawList &pkt = compiler.compile(dense, dv);
		if (!expect(pkt.debug.sectors_walked == 4, "four routed sectors walked")) return 1;
		if (!expect(pkt.debug.visible_patches == 224,
				"the traversal emission budget is shared across sectors")) return 1;
		if (!expect(pkt.debug.traversal.budget_drops == 32,
				"over-budget emissions are counted, not kept")) return 1;
		if (!expect(static_cast<int>(pkt.patches.size()) == 224 &&
						pkt.patches.size() <= TerrainFrameCompiler::kPatchBudget,
				"the draw_list stays within the pool budget")) return 1;
		for (size_t i = 1; i < pkt.patches.size(); ++i) {
			if (!expect(pkt.patches[i - 1].distance <= pkt.patches[i].distance,
					"dense draw_list stays front-to-back")) return 1;
		}
	}

	std::printf("OK: terrain_frame_compiler snapshot/order/budget/fallback/frustum\n");
	return 0;
}
