// The region bake's cost (by hand, not a ctest): a rolling map baked whole with its state, then brushes of a
// few sizes baked again over their region, each node level's count and time, on one thread and on every
// hardware thread. Usage: trngen_region_bake_bench [threads]
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/cpt/trngen/terrain_bake_state.h>
#include <formats/cpt/trngen/terrain_region_bake.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::trngen;

namespace {

std::vector<uint16_t> ground() {
	std::vector<uint16_t> depth(static_cast<size_t>(kDepthSide) * kDepthSide);
	for (int y = 0; y < kDepthSide; ++y)
		for (int x = 0; x < kDepthSide; ++x) {
			double h = 30.0 + 12.0 * std::sin(x * 0.011) * std::cos(y * 0.017) + 6.0 * std::sin((x + 2 * y) * 0.043);
			h += 25.0 * std::exp(-((x - 600) * (x - 600) + (y - 420) * (y - 420)) / 9000.0);
			depth[static_cast<size_t>(y) * kDepthSide + x] = static_cast<uint16_t>(h * 256.0);
		}
	return depth;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char **argv) {
	const unsigned many = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : TerrainBakeInput::for_hardware();
	TerrainBakeInput input;
	input.depth16 = ground();
	input.threads = many;
	CptFile cpt;
	TerrainBakeState state;
	std::string error;
	auto start = std::chrono::steady_clock::now();
	if (!bake_terrain_state(input, cpt, state, error)) {
		std::printf("the whole bake failed: %s\n", error.c_str());
		return 1;
	}
	std::printf("whole bake with its state, %u threads: %.3f s\n", many, seconds_since(start));

	struct Brush {
		const char *name;
		DepthRect rect;
	};
	const Brush brushes[] = {
		{"8 texels inside a leaf", {300, 140, 8, 8}},
		{"32 texels across a 64 corner (a 32 m brush)", {624, 624, 32, 32}},
		{"128 texels", {200, 600, 128, 128}},
		{"256 texels across the quadrants", {384, 384, 256, 256}},
	};
	for (unsigned threads : {1u, many}) {
		for (const Brush &brush : brushes) {
			std::vector<uint16_t> map = input.depth16;
			for (int y = brush.rect.y; y < brush.rect.y + brush.rect.h; ++y)
				for (int x = brush.rect.x; x < brush.rect.x + brush.rect.w; ++x)
					map[static_cast<size_t>(y) * kDepthSide + static_cast<size_t>(x)] += 384;
			RegionBakeInput region;
			region.depth16 = &map;
			region.dirty = {brush.rect};
			region.threads = threads;
			RegionBakeResult result;
			start = std::chrono::steady_clock::now();
			if (!bake_terrain_region(state, region, result, error)) {
				std::printf("the region bake failed: %s\n", error.c_str());
				return 1;
			}
			const double total = seconds_since(start);
			std::printf("%-46s %u thread(s): %.3f s;", brush.name, threads, total);
			for (int depth = 4; depth >= 1; --depth) {
				const RegionBakeLevelCost &cost = result.cost[static_cast<size_t>(depth)];
				std::printf(" depth %d: %d node(s) %.3f s (%.3f s a node)", depth, cost.nodes, cost.seconds,
				            cost.nodes ? cost.seconds / cost.nodes : 0.0);
			}
			std::printf("; tiles %.3f s, draw-back %.3f s, inside %.3f s\n", result.tile_seconds,
			            result.draw_back_seconds, result.seconds);
		}
	}
	return 0;
}
