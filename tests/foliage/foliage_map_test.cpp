#include <formats/foliage/foliage.h>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void set_index(opennova::FoliageMap &map, int x, int y, uint8_t value) {
	map.indices[static_cast<size_t>(y * map.width + x)] = value;
}

} // namespace

int main() {
	opennova::FoliageMap map = opennova::foliage_make_default_map(256, 256, 0);
	if (!expect(opennova::foliage_has_size(map), "default foliage map should allocate indices")) return 1;
	if (!expect(map.width == 256 && map.height == 256, "default foliage map dimensions should round-trip")) return 1;
	if (!expect(opennova::foliage_get_index(map, 10, 10) == 0, "default foliage map should be filled with zero")) return 1;

	opennova::FoliageMap flat_map = opennova::foliage_make_default_map(256, 256, 255);
	set_index(flat_map, 226, 250, 37);
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
	set_index(boundary_map, 124, 0, 73);
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    boundary_map, 500 * 65536 - 1, 0) == 73,
	            "detail lookup should preserve fixed-point precision at integer boundaries"))
		return 1;

	opennova::FoliageMap non_power_of_two = opennova::foliage_make_default_map(300, 300, 0);
	set_index(non_power_of_two, 226, 250, 91);
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    non_power_of_two, -120 * 65536, -24 * 65536) == 91,
	            "detail lookup should derive floor(log2(width)) exactly like the retail loader"))
		return 1;

	opennova::FoliageMap wide_stride = opennova::foliage_make_default_map(1025, 1025, 0);
	set_index(wide_stride, 904, 1000, 117);
	if (!expect(opennova::foliage_sample_detail_flat_wrap(
	                    wide_stride, -120 * 65536, -24 * 65536) == 117,
	            "detail lookup should retain valid exponent-10 maps and their actual-width stride"))
		return 1;

	opennova::FoliageMap oversized_map = opennova::foliage_make_default_map(2048, 1, 99);
	if (!expect(opennova::foliage_sample_detail_flat_wrap(oversized_map, 0, 0) == 0,
	            "detail lookup should reject dimensions that make retail's shift invalid"))
		return 1;

	const int map_x = opennova::foliage_map_x_from_heightmap_x(512.0f, 256);
	const int map_y = opennova::foliage_map_y_from_heightmap_y(768.0f, 256);
	if (!expect(map_x == 128, "heightmap -> foliage map X conversion should match the authored grid")) return 1;
	if (!expect(map_y == 192, "heightmap -> foliage map Y conversion should match the authored grid")) return 1;

	std::printf("OK: foliage helpers preserve runtime map sampling\n");
	return 0;
}
