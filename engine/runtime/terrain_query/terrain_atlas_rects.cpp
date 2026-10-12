// Where a change to a terrain's atlas shows (terrain_atlas_rects.h).
#include <runtime/terrain_query/terrain_atlas_rects.h>

#include <algorithm>
#include <array>

namespace opennova::terrain {

namespace {

constexpr int32_t kSector = COORDS_SECTOR_SIZE;
constexpr int32_t kAtlas = COORDS_ATLAS_SIZE;
// What a 16.16 position reaches on each axis, in whole units.
constexpr int64_t kPlaneMin = -32768;
constexpr int64_t kPlaneMax = 32768;

struct Run {
	int32_t begin = 0;
	int32_t end = 0;
};

// The units of a sector drawing the quadrant at `origin` on one axis whose taps read a texel of
// [a0, a1): a tap inside the quadrant is its texel; one past its edge stays in the quadrant (& 511
// from its origin) where the axis is locked and runs across the atlas (& 1023) where it is not
// [orig: Terrain_SampleHeightBilinear @ 0x6068FA..0x60694E].
std::vector<Run> axis_runs(int32_t origin, bool locked, int32_t a0, int32_t a1, TerrainAtlasTaps taps) {
	std::vector<Run> runs;
	bool open = false;
	int32_t start = 0;
	for (int32_t unit = 0; unit <= kSector; ++unit) {
		bool hit = false;
		if (unit < kSector)
			for (int32_t k = -taps.before; k <= taps.after && !hit; ++k) {
				const int32_t local = unit + k;
				int32_t texel = origin + local;
				if (local < 0 || local >= kSector)
					texel = locked ? origin + (local & (kSector - 1)) : (texel & (kAtlas - 1));
				hit = a0 <= texel && texel < a1;
			}
		if (hit && !open) {
			open = true;
			start = unit;
		} else if (!hit && open) {
			open = false;
			runs.push_back({start, unit});
		}
	}
	return runs;
}

int64_t floor_div(int64_t a, int64_t b) {
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

} // namespace

TerrainAtlasRouting terrain_atlas_routing(const TerrainHeightField &field) {
	return TerrainAtlasRouting{field.layout.sector_grid, field.layout.origin_x, field.layout.origin_y, field.wrap_x,
	                           field.wrap_z};
}

std::vector<TerrainWorldRect> terrain_atlas_world_rects(const TerrainAtlasRouting &routing, const CoordsRect &atlas,
		const TerrainWorldRect &bounds, TerrainAtlasTaps taps, const CoordsQuadrantLocks &locks) {
	std::vector<TerrainWorldRect> out;
	if (routing.sector_grid == nullptr || taps.before < 0 || taps.after < 0) return out;
	const int32_t ax0 = std::max(atlas.x, 0), az0 = std::max(atlas.z, 0);
	const int32_t ax1 = static_cast<int32_t>(std::min<int64_t>(int64_t(atlas.x) + atlas.w, kAtlas));
	const int32_t az1 = static_cast<int32_t>(std::min<int64_t>(int64_t(atlas.z) + atlas.h, kAtlas));
	const int64_t bx0 = std::max<int64_t>(bounds.x0, kPlaneMin), bz0 = std::max<int64_t>(bounds.z0, kPlaneMin);
	const int64_t bx1 = std::min<int64_t>(bounds.x1, kPlaneMax), bz1 = std::min<int64_t>(bounds.z1, kPlaneMax);
	if (ax0 >= ax1 || az0 >= az1 || bx0 >= bx1 || bz0 >= bz1) return out;
	// Each quadrant's units on each axis, worked out once (by the slot's two low bits, the routing's own
	// pick, and the quadrant's locks).
	std::array<std::vector<Run>, 4> xs, zs;
	for (int32_t slot = 0; slot < 4; ++slot) {
		const int32_t qx = (slot & 2) != 0 ? kSector : 0, qz = (slot & 1) != 0 ? kSector : 0;
		const int quadrant = coords_quadrant_index(qx, qz);
		xs[slot] = axis_runs(qx, locks.locked_x(quadrant), ax0, ax1, taps);
		zs[slot] = axis_runs(qz, locks.locked_z(quadrant), az0, az1, taps);
	}
	// Every sector of the bounds, routed as a position in it is (atlas_routing.h).
	const int64_t sx0 = floor_div(bx0, kSector), sx1 = floor_div(bx1 - 1, kSector);
	const int64_t sz0 = floor_div(bz0, kSector), sz1 = floor_div(bz1 - 1, kSector);
	for (int64_t sz = sz0; sz <= sz1; ++sz) {
		const int32_t row = detail::surface_clamp_grid(static_cast<int32_t>(sz) - routing.origin_y, routing.wrap_z) & 0xF;
		for (int64_t sx = sx0; sx <= sx1; ++sx) {
			const int32_t col = detail::surface_clamp_grid(static_cast<int32_t>(sx) - routing.origin_x, routing.wrap_x) & 0xF;
			const int32_t slot = routing.sector_grid[COORDS_SECTOR_GRID_DIM * row + col] - 1;
			if (slot < 0) continue;
			for (const Run &z : zs[slot & 3])
				for (const Run &x : xs[slot & 3]) {
					const int64_t x0 = std::max<int64_t>(sx * kSector + x.begin, bx0);
					const int64_t x1 = std::min<int64_t>(sx * kSector + x.end, bx1);
					const int64_t z0 = std::max<int64_t>(sz * kSector + z.begin, bz0);
					const int64_t z1 = std::min<int64_t>(sz * kSector + z.end, bz1);
					if (x0 < x1 && z0 < z1)
						out.push_back(TerrainWorldRect{static_cast<int32_t>(x0), static_cast<int32_t>(z0),
						                               static_cast<int32_t>(x1), static_cast<int32_t>(z1)});
				}
		}
	}
	return out;
}

std::vector<TerrainWorldRect> terrain_atlas_world_rects(const TerrainAtlasRouting &routing, const CoordsRect &atlas,
		TerrainAtlasTaps taps, const CoordsQuadrantLocks &locks) {
	const int64_t x0 = int64_t(routing.origin_x) * kSector, z0 = int64_t(routing.origin_y) * kSector;
	const auto clip = [](int64_t v) { return static_cast<int32_t>(std::clamp<int64_t>(v, kPlaneMin, kPlaneMax)); };
	const TerrainWorldRect grid{clip(x0), clip(z0), clip(x0 + COORDS_SECTOR_GRID_DIM * kSector),
	                            clip(z0 + COORDS_SECTOR_GRID_DIM * kSector)};
	return terrain_atlas_world_rects(routing, atlas, grid, taps, locks);
}

std::vector<TerrainWorldRect> terrain_height_world_rects(const TerrainHeightField &field, const CoordsRect &atlas,
		const TerrainWorldRect &bounds) {
	return terrain_atlas_world_rects(terrain_atlas_routing(field), atlas, bounds, kTerrainHeightTaps, field.locks);
}

CoordsRect terrain_map_atlas_rect(const CoordsRect &map_rect, int32_t map_width) {
	if (map_width <= 0 || map_rect.x < 0 || map_rect.z < 0 || map_rect.w <= 0 || map_rect.h <= 0) return CoordsRect{};
	// A map 2048 wide or more: the sample's shift is negative, taken modulo 32 by the processor, so every
	// position reads its texel (0, 0) [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065C6].
	if (detail::surface_shift(map_width) > 10)
		return map_rect.x == 0 && map_rect.z == 0 ? CoordsRect{0, 0, kAtlas, kAtlas} : CoordsRect{};
	const int32_t shift = 10 - detail::surface_shift(map_width);
	const auto at = [shift](int64_t texel) { return static_cast<int32_t>(std::min<int64_t>(texel << shift, kAtlas)); };
	const int32_t x0 = at(map_rect.x), x1 = at(int64_t(map_rect.x) + map_rect.w);
	const int32_t z0 = at(map_rect.z), z1 = at(int64_t(map_rect.z) + map_rect.h);
	if (x0 >= x1 || z0 >= z1) return CoordsRect{};
	return CoordsRect{x0, z0, x1 - x0, z1 - z0};
}

} // namespace opennova::terrain
