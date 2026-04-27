#include <foliage/foliage.h>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	opennova::FoliageMap map = opennova::foliage_make_default_map(256, 256, 0);
	if (!expect(opennova::foliage_has_size(map), "default foliage map should allocate indices")) return 1;
	if (!expect(map.width == 256 && map.height == 256, "default foliage map dimensions should round-trip")) return 1;
	if (!expect(opennova::foliage_get_index(map, 10, 10) == 0, "default foliage map should be filled with zero")) return 1;

	if (!expect(opennova::foliage_paint_circle(map, 128, 128, 8, 0.5f, 1.0f, 7), "paint_circle should modify the map"))
		return 1;
	if (!expect(opennova::foliage_get_index(map, 128, 128) == 7, "paint_circle should write the target foliage index"))
		return 1;
	if (!expect(opennova::foliage_count_index(map, 7) > 0, "count_index should find painted pixels")) return 1;
	if (!expect(opennova::foliage_remap_index(map, 7, 254) > 0, "remap_index should rewrite painted pixels")) return 1;
	if (!expect(opennova::foliage_get_index(map, 128, 128) == 254, "remap_index should rewrite the target foliage index"))
		return 1;
	if (!expect(opennova::foliage_count_index(map, 7) == 0, "remap_index should clear the old index")) return 1;
	if (!expect(opennova::foliage_remap_index(map, 254, 0) > 0, "remap_index should support erase-to-zero")) return 1;
	if (!expect(opennova::foliage_count_index(map, 254) == 0, "erase remap should clear the canonical index")) return 1;

	const int map_x = opennova::foliage_map_x_from_heightmap_x(512.0f, 256);
	const int map_y = opennova::foliage_map_y_from_heightmap_y(768.0f, 256);
	if (!expect(map_x == 128, "heightmap -> foliage map X conversion should match the authored grid")) return 1;
	if (!expect(map_y == 192, "heightmap -> foliage map Y conversion should match the authored grid")) return 1;

	const float hm_x = opennova::foliage_heightmap_x_from_map_x(64, 256);
	const float hm_y = opennova::foliage_heightmap_y_from_map_y(192, 256);
	if (!expect(hm_x == 256.0f, "foliage map -> heightmap X conversion should round-trip")) return 1;
	if (!expect(hm_y == 768.0f, "foliage map -> heightmap Y conversion should round-trip")) return 1;

	if (!expect(opennova::foliage_sector_id_from_heightmap(10.0f, 10.0f) == 1, "NW quadrant should map to sector 1")) return 1;
	if (!expect(opennova::foliage_sector_id_from_heightmap(800.0f, 10.0f) == 3, "NE quadrant should map to sector 3")) return 1;
	if (!expect(opennova::foliage_sector_id_from_heightmap(10.0f, 800.0f) == 2, "SW quadrant should map to sector 2")) return 1;
	if (!expect(opennova::foliage_sector_id_from_heightmap(800.0f, 800.0f) == 4, "SE quadrant should map to sector 4")) return 1;

	std::printf("OK: foliage helpers preserve authored map semantics\n");
	return 0;
}
