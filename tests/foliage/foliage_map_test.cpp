#include <formats/foliage/foliage.h>

#include <cstdio>
#include <vector>

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

	// The foliagemap pixel -> slot mask [orig: Foliage_RemapPixelToDefMask
	// @0x5FF4E0]: OR of 1 << slot over slots whose four codes contain the
	// pixel; pixel 0 never matches; an empty-graphic slot is skipped.
	{
		std::vector<opennova::FoliageDef> defs(4);
		defs[0].graphic = "grass.3di";
		defs[0].match = {254, 10, 20, 30};
		defs[1].graphic = "bush.3di";
		defs[1].match = {253, 30, -1, -1};
		defs[2].graphic = "";
		defs[2].match = {254, -1, -1, -1};
		defs[3].graphic = "tree.3di";
		defs[3].match = {0, 40, -1, -1};
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 254) == 0x1u,
				"the first code selects its slot")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 20) == 0x1u,
				"the third code selects the slot as well")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 30) == 0x3u,
				"a pixel matching two slots ORs both slot bits")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 253) == 0x2u,
				"slot one selects on its own code")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 40) == 0x8u,
				"slot three selects on its second code")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 0) == 0u,
				"pixel 0 never matches, even an authored 0 code")) return 1;
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(defs, 99) == 0u,
				"an unmatched pixel selects nothing")) return 1;
		if (!expect(!opennova::foliage_def_matches_pixel(defs[2], 254),
				"a slot with an empty graphic (header byte 0) is skipped")) return 1;
		std::vector<opennova::FoliageDef> five(5, defs[0]);
		five[4].graphic = "extra.3di";
		five[4].match = {77, -1, -1, -1};
		if (!expect(opennova::foliage_remap_pixel_to_def_mask(five, 77) == 0u,
				"only FOLIAGE_MAX_DEFS slots participate")) return 1;
	}

	const int map_x = opennova::foliage_map_x_from_heightmap_x(512.0f, 256);
	const int map_y = opennova::foliage_map_y_from_heightmap_y(768.0f, 256);
	if (!expect(map_x == 128, "heightmap -> foliage map X conversion should match the authored grid")) return 1;
	if (!expect(map_y == 192, "heightmap -> foliage map Y conversion should match the authored grid")) return 1;

	std::printf("OK: foliage helpers preserve runtime map sampling\n");
	return 0;
}
