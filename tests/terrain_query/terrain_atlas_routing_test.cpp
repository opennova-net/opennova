// The atlas routing (runtime/terrain_query atlas_routing.h): a mission position routes through the .trn's
// sector grid to a texel of the 1024-unit atlas the terrain's heights and maps lie over, as the game's
// lookups route it [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606530..0x60659D; Foliage_SampleFoliageMapMask
// @ 0x606620], and the inverse names every grid cell where a change to an atlas rect shows: an island (one
// mapped cell), a tiled layout (a quadrant repeated), the wraps and clamps past the grid (the cell masks,
// D-TERRAIN-16) and empty sectors (routed nowhere, painting nothing). The char map's and the foliage map's
// samplers route through it as their inline routing did.
#include <runtime/terrain_query/atlas_routing.h>
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/surface_type_map.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/test_expect.h"

namespace {

using namespace opennova::terrain;

constexpr int32_t kQ16 = 65536;

int32_t fx(int32_t units) { return units * kQ16; }

// The routing as surface_type_at_fixed held it before the lift, verbatim, for the sampler comparison.
bool old_route(const int *grid, int32_t ox, int32_t oy, bool wx, bool wz, int32_t x, int32_t y, int32_t &fine_x,
               int32_t &fine_z) {
	const auto clamp = [](int32_t v, bool wrap) { return (!wrap && (v & ~0xF) != 0) ? (v < 0 ? 0 : 15) : v; };
	const int32_t col = clamp((x >> 25) - ox, wx);
	const int32_t row = clamp(((-y) >> 25) - oy, wz);
	const int32_t slot = grid[16 * (row & 0xF) + (col & 0xF)] - 1;
	if (slot < 0) return false;
	fine_x = (x >> 16) & 0x1FF;
	fine_z = ((-y) >> 16) & 0x1FF;
	if ((slot & 1) != 0) fine_z += 512;
	if ((slot & 2) != 0) fine_x += 512;
	return true;
}

bool rect_inside(const TerrainWorldRect &r, const CoordsRect &atlas, const TerrainAtlasRouting &routing) {
	// Every corner texel of the world rect routes into the atlas rect (x right, z = -mission y down).
	const int32_t xs[2] = {r.x0, r.x1 - 1}, zs[2] = {r.z0, r.z1 - 1};
	for (int32_t x : xs)
		for (int32_t z : zs) {
			const TerrainAtlasTexel t = terrain_atlas_texel_at(routing, fx(x), -fx(z));
			if (!t.mapped || t.x < atlas.x || t.x >= atlas.x + atlas.w || t.z < atlas.z || t.z >= atlas.z + atlas.h)
				return false;
		}
	return true;
}

int test_island() {
	// One mapped cell: grid (row 2, col 3) = 4, the atlas's bottom-right quadrant, the grid from (-4, -4).
	int grid[16][16] = {};
	grid[2][3] = 4;
	const TerrainAtlasRouting routing{&grid[0][0], -4, -4, false, false};
	// World (-412, z -974): sector (-1, -2), cell (3, 2), its offset (100, 50) on the quadrant's (512, 512).
	const TerrainAtlasTexel t = terrain_atlas_texel_at(routing, fx(-412), fx(974));
	TEST_EXPECT(t.mapped && t.cell_col == 3 && t.cell_row == 2 && t.slot == 3 && t.x == 612 && t.z == 562);
	// Another cell is empty: routed nowhere.
	TEST_EXPECT(!terrain_atlas_texel_at(routing, fx(-2048 + 5), -fx(-2048 + 5)).mapped);
	// The inverse of an atlas rect inside the cell's quadrant: the one world rect.
	const CoordsRect atlas{600, 560, 20, 10};
	const std::vector<TerrainWorldRect> rects = terrain_atlas_world_rects(routing, atlas);
	TEST_EXPECT(rects.size() == 1);
	TEST_EXPECT(rects[0].x0 == -424 && rects[0].z0 == -976 && rects[0].x1 == -404 && rects[0].z1 == -966);
	TEST_EXPECT(rect_inside(rects[0], atlas, routing));
	// A rect of another quadrant shows nowhere; a rect past the atlas is clipped to it.
	TEST_EXPECT(terrain_atlas_world_rects(routing, CoordsRect{0, 0, 512, 512}).empty());
	const std::vector<TerrainWorldRect> clipped = terrain_atlas_world_rects(routing, CoordsRect{1000, 1000, 100, 100});
	TEST_EXPECT(clipped.size() == 1 && clipped[0].x1 - clipped[0].x0 == 24 && clipped[0].z1 - clipped[0].z0 == 24);
	// No grid: nothing routes, nothing shows.
	const TerrainAtlasRouting none{};
	TEST_EXPECT(!terrain_atlas_texel_at(none, 0, 0).mapped && terrain_atlas_world_rects(none, atlas).empty());
	std::printf("island: one cell routed and inverted, other quadrants and empty cells nowhere\n");
	return 0;
}

int test_tiled() {
	// A 4 x 4 block repeating the top-left quadrant (slot 0), the grid from (0, 0).
	int grid[16][16] = {};
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c) grid[r][c] = 1;
	const TerrainAtlasRouting routing{&grid[0][0], 0, 0, false, false};
	const std::vector<TerrainWorldRect> whole = terrain_atlas_world_rects(routing, CoordsRect{0, 0, 512, 512});
	TEST_EXPECT(whole.size() == 16);
	for (const TerrainWorldRect &r : whole) TEST_EXPECT(r.x1 - r.x0 == 512 && r.z1 - r.z0 == 512);
	// One texel: sixteen one-unit rects, each routing back to it.
	const CoordsRect texel{10, 20, 1, 1};
	const std::vector<TerrainWorldRect> repeats = terrain_atlas_world_rects(routing, texel);
	TEST_EXPECT(repeats.size() == 16);
	for (const TerrainWorldRect &r : repeats) TEST_EXPECT(r.x1 - r.x0 == 1 && r.z1 - r.z0 == 1 && rect_inside(r, texel, routing));
	// Grid order: the first repeat is cell (0, 0), the last cell (3, 3).
	TEST_EXPECT(repeats.front().x0 == 10 && repeats.front().z0 == 20 && repeats.back().x0 == 3 * 512 + 10 &&
	            repeats.back().z0 == 3 * 512 + 20);
	// The other quadrants are drawn nowhere.
	TEST_EXPECT(terrain_atlas_world_rects(routing, CoordsRect{512, 0, 512, 1024}).empty());
	std::printf("tiled: a repeated quadrant inverts to every repeat\n");
	return 0;
}

int test_wraps() {
	// Cells (0, 0) = 1 and (15, 15) = 4, the grid from (0, 0).
	int grid[16][16] = {};
	grid[0][0] = 1;
	grid[15][15] = 4;
	const TerrainAtlasRouting wrapped{&grid[0][0], 0, 0, true, true};
	const TerrainAtlasRouting clamped{&grid[0][0], 0, 0, false, false};
	// Column 16, row 0: wraps to column 0 (mapped); clamps to column 15 (row 0: empty).
	const TerrainAtlasTexel w16 = terrain_atlas_texel_at(wrapped, fx(16 * 512 + 5), -fx(7));
	TEST_EXPECT(w16.mapped && w16.cell_col == 0 && w16.cell_row == 0 && w16.x == 5 && w16.z == 7);
	const TerrainAtlasTexel c16 = terrain_atlas_texel_at(clamped, fx(16 * 512 + 5), -fx(7));
	TEST_EXPECT(!c16.mapped && c16.cell_col == 15 && c16.cell_row == 0);
	// Column -1, row -1: wraps to (15, 15) (mapped); clamps to (0, 0) (mapped).
	const TerrainAtlasTexel wm = terrain_atlas_texel_at(wrapped, fx(-512 + 3), -fx(-512 + 9));
	TEST_EXPECT(wm.mapped && wm.cell_col == 15 && wm.cell_row == 15 && wm.x == 512 + 3 && wm.z == 512 + 9);
	const TerrainAtlasTexel cm = terrain_atlas_texel_at(clamped, fx(-512 + 3), -fx(-512 + 9));
	TEST_EXPECT(cm.mapped && cm.cell_col == 0 && cm.cell_row == 0 && cm.x == 3 && cm.z == 9);
	// The inverse names the grid's own cells, wrap or not.
	TEST_EXPECT(terrain_atlas_world_rects(wrapped, CoordsRect{0, 0, 1024, 1024}).size() == 2);
	std::printf("wraps: a wrapped axis takes the low four bits, a clamped one the edge cell\n");
	return 0;
}

int test_samplers() {
	// A mixed layout with empty cells, both wraps, and maps of 256 and 1024: the samplers read the texel the
	// routing names, as the inline routing they replace did.
	int grid[16][16] = {};
	for (int r = 0; r < 16; ++r)
		for (int c = 0; c < 16; ++c) grid[r][c] = (r * 7 + c * 3) % 5; // 0 (empty) .. 4
	std::vector<uint8_t> map256(256 * 256), map1024(1024 * 1024);
	for (size_t i = 0; i < map256.size(); ++i) map256[i] = static_cast<uint8_t>(i * 31 + 7);
	for (size_t i = 0; i < map1024.size(); ++i) map1024[i] = static_cast<uint8_t>(i * 13 + 1);
	for (int wrap = 0; wrap < 4; ++wrap) {
		const bool wx = (wrap & 1) != 0, wz = (wrap & 2) != 0;
		SurfaceTypeMap surface;
		surface.data = map256.data();
		surface.width = surface.height = 256;
		surface.sector_grid = &grid[0][0];
		surface.origin_x = -3;
		surface.origin_y = 2;
		surface.wrap_x = wx;
		surface.wrap_z = wz;
		FoliageMaskMap foliage;
		foliage.data = map1024.data();
		foliage.width = foliage.height = 1024;
		foliage.sector_grid = &grid[0][0];
		foliage.origin_x = -3;
		foliage.origin_y = 2;
		foliage.wrap_x = wx;
		foliage.wrap_z = wz;
		uint32_t seed = 12345u + static_cast<uint32_t>(wrap);
		for (int i = 0; i < 4000; ++i) {
			seed = seed * 1103515245u + 12345u;
			const int32_t x = static_cast<int32_t>(seed % 40000u) - 20000;
			seed = seed * 1103515245u + 12345u;
			const int32_t y = static_cast<int32_t>(seed % 40000u) - 20000;
			const int32_t x_fixed = x * kQ16 + static_cast<int32_t>(seed & 0xFFFF);
			const int32_t y_fixed = y * kQ16 + static_cast<int32_t>((seed >> 8) & 0xFFFF);
			int32_t ox = 0, oz = 0;
			const bool mapped = old_route(&grid[0][0], -3, 2, wx, wz, x_fixed, y_fixed, ox, oz);
			const TerrainAtlasTexel t = terrain_atlas_texel_at(surface.routing(), x_fixed, y_fixed);
			TEST_EXPECT(t.mapped == mapped);
			if (mapped) TEST_EXPECT(t.x == ox && t.z == oz);
			const int32_t surface_want = mapped ? map256[static_cast<size_t>(oz >> 2) * 256 + (ox >> 2)] : 7;
			TEST_EXPECT(surface_type_at_fixed(surface, x_fixed, y_fixed) == surface_want);
			const int32_t foliage_want = mapped ? map1024[static_cast<size_t>(oz) * 1024 + ox] : 0;
			TEST_EXPECT(foliage_mask_at_fixed(foliage, x_fixed, y_fixed) == foliage_want);
		}
	}
	// A map's rect in atlas texels: 256 wide shifts by 2, 512 by 1, 1024 by 0.
	const CoordsRect r256 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 256);
	TEST_EXPECT(r256.x == 4 && r256.z == 8 && r256.w == 12 && r256.h == 16);
	const CoordsRect r512 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 512);
	TEST_EXPECT(r512.x == 2 && r512.w == 6);
	const CoordsRect r1024 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 1024);
	TEST_EXPECT(r1024.x == 1 && r1024.h == 4);
	std::printf("samplers: the char map and the foliage map read the routed texel under every wrap\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_island() + test_tiled() + test_wraps() + test_samplers();
	if (failures == 0) std::printf("terrain_atlas_routing: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
