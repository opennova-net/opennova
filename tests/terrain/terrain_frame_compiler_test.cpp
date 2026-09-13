// TerrainFrameCompiler (ADR 0033 R2) — the engine-owned terrain frame: scene
// snapshot invariants, the sector-window walk, front-to-back order, the shared
// emission budget, the LOD-family fallback, the foliage detail-cell handoff
// (and its node-distance gate),
// and the engine-side MVP/frustum path.
#include <runtime/terrain/terrain_frame.h>
#include <runtime/renderer/foliage_frame.h>

#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>

#include <cmath>
#include <cstdio>
#include <utility>

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

void add_quadrant_tiles(opennova::CptFile &cpt, int source_x, int count) {
	for (int i = 0; i < count; ++i) {
		auto tile = make_tile(static_cast<uint16_t>(source_x + (i % 8) * 64),
				static_cast<uint16_t>((i / 8) * 64));
		give_lod0_list(tile);
		cpt.tiles.push_back(std::move(tile));
	}
}

bool same_detail_cells(const std::vector<opennova::FoliageDetailPatch> &a,
		const std::vector<opennova::FoliageDetailPatch> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].key != b[i].key || a[i].distance != b[i].distance) return false;
	}
	return true;
}

int test_foliage_handoff_is_independent_of_terrain_budget() {
	// Three full earlier sectors and one 31/32-tile sector leave exactly
	// 223/224 main draws before the nearby sector. Its NW leaf is the next
	// emission. The remaining nearby cells must survive either boundary.
	// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, cap bypass @ 0x608FBC
	// -> 0x609012, independent foliage handoff @ 0x60905C..0x60907C]
	for (int preceding = 223; preceding <= 224; ++preceding) {
		auto cpt = make_cpt_base();
		add_quadrant_tiles(cpt, 0, 64);
		add_quadrant_tiles(cpt, 512, preceding - 192);
		opennova::TrnConfig routing;
		routing.origin_x = routing.origin_y = -4;
		for (int sx = -2; sx <= 0; ++sx) routing.sector_grid[4][sx + 4] = 1;
		routing.sector_grid[4][5] = 3;
		routing.sector_grid[5][5] = 1;
		opennova::TrnConfig nearby_only;
		nearby_only.origin_x = nearby_only.origin_y = -4;
		nearby_only.sector_grid[5][5] = 1;
		const auto dense = opennova::build_terrain_scene_snapshot(cpt, routing);
		const auto control = opennova::build_terrain_scene_snapshot(cpt, nearby_only);
		opennova::TerrainViewInput view;
		view.skip_empty_sectors = true;
		view.cam_x = view.cam_z = 544.0f;
		view.cam_y = 10.0f;
		identity(view.view);
		identity(view.proj);
		view.config.no_frustum = true;
		view.config.force_leaves = true;
		opennova::TerrainFrameCompiler compiler;
		const auto expected = compiler.compile(control, view).detail_cells;
		if (!expect(expected.size() == 24,
				"the nearby control has 24 in-range cells across several terrain leaves")) return 1;
		const auto &limited = compiler.compile(dense, view);
		if (!expect(limited.debug.sectors_walked == 5 && limited.patches.size() == 224 &&
				limited.debug.traversal.budget_drops == preceding + 64 - 224,
				"223/224 earlier emissions saturate only the main terrain draw list")) return 1;
		int retained_nearby = 0;
		for (const auto &draw : limited.patches)
			if (draw.sector_x == 1 && draw.sector_z == 1) ++retained_nearby;
		if (!expect(retained_nearby == 224 - preceding,
				"the main append accepts its last slot at 223 and rejects the next at 224")) return 1;
		if (!expect(same_detail_cells(limited.detail_cells, expected),
				"foliage preserves every nearby key and its order after the main list fills")) return 1;

		// Default flat sectors fill the main list first and consume their own
		// foliage slots. This fixture stays below 128, so the nearby authored
		// keys survive alongside the additional flagged cells in source order.
		view.skip_empty_sectors = false;
		const auto &with_empty = compiler.compile(dense, view);
		std::vector<opennova::FoliageDetailPatch> authored_cells;
		for (const auto &cell : with_empty.detail_cells)
			if ((cell.key & 0x80000000u) == 0u) authored_cells.push_back(cell);
		if (!expect(with_empty.patches.size() == 224 &&
				with_empty.detail_cells.size() > authored_cells.size() &&
				with_empty.detail_cells.size() <= opennova::kFoliageDetailPatchCapacity &&
				same_detail_cells(authored_cells, expected),
				"default fallback adds flagged foliage keys independently of the full main list")) return 1;
		for (const auto &draw : with_empty.patches)
			if (!expect(draw.zero_height,
					"earlier empty sectors fill all retained main slots in this fixture")) return 1;
	}
	return 0;
}

int test_flat_terrain_keys_reach_empty_foliage_cache_entries() {
	auto cpt = make_cpt_base();
	add_quadrant_tiles(cpt, 0, 64);
	opennova::TrnConfig routing;
	routing.origin_x = routing.origin_y = -4;
	const auto scene = opennova::build_terrain_scene_snapshot(cpt, routing);
	opennova::TerrainViewInput terrain_view;
	terrain_view.cam_x = terrain_view.cam_z = 256.0f;
	terrain_view.cam_y = 10.0f;
	identity(terrain_view.view);
	identity(terrain_view.proj);
	terrain_view.config.no_frustum = true;
	terrain_view.config.force_leaves = true;
	opennova::TerrainFrameCompiler terrain;
	const auto &terrain_draws = terrain.compile(scene, terrain_view);
	if (!expect(!terrain_draws.detail_cells.empty(),
			"default empty sectors produce real foliage collector keys")) return 1;

	opennova::renderer::FoliageViewInput foliage_view;
	foliage_view.no_frustum = true;
	for (const auto &cell : terrain_draws.detail_cells) {
		if (!expect((cell.key & 0x80000000u) != 0u,
				"all-empty terrain preserves the flat flag through frame compilation")) return 1;
		foliage_view.detail_cells.push_back({cell.key, cell.distance});
	}
	std::array<opennova::foliage::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS> slots{};
	slots[0].enabled = true;
	slots[0].model_radius = 2.0f;
	slots[0].source_vertex_count = 3;
	std::array<opennova::renderer::FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS> geometry{};
	geometry[0].valid = true;
	geometry[0].vertices = {{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 1, 0, 0, 1}};
	geometry[0].indices = {0, 1, 2};
	opennova::renderer::FoliageFrameCompiler foliage;
	foliage.configure_slots(slots, geometry);
	int samples = 0;
	opennova::foliage::WorldSamplers world;
	world.detail_foliage_mask_at = [&samples](int32_t, int32_t) { ++samples; return 1u; };
	world.model_foliage_mask_at = world.detail_foliage_mask_at;
	world.height_at = [&samples](float, float) { ++samples; return 8.0f; };
	world.path_blocked = [&samples](float, float, float) { ++samples; return false; };
	opennova::renderer::FoliageExpansionSamplers expansion;
	expansion.terrain_uv_at = [&samples](float, float, float &u, float &v) {
		++samples; u = v = 0.5f; return true;
	};
	for (int frame = 0; frame < 2; ++frame) {
		const auto &draws = foliage.compile(foliage_view, world, expansion);
		const auto &stats = draws.debug.runtime.detail;
		if (!expect(draws.vertices.empty() && draws.indices.empty() &&
				draws.mesh_builds.empty() && draws.commands.empty() && samples == 0 &&
				stats.residents == foliage_view.detail_cells.size() &&
				(frame == 0 ? stats.regenerations : stats.hits) == foliage_view.detail_cells.size(),
				"flat terrain keys retain empty foliage cache residents without sampling or drawing")) return 1;
	}
	terrain_view.skip_empty_sectors = true;
	if (!expect(terrain.compile(scene, terrain_view).detail_cells.empty(),
			"skip-empty view suppresses both flat terrain and its foliage keys")) return 1;
	return 0;
}

int test_full_terrain_budget_preserves_foliage_frustum_and_distance_gates() {
	auto cpt = make_cpt_base();
	add_quadrant_tiles(cpt, 0, 64);
	opennova::TrnConfig routing;
	routing.origin_x = routing.origin_y = -4;
	// Five distant sectors supply 320 visible terrain emissions before the
	// nearby sector. All are in front of a wide camera, beyond foliage range.
	for (int sx = -2; sx <= 2; ++sx) routing.sector_grid[4][sx + 4] = 1;
	routing.sector_grid[5][5] = 1;
	opennova::TrnConfig nearby_only;
	nearby_only.origin_x = nearby_only.origin_y = -4;
	nearby_only.sector_grid[5][5] = 1;
	const auto dense = opennova::build_terrain_scene_snapshot(cpt, routing);
	const auto control = opennova::build_terrain_scene_snapshot(cpt, nearby_only);
	opennova::TerrainViewInput view;
	view.skip_empty_sectors = true;
	view.cam_x = view.cam_z = 896.0f;
	view.cam_y = 10.0f;
	view.config.force_leaves = true;
	look_at(view.view, {896.0f, 10.0f, 896.0f},
			{896.0f, 10.0f, 895.0f}, {0.0f, 1.0f, 0.0f});
	perspective_gl(view.proj, 1.5707963268f, 8.0f, 0.1f, 5000.0f);
	opennova::TerrainFrameCompiler compiler;
	const auto expected = compiler.compile(control, view).detail_cells;
	if (!expect(!expected.empty(), "the forward nearby foliage wedge is visible")) return 1;
	const auto &limited = compiler.compile(dense, view);
	if (!expect(limited.patches.size() == 224 && limited.debug.traversal.budget_drops > 0,
			"earlier visible distant sectors exhaust the main terrain cap")) return 1;
	for (const auto &draw : limited.patches)
		if (!expect(draw.sector_z == 0,
				"the nearby sector contributes foliage after every main slot is spent")) return 1;
	if (!expect(same_detail_cells(limited.detail_cells, expected),
			"budget-independent foliage keeps the same frustum wedge and traversal order")) return 1;
	for (const auto &cell : limited.detail_cells) {
		if (!expect(cell.distance <= opennova::kFoliageDetailDistanceLimit &&
				(cell.key & 0x7fffu) <= 896u,
				"the independent handoff adds no far or wholly behind-camera cells")) return 1;
	}
	return 0;
}

int test_foliage_handoff_requires_the_traversal_node_distance_gate() {
	// Retail hands a node to the collector only when its raw traversal
	// distance (X/Z clamped to the node box, Y to the node center) less the
	// fixed 16.0 is within 42: nodes beyond 58 units never reach the
	// collector, however near their own 16u cells are. Flat sectors zero the
	// node center Y, so their node distance is the camera height itself.
	// [orig: Terrain_TraverseQuadtreeNode @ 0x608A00, 16.0 @ 0x608D46,
	// flat center @ 0x608A50, handoff gate @ 0x60906B..0x609078]
	const auto view_at = [](float cam_y, bool skip_empty) {
		opennova::TerrainViewInput view;
		view.skip_empty_sectors = skip_empty;
		view.cam_x = view.cam_z = 32.0f;
		view.cam_y = cam_y;
		identity(view.view);
		identity(view.proj);
		view.config.no_frustum = true;
		view.config.force_leaves = true;
		return view;
	};
	opennova::TerrainFrameCompiler compiler;

	// Authored: the camera stands inside a 64u leaf whose height range is
	// [0,127] (one 127-unit sample among zeros), so the node distance is
	// |63.5 - cam_y| while most of its 16u cells sit at height 0.
	{
		opennova::CptFile cpt;
		cpt.depth_buffer.assign(1024 * 1024, 0);
		// Mip max = (0x7F00 + 127) >> 7 = 254 -> 127.0; min stays 0.
		cpt.depth_buffer[40 * 1024 + 40] = 0x7F00;
		auto tile = make_tile(0, 0);
		give_lod0_list(tile);
		cpt.tiles.push_back(std::move(tile));
		opennova::TrnConfig routing;
		routing.origin_x = routing.origin_y = -2;
		routing.sector_grid[2][2] = 1;
		const auto scene = opennova::build_terrain_scene_snapshot(cpt, routing);
		if (!expect(scene.valid(), "the mixed-height node fixture builds")) return 1;
		const auto &far_center = compiler.compile(scene, view_at(0.0f, true));
		if (!expect(far_center.patches.size() == 1 && !far_center.patches[0].zero_height &&
				far_center.detail_cells.empty(),
				"a node 63.5 units from the camera is drawn but never handed to the "
				"collector, despite its height-0 cells directly under the camera")) return 1;
		if (!expect(!compiler.compile(scene, view_at(5.5f, true)).detail_cells.empty(),
				"node distance 58.0 (58 - 16 <= 42) still hands off")) return 1;
		if (!expect(compiler.compile(scene, view_at(5.0f, true)).detail_cells.empty(),
				"node distance 58.5 does not hand off")) return 1;
		if (!expect(!compiler.compile(scene, view_at(30.0f, true)).detail_cells.empty(),
				"a node inside 58 units hands off and its near cells collect")) return 1;
	}

	// Flat: every sector is empty. Quadrant 1's raw heights are 64 (the
	// collector's cell centers) while the flat traversal center is 0.
	{
		opennova::CptFile cpt;
		cpt.depth_buffer.assign(1024 * 1024, 0x4000); // 64.0 world units
		auto tile = make_tile(0, 0);
		give_lod0_list(tile);
		cpt.tiles.push_back(std::move(tile));
		opennova::TrnConfig all_empty;
		all_empty.origin_x = all_empty.origin_y = -2;
		const auto scene = opennova::build_terrain_scene_snapshot(cpt, all_empty);
		if (!expect(scene.valid(), "the all-empty fixture builds")) return 1;
		const auto &high = compiler.compile(scene, view_at(64.0f, false));
		if (!expect(high.patches.size() == 121 && high.detail_cells.empty(),
				"a flat sector under a camera at 64 (node distance 64 > 58) draws but "
				"never hands off, though its raw quadrant-1 cells sit at the camera height")) return 1;
		for (const auto &draw : high.patches)
			if (!expect(draw.zero_height, "an all-empty grid draws only the flat fallback")) return 1;
		const auto &low = compiler.compile(scene, view_at(50.0f, false));
		if (!expect(!low.detail_cells.empty(),
				"a flat sector under a camera at 50 hands off and its cells collect")) return 1;
		for (const auto &cell : low.detail_cells)
			if (!expect((cell.key & 0x80000000u) != 0u,
					"flat handoffs keep their flagged keys")) return 1;
	}
	return 0;
}

} // namespace

int main() {
	using opennova::TerrainFrameCompiler;
	using opennova::TerrainViewInput;

	if (test_foliage_handoff_is_independent_of_terrain_budget() != 0) return 1;
	if (test_flat_terrain_keys_reach_empty_foliage_cache_entries() != 0) return 1;
	if (test_full_terrain_budget_preserves_foliage_frustum_and_distance_gates() != 0) return 1;
	if (test_foliage_handoff_requires_the_traversal_node_distance_gate() != 0) return 1;

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
	view.skip_empty_sectors = true;
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
		if (!expect(pkt.patches[0].page_lod_level == 4 &&
					pkt.patches[0].sector_x == 0 && pkt.patches[0].sector_z == 0 &&
					pkt.patches[0].local_page_x == 0 && pkt.patches[0].local_page_z == 0 &&
					pkt.patches[0].source_page_x == 0 && pkt.patches[0].source_page_z == 0,
				"leaf draw retains its cache-page and source-atlas identity")) return 1;
		if (!expect(pkt.patches[1].page_lod_level == 4 &&
					pkt.patches[1].local_page_x == 64 && pkt.patches[1].local_page_z == 0 &&
					pkt.patches[1].source_page_x == 64 && pkt.patches[1].source_page_z == 0,
				"adjacent leaf keeps a distinct page origin")) return 1;
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
		fv.skip_empty_sectors = true;
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
		// The tracked visible bounds follow the same frustum verdict: valid
		// with the tile heights inside them when terrain is in view, absent
		// when nothing survives [orig: Terrain_TraverseQuadtreeNode @ 0x608a00
		// trackBounds leg; terrain_render_visible_sectors @ 0x609263].
		// The g_WaterActive predicate over tracked bounds
		// [orig: terrain_setup_view_and_lighting @0x60ff12..0x60ff31].
		if (!expect(opennova::water_pass_active(false, 0.0f, 0.0f, 4.0f, false),
						"untracked bounds keep the water pass live") ||
				!expect(opennova::water_pass_active(true, 3.0f, 9.0f, 4.0f, false),
						"lowest visible terrain at or below the water keeps it live") ||
				!expect(!opennova::water_pass_active(true, 5.0f, 9.0f, 4.0f, false),
						"terrain wholly above the water retires the pass") ||
				!expect(opennova::water_pass_active(true, 5.0f, 9.0f, 4.0f, true),
						"last frame's Blink water verdict forces the pass"))
			return 1;
		if (!expect(!away.visible_bounds.valid,
				"no visible terrain tracks no bounds")) return 1;
		look_at(fv.view, {32.0f, 40.0f, -20.0f}, {32.0f, 8.0f, 32.0f},
				{0.0f, 1.0f, 0.0f});
		const opennova::TerrainDrawList &toward = compiler.compile(scene, fv);
		if (!expect(toward.visible_bounds.valid,
				"terrain in view tracks bounds")) return 1;
		if (!expect(toward.visible_bounds.min[1] <= 8.0f &&
						toward.visible_bounds.max[1] >= 8.0f,
				"the tracked height range spans the visible tile heights")) return 1;
	}

	// --- Empty sectors: topology reuse, independent geometry/UV/cache mode --
	// [orig: PolyTrn_RenderFrame @ 0x60EAC0, routing @ 0x60EC94..0x60ECBD;
	// decode_terrain_tile_vertices @ 0x602AA0, zero stores @ 0x602DC7..0x602DCF]
	{
		opennova::CptFile fallback_cpt = make_cpt_base();
		opennova::CptTile tile = make_tile(0, 0);
		tile.tile_size = 512;
		tile.vertex_indices = {0, 0, 512, 0, 0, 512, 512, 512};
		give_lod0_list(tile);
		fallback_cpt.tiles.push_back(tile);
		fallback_cpt.depth_buffer[512] = 4096;
		fallback_cpt.depth_buffer[512 * 1024] = 6144;
		fallback_cpt.depth_buffer[512 * 1024 + 512] = 8192;
		opennova::TerrainSceneSnapshot fallback_scene =
				opennova::build_terrain_scene_snapshot(fallback_cpt, trn);
		TerrainViewInput fallback_view;
		fallback_view.cam_x = fallback_view.cam_z = 32.0f;
		fallback_view.cam_y = 12.0f;
		fallback_view.config.no_frustum = true;
		const auto &mixed = compiler.compile(fallback_scene, fallback_view);
		if (!expect(mixed.debug.sectors_walked == 121 && mixed.patches.size() == 121,
				"ordinary main view walks empty and authored sectors across the 11x11 window")) return 1;
		int normal_count = 0;
		int flat_count = 0;
		opennova::TerrainTileCompositionCache pages;
		int flat_layer = -1;
		for (const auto &patch : mixed.patches) {
			if (!expect(patch.tile_index == 0,
					"empty sectors reuse the existing first-quadrant mesh topology")) return 1;
			const auto vertices = opennova::build_terrain_tile_vertices(
					fallback_cpt, trn, patch.tile_index, patch.zero_height);
			if (!expect(vertices.size() == 4, "both variants retain the CPT vertex count")) return 1;
			const auto request = opennova::terrain_tile_composition_request(patch);
			const auto decision = pages.request(request);
			if (!expect(decision.has_value(), "both mesh modes produce a cache request")) return 1;
			if (patch.zero_height) {
				++flat_count;
				for (const auto &vertex : vertices) {
					if (!expect(vertex.position[1] == 0.0f,
							"flat variant never inherits nonzero source heights")) return 1;
				}
				if (!expect(vertices[3].position[0] == 512.0f &&
						vertices[3].position[2] == 512.0f &&
						vertices[3].atlas_uv == std::array<float, 2>{0.5f, 0.5f},
						"flat positions preserve the footprint and secondary detail UVs")) return 1;
				if (flat_layer < 0) flat_layer = decision->binding.layer;
				if (!expect(decision->binding.layer == flat_layer &&
						request.page.page_lod_level == 0 && request.tile_index == -1 &&
						request.page.sector_origin_x == 0 && request.page.sector_origin_z == 0,
						"all empty sectors share one canonical flat cache page")) return 1;
				const auto uv = opennova::TerrainTileCompositionCache::page_projection(
						request.page, patch.zero_height);
				if (!expect(uv.has_value() && uv->project(patch.sector_ox + 512.0f,
						patch.sector_oz + 512.0f) == std::array<float, 2>{0.0f, 0.0f},
						"the submitted flat primary UV remains zero at every world-sector origin")) return 1;
			} else {
				++normal_count;
				if (!expect(vertices[0].position[1] == 8.0f && vertices[3].position[1] == 32.0f,
						"authored sector retains the nonflat source geometry")) return 1;
			}
		}
		if (!expect(normal_count == 1 && flat_count == 120,
				"adjacent authored and empty sectors keep distinct mesh modes")) return 1;
		opennova::TerrainPatchDraw other_flat;
		other_flat.zero_height = true;
		other_flat.tile_index = 77;
		other_flat.page_lod_level = 4;
		other_flat.local_page_x = other_flat.source_page_x = 128;
		other_flat.sector_x = -7;
		other_flat.sector_z = 9;
		const auto other_page = pages.request(
				opennova::terrain_tile_composition_request(other_flat));
		if (!expect(other_page.has_value() && !other_page->job.has_value() &&
				other_page->binding.layer == flat_layer,
				"different flat mesh tiles and LODs cannot split the shared page identity")) return 1;

		TerrainViewInput low_view;
		identity(low_view.view);
		identity(low_view.proj);
		low_view.config.no_nearfar = true;
		const auto &low_window = compiler.compile(fallback_scene, low_view);
		if (!expect(!low_window.patches.empty(),
				"a frustum around zero height retains flat fallback geometry")) return 1;
		for (const auto &patch : low_window.patches) {
			if (!expect(patch.zero_height,
					"the zero-height frustum rejects elevated source terrain but admits its flat variant")) return 1;
		}
		if (!expect(low_window.visible_bounds.valid && low_window.visible_bounds.min[1] >= 8.0f,
				"flat culling does not replace retail's raw tracked height stores")) return 1;
		fallback_view.skip_empty_sectors = true;
		if (!expect(compiler.compile(fallback_scene, fallback_view).patches.size() == 1,
				"explicit skip policy leaves only the authored sector")) return 1;
		fallback_scene.sector_grid[2][2] = 0;
		if (!expect(compiler.compile(fallback_scene, fallback_view).patches.empty(),
				"all-empty sector grid with skip enabled emits nothing")) return 1;
		fallback_view.skip_empty_sectors = false;
		fallback_view.water_height = 4.0f;
		fallback_view.cam_y = 2.0f;
		const auto &underwater = compiler.compile(fallback_scene, fallback_view);
		if (!expect(underwater.patches.size() == 121 && underwater.below_water &&
				underwater.visible_bounds.valid && underwater.visible_bounds.min[1] >= 8.0f,
				"all-empty fallback survives underwater and retains retail's raw tracked bounds")) return 1;
		fallback_view.cam_y = 6.0f;
		if (!expect(!compiler.compile(fallback_scene, fallback_view).below_water,
				"flat fallback above water retains ordinary view classification")) return 1;
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
		dv.skip_empty_sectors = true;
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
