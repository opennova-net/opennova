// The atlas routing (runtime/terrain_query atlas_routing.h): a mission position routes through the .trn's
// sector grid to a texel of the 1024-unit atlas the terrain's heights and maps lie over, as the game's
// lookups route it [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606530..0x60659D; Foliage_SampleFoliageMapMask
// @ 0x606620; Terrain_SampleHeightBilinear @ 0x6067BD..0x6068C4], and its inverse (terrain_atlas_rects.h):
// where a change to an atlas rect shows, for the one-texel lookups (the char and foliage maps) and for every
// height lookup (the bilinear ground's t and t + 1, the gradient's t - 1 and t + 1, past a quadrant's edge
// kept in the quadrant where the .trn locks the axis and run across the atlas where it does not [orig: @
// 0x6068FA..0x60694E]): an island, a tiled layout, the wraps and clamps past the grid (the edge cell's
// quadrant repeating in every sector past a clamped edge), empty sectors, both inverses against a brute
// walk of every unit through the routing and the shared tap kernel (coords_taps_for_quadrant); the map rect
// of every map width; INT32_MIN routed without UB.
#include <runtime/terrain_query/atlas_routing.h>
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/terrain_query/terrain_atlas_rects.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "common/test_expect.h"

namespace {

using namespace opennova::terrain;

constexpr int32_t kQ16 = 65536;

int32_t fx(int32_t units) { return units * kQ16; }

// The routing as surface_type_at_fixed held it before the lift, verbatim: a regression pin that the lift
// changed no sampler (the retail pins are the hand-routed cases of test_island and test_wraps).
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
	// Every corner unit of the world rect routes into the atlas rect (x right, z = -mission y down).
	const int32_t xs[2] = {r.x0, r.x1 - 1}, zs[2] = {r.z0, r.z1 - 1};
	for (int32_t x : xs)
		for (int32_t z : zs) {
			const TerrainAtlasTexel t = terrain_atlas_texel_at(routing, fx(x), -fx(z));
			if (!t.mapped || t.x < atlas.x || t.x >= atlas.x + atlas.w || t.z < atlas.z || t.z >= atlas.z + atlas.h)
				return false;
		}
	return true;
}

// The units of `bounds` (sizes up to a few sectors) as a bitmap, set by the inverse's rects.
struct UnitMap {
	TerrainWorldRect bounds;
	std::vector<uint8_t> set;
	explicit UnitMap(const TerrainWorldRect &b)
	    : bounds(b), set(size_t(b.x1 - b.x0) * size_t(b.z1 - b.z0), 0) {}
	uint8_t &at(int32_t x, int32_t z) { return set[size_t(z - bounds.z0) * size_t(bounds.x1 - bounds.x0) + size_t(x - bounds.x0)]; }
};

// The inverse against a brute walk: every unit of the bounds routed (terrain_atlas_texel_at) and its taps
// taken through the shared tap kernel (coords_taps_for_quadrant, the heights' samplers' own), hit where a tap
// on each axis lands in the atlas rect; the rects must cover exactly those units, each once.
bool inverse_matches_walk(const TerrainAtlasRouting &routing, const CoordsQuadrantLocks &locks, const CoordsRect &atlas,
                          const TerrainWorldRect &bounds, TerrainAtlasTaps taps) {
	UnitMap want(bounds), got(bounds);
	for (int32_t z = bounds.z0; z < bounds.z1; ++z)
		for (int32_t x = bounds.x0; x < bounds.x1; ++x) {
			const TerrainAtlasTexel t = terrain_atlas_texel_at(routing, fx(x), -fx(z));
			if (!t.mapped) continue;
			const int qx = (t.slot & 2) != 0 ? 512 : 0, qz = (t.slot & 1) != 0 ? 512 : 0;
			const CoordsTaps kernel = coords_taps_for_quadrant(locks, qx, qz, COORDS_ATLAS_SIZE);
			bool hit_x = false, hit_z = false;
			for (int32_t k = -taps.before; k <= taps.after; ++k) {
				const int tx = kernel.x(t.x + k), tz = kernel.z(t.z + k);
				hit_x |= atlas.x <= tx && tx < atlas.x + atlas.w;
				hit_z |= atlas.z <= tz && tz < atlas.z + atlas.h;
			}
			if (hit_x && hit_z) want.at(x, z) = 1;
		}
	for (const TerrainWorldRect &r : terrain_atlas_world_rects(routing, atlas, bounds, taps, locks)) {
		if (r.x0 < bounds.x0 || r.z0 < bounds.z0 || r.x1 > bounds.x1 || r.z1 > bounds.z1 || r.x0 >= r.x1 || r.z0 >= r.z1)
			return false;
		for (int32_t z = r.z0; z < r.z1; ++z)
			for (int32_t x = r.x0; x < r.x1; ++x) {
				if (got.at(x, z)) return false; // each unit once
				got.at(x, z) = 1;
			}
	}
	return got.set == want.set;
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
	// Sector order: the first repeat is cell (0, 0), the last cell (3, 3).
	TEST_EXPECT(repeats.front().x0 == 10 && repeats.front().z0 == 20 && repeats.back().x0 == 3 * 512 + 10 &&
	            repeats.back().z0 == 3 * 512 + 20);
	// The other quadrants are drawn nowhere.
	TEST_EXPECT(terrain_atlas_world_rects(routing, CoordsRect{512, 0, 512, 1024}).empty());
	std::printf("tiled: a repeated quadrant inverts to every repeat\n");
	return 0;
}

int test_wraps() {
	// Cells (0, 0) = 1, (0, 15) = 1 and (15, 15) = 4, the grid from (0, 0).
	int grid[16][16] = {};
	grid[0][0] = 1;
	grid[0][15] = 1;
	grid[15][15] = 4;
	const TerrainAtlasRouting wrapped{&grid[0][0], 0, 0, true, true};
	const TerrainAtlasRouting clamped{&grid[0][0], 0, 0, false, false};
	// Column 16, row 0: wraps to column 0; clamps to column 15 (mapped too, the same quadrant).
	const TerrainAtlasTexel w16 = terrain_atlas_texel_at(wrapped, fx(16 * 512 + 5), -fx(7));
	TEST_EXPECT(w16.mapped && w16.cell_col == 0 && w16.cell_row == 0 && w16.x == 5 && w16.z == 7);
	const TerrainAtlasTexel c16 = terrain_atlas_texel_at(clamped, fx(16 * 512 + 5), -fx(7));
	TEST_EXPECT(c16.mapped && c16.cell_col == 15 && c16.cell_row == 0 && c16.x == 5);
	// Column -1, row -1: wraps to (15, 15); clamps to (0, 0).
	const TerrainAtlasTexel wm = terrain_atlas_texel_at(wrapped, fx(-512 + 3), -fx(-512 + 9));
	TEST_EXPECT(wm.mapped && wm.cell_col == 15 && wm.cell_row == 15 && wm.x == 512 + 3 && wm.z == 512 + 9);
	const TerrainAtlasTexel cm = terrain_atlas_texel_at(clamped, fx(-512 + 3), -fx(-512 + 9));
	TEST_EXPECT(cm.mapped && cm.cell_col == 0 && cm.cell_row == 0 && cm.x == 3 && cm.z == 9);
	// The grid's own sectors: three mapped cells.
	TEST_EXPECT(terrain_atlas_world_rects(wrapped, CoordsRect{0, 0, 1024, 1024}).size() == 3);
	// Past a clamped edge the edge cell's quadrant repeats in every sector (the routing keeps x & 511): texel
	// 100 shows at x 7780 (sector 15, in the grid), 8292, 8804 and 10340 (sectors 16, 17, 20) alike.
	const CoordsRect t100{100, 0, 1, 1};
	const TerrainWorldRect past{15 * 512, 0, 21 * 512, 512};
	const std::vector<TerrainWorldRect> repeats = terrain_atlas_world_rects(clamped, t100, past);
	TEST_EXPECT(repeats.size() == 6);
	for (const TerrainWorldRect &r : repeats) TEST_EXPECT(r.x1 - r.x0 == 1 && (r.x0 - 100) % 512 == 0 && r.z0 == 0);
	for (const int32_t x : {7780, 8292, 8804, 10340})
		TEST_EXPECT(terrain_atlas_texel_at(clamped, fx(x), 0).mapped && terrain_atlas_texel_at(clamped, fx(x), 0).x == 100);
	// Past a wrapped edge the grid repeats: sectors 16..20 are cells 0..4, only cell 0 mapped on row 0.
	const std::vector<TerrainWorldRect> wrapped_repeats = terrain_atlas_world_rects(wrapped, t100, past);
	TEST_EXPECT(wrapped_repeats.size() == 2 && wrapped_repeats[0].x0 == 15 * 512 + 100 && wrapped_repeats[1].x0 == 16 * 512 + 100);
	std::printf("wraps: a wrapped axis repeats the grid, a clamped one the edge cell's quadrant\n");
	return 0;
}

int test_inverse_against_walk() {
	// The four quadrants over a 2 x 2 block from (0, 0), quadrant 0 locked on x, quadrant 3 on z; the rects
	// against the walk for the one-texel lookups and every height lookup, at a quadrant's first column (the
	// unlocked neighbour's last unit reads it), at the atlas's first column (the wrap of 1023 + 1), across the
	// z seam, at the atlas's last column, and inside a quadrant.
	int grid[16][16] = {};
	grid[0][0] = 1; // quadrant (0, 0)
	grid[0][1] = 3; // (512, 0)
	grid[1][0] = 2; // (0, 512)
	grid[1][1] = 4; // (512, 512)
	const TerrainAtlasRouting routing{&grid[0][0], 0, 0, false, false};
	CoordsQuadrantLocks locks;
	locks.set(0, true, false);
	locks.set(3, false, true);
	const TerrainWorldRect bounds{-20, -20, 1100, 1100};
	for (const CoordsRect &atlas : {CoordsRect{512, 100, 1, 1}, CoordsRect{0, 0, 2, 2}, CoordsRect{200, 510, 5, 4},
	                                CoordsRect{1023, 300, 1, 1}, CoordsRect{700, 700, 30, 20}, CoordsRect{511, 0, 2, 1024}}) {
		TEST_EXPECT(inverse_matches_walk(routing, locks, atlas, bounds, kTerrainNearestTaps));
		TEST_EXPECT(inverse_matches_walk(routing, locks, atlas, bounds, kTerrainHeightTaps));
	}
	// A height change at quadrant (512, 512)'s first column (atlas x 512, z 600) shows in the last unit of the
	// sector drawing unlocked quadrant (0, 512), whose bilinear x + 1 tap runs across the atlas into it; quadrant
	// (0, 0), locked on x, reads its own first column there instead.
	TerrainHeightField field;
	field.layout.sector_grid = &grid[0][0];
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	field.wrap_x = false;
	field.wrap_z = true;
	field.locks = locks;
	const TerrainAtlasRouting from_field = terrain_atlas_routing(field);
	TEST_EXPECT(from_field.sector_grid == &grid[0][0] && from_field.origin_x == 0 && from_field.origin_y == 0 &&
	            !from_field.wrap_x && from_field.wrap_z);
	field.layout.origin_x = 3;
	field.layout.origin_y = -2;
	field.wrap_x = true;
	field.wrap_z = false;
	const TerrainAtlasRouting swapped = terrain_atlas_routing(field);
	TEST_EXPECT(swapped.origin_x == 3 && swapped.origin_y == -2 && swapped.wrap_x && !swapped.wrap_z);
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	field.wrap_x = false;
	const CoordsRect seam{512, 600, 1, 1};
	const std::vector<TerrainWorldRect> heights = terrain_height_world_rects(field, seam, bounds);
	const std::vector<TerrainWorldRect> same =
	        terrain_atlas_world_rects(terrain_atlas_routing(field), seam, bounds, kTerrainHeightTaps, locks);
	TEST_EXPECT(heights.size() == same.size() && !heights.empty());
	bool neighbour = false, locked_own = false;
	for (const TerrainWorldRect &r : heights) {
		neighbour |= r.x0 == 511 && r.x1 == 512 && r.z0 == 512 + 87 && r.z1 == 512 + 90; // (0, 512)'s last column
		locked_own |= r.x0 == 511 && r.z1 <= 512; // (0, 0)'s last column: none
	}
	TEST_EXPECT(neighbour && !locked_own);
	std::printf("inverse: the rects are exactly the units the walk finds, one-texel and height taps, locks and seams\n");
	return 0;
}

int test_samplers() {
	// A mixed layout with empty cells, both wraps, and maps of 256 and 1024: the samplers read the texel the
	// routing names, as the inline routing they replace did (a regression pin, old_route above).
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
	// INT32_MIN on both axes routes without UB, as retail's NEG/SAR has it: -y stays INT32_MIN, both cells
	// -64 (the grid's cell 0 under a wrap from origin 0), both offsets 0.
	int one[16][16] = {};
	one[0][0] = 4;
	const int32_t lowest = std::numeric_limits<int32_t>::min();
	const TerrainAtlasTexel low = terrain_atlas_texel_at(TerrainAtlasRouting{&one[0][0], 0, 0, true, true}, lowest, lowest);
	TEST_EXPECT(low.mapped && low.cell_col == 0 && low.cell_row == 0 && low.x == 512 && low.z == 512);
	std::printf("samplers: the char map and the foliage map read the routed texel under every wrap; INT32_MIN routed\n");
	return 0;
}

int test_map_rects() {
	// A map's rect in atlas texels: 256 wide shifts by 2, 512 by 1, 1024 by 0, clipped to the atlas.
	const CoordsRect r256 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 256);
	TEST_EXPECT(r256.x == 4 && r256.z == 8 && r256.w == 12 && r256.h == 16);
	const CoordsRect r512 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 512);
	TEST_EXPECT(r512.x == 2 && r512.w == 6);
	const CoordsRect r1024 = terrain_map_atlas_rect(CoordsRect{1, 2, 3, 4}, 1024);
	TEST_EXPECT(r1024.x == 1 && r1024.h == 4);
	// 600 wide samples as 512 (shift 1): texel 300 is read, texel 520 never.
	const CoordsRect r600 = terrain_map_atlas_rect(CoordsRect{300, 0, 1, 1}, 600);
	TEST_EXPECT(r600.x == 600 && r600.w == 2);
	TEST_EXPECT(terrain_map_atlas_rect(CoordsRect{520, 0, 1, 1}, 600).w == 0);
	// 2048 wide or more: every position reads texel (0, 0), which covers the atlas; any other texel nothing.
	const CoordsRect r2048 = terrain_map_atlas_rect(CoordsRect{0, 0, 1, 1}, 2048);
	TEST_EXPECT(r2048.x == 0 && r2048.z == 0 && r2048.w == 1024 && r2048.h == 1024);
	TEST_EXPECT(terrain_map_atlas_rect(CoordsRect{1, 0, 1, 1}, 4096).w == 0);
	// A negative origin or no size: nothing.
	TEST_EXPECT(terrain_map_atlas_rect(CoordsRect{-1, 0, 2, 2}, 256).w == 0);
	TEST_EXPECT(terrain_map_atlas_rect(CoordsRect{0, 0, 0, 2}, 256).w == 0);
	std::printf("map rects: every width's shift, the 2048-wide texel 0, refusals\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_island() + test_tiled() + test_wraps() + test_inverse_against_walk() + test_samplers() +
	                     test_map_rects();
	if (failures == 0) std::printf("terrain_atlas_routing: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
