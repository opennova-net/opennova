#include <terrain/quadtree.h>

#include <cstdio>

int main() {
	for (int sublevel = 0; sublevel < 16; ++sublevel) {
		const int expected = sublevel / 2;
		const int actual = opennova::terrain_lod_family(sublevel);
		if (actual != expected) {
			std::fprintf(stderr, "FAIL: sublevel %d selected family %d, expected %d\n",
				sublevel, actual, expected);
			return 1;
		}
	}
	if (opennova::terrain_lod_family(-1) != 0 ||
			opennova::terrain_lod_family(16) != 7) {
		std::fprintf(stderr, "FAIL: terrain LOD family clamp\n");
		return 1;
	}
	std::puts("OK: all 16 terrain sublevels select the recovered eight LOD families");
	return 0;
}
