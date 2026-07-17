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

	opennova::FoliageMap flat_map = opennova::foliage_make_default_map(256, 256, 255);
	opennova::foliage_set_index(flat_map, 226, 250, 37);
	int detail_map_x = -1;
	int detail_map_y = -1;
	if (!expect(opennova::foliage_detail_sample_resolution(flat_map) == 256 &&
	                opennova::foliage_detail_flat_wrap_position(
	                    flat_map, -120 * 65536, -24 * 65536,
	                    detail_map_x, detail_map_y) &&
	                detail_map_x == 226 && detail_map_y == 250,
	            "detail authoring coordinates should share the runtime flat-map policy"))
		return 1;
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    flat_map, -120 * 65536, -24 * 65536) == 37,
	            "detail lookup should use the retail flat negative-coordinate witness"))
		return 1;
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    flat_map, 904 * 65536, 1000 * 65536) == 37,
	            "detail lookup should repeat every 1024 world units"))
		return 1;
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    flat_map, -(119 * 65536 + 16384), -(23 * 65536 + 16384)) == 37,
	            "detail lookup should retain retail Q16 negative-fraction semantics"))
		return 1;

	opennova::FoliageMap boundary_map = opennova::foliage_make_default_map(256, 256, 0);
	opennova::foliage_set_index(boundary_map, 124, 0, 73);
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    boundary_map, 500 * 65536 - 1, 0) == 73,
	            "detail lookup should preserve fixed-point precision at integer boundaries"))
		return 1;

	opennova::FoliageMap non_power_of_two = opennova::foliage_make_default_map(300, 300, 0);
	opennova::foliage_set_index(non_power_of_two, 226, 250, 91);
	if (!expect(opennova::foliage_detail_sample_resolution(non_power_of_two) == 256 &&
	                opennova::foliage_sample_detail_flat_wrap(
	                    non_power_of_two, -120 * 65536, -24 * 65536) == 91,
	            "detail lookup should derive floor(log2(width)) exactly like the retail loader"))
		return 1;
	opennova::FoliageMap wrapped_paint =
	    opennova::foliage_make_default_map(300, 300, 0);
	if (!expect(opennova::foliage_paint_detail_circle_wrap(
	                    wrapped_paint, 255, 255, 2, 1.0f, 1.0f, 19) &&
	                opennova::foliage_get_index(wrapped_paint, 255, 255) == 19 &&
	                opennova::foliage_get_index(wrapped_paint, 0, 255) == 19 &&
	                opennova::foliage_get_index(wrapped_paint, 1, 255) == 19 &&
	                opennova::foliage_get_index(wrapped_paint, 255, 0) == 19 &&
	                opennova::foliage_get_index(wrapped_paint, 255, 1) == 19 &&
	                opennova::foliage_get_index(wrapped_paint, 256, 255) == 0 &&
	                opennova::foliage_get_index(wrapped_paint, 255, 256) == 0,
	            "detail painting should wrap at the effective-resolution seam without touching unused stride cells"))
		return 1;

	opennova::FoliageMap wide_stride = opennova::foliage_make_default_map(1025, 1025, 0);
	opennova::foliage_set_index(wide_stride, 904, 1000, 117);
	if (!expect(opennova::foliage_detail_sample_resolution(wide_stride) == 1024 &&
	                opennova::foliage_sample_detail_flat_wrap(
	                    wide_stride, -120 * 65536, -24 * 65536) == 117,
	            "detail lookup should retain valid exponent-10 maps and their actual-width stride"))
		return 1;

	opennova::FoliageMap oversized_map = opennova::foliage_make_default_map(2048, 1, 99);
	if (!expect(opennova::foliage_detail_sample_resolution(oversized_map) == 0 &&
	                opennova::foliage_sample_detail_flat_wrap(oversized_map, 0, 0) == 0,
	            "detail lookup should reject dimensions that make retail's shift invalid"))
		return 1;

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
