#include "foliage/foliage.h"

// Engine: jodemo.exe foliage-def normalization and world->foliagemap helpers
// consumed by sub_5C0240@0x5C0240 / sub_5C65E0@0x5C65E0
// [orig: sub_5C0240 @ 0x5C0240 / sub_5C65E0 @ 0x5C65E0 (jodemo); docs/foliage/foliage-re.md]
// docs/engine_spec_foliage.md 2.3, 4.4.4, 8

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova {

namespace {

template <typename T>
static T clamp_cast(int value, int min_value, int max_value) {
	return static_cast<T>(std::clamp(value, min_value, max_value));
}

static int detail_map_exponent(const FoliageMap &map) {
	// [orig: Foliage_LoadFoliageMapPCX @ 0x605ad0]
	if (!foliage_has_size(map)) {
		return -1;
	}

	int exponent = 0;
	for (unsigned width = static_cast<unsigned>(map.width); width > 1; width >>= 1u) {
		++exponent;
	}
	return exponent <= 10 ? exponent : -1;
}

static void foliage_fill_grayscale_palette(uint8_t palette[256][3]) {
	for (int i = 0; i < 256; ++i) {
		palette[i][0] = static_cast<uint8_t>(i);
		palette[i][1] = static_cast<uint8_t>(i);
		palette[i][2] = static_cast<uint8_t>(i);
	}
}

static bool foliage_is_valid_index(const FoliageMap &map, int x, int y) {
	return foliage_has_size(map) && x >= 0 && y >= 0 && x < map.width && y < map.height;
}

static bool foliage_detail_flat_wrap_position(const FoliageMap &map,
                                              int32_t world_x_fixed,
                                              int32_t world_z_fixed,
                                              int &map_x, int &map_y) {
	// [orig: Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0]
	// Retail applies the loader's width exponent to both wrapped axes. Its
	// sampler receives retail Z and negates it; world_z_fixed is already
	// -retailZ in the reimpl plane.
	map_x = -1;
	map_y = -1;
	const int exponent = detail_map_exponent(map);
	if (exponent < 0) {
		return false;
	}

	const int shift = 10 - exponent;
	map_x = static_cast<int>(((static_cast<uint32_t>(world_x_fixed) >> 16u) & 0x3ffu) >> shift);
	map_y = static_cast<int>(((static_cast<uint32_t>(world_z_fixed) >> 16u) & 0x3ffu) >> shift);
	return map_x < map.width && map_y < map.height;
}

} // namespace

int foliage_normalize_color_mode(int value) {
	if (value < static_cast<int>(FoliageColorMode::MatchGround)) {
		return static_cast<int>(FoliageColorMode::MatchGround);
	}
	if (value > static_cast<int>(FoliageColorMode::RetainFullColor)) {
		return static_cast<int>(FoliageColorMode::RetainFullColor);
	}
	return value;
}

uint8_t foliage_normalize_attrib_flags(uint8_t flags) {
	return static_cast<uint8_t>(flags & FOLIAGE_ATTRIB_KNOWN_MASK);
}

FoliageDef foliage_normalize_def(const FoliageDef &def) {
	FoliageDef normalized = def;
	normalized.color_lower = foliage_normalize_color_mode(normalized.color_lower);
	normalized.color_upper = foliage_normalize_color_mode(normalized.color_upper);
	normalized.match = std::clamp(normalized.match, -1, 255);
	normalized.attrib_flags = foliage_normalize_attrib_flags(normalized.attrib_flags);
	return normalized;
}

FoliageMap foliage_make_default_map(int width, int height, uint8_t fill_index) {
	FoliageMap map;
	if (width <= 0 || height <= 0) {
		return map;
	}
	map.width = width;
	map.height = height;
	map.indices.assign(static_cast<size_t>(width * height), fill_index);
	foliage_fill_grayscale_palette(map.palette);
	return map;
}

bool foliage_has_size(const FoliageMap &map) {
	return map.width > 0 && map.height > 0 &&
	       map.indices.size() >= static_cast<size_t>(map.width * map.height);
}

uint8_t foliage_get_index(const FoliageMap &map, int x, int y) {
	if (!foliage_is_valid_index(map, x, y)) {
		return 0;
	}
	return map.indices[static_cast<size_t>(y * map.width + x)];
}

uint8_t foliage_sample_detail_flat_wrap(const FoliageMap &map,
                                        int32_t world_x_fixed,
                                        int32_t world_z_fixed) {
	int map_x = -1;
	int map_y = -1;
	if (!foliage_detail_flat_wrap_position(
	        map, world_x_fixed, world_z_fixed, map_x, map_y)) {
		return 0;
	}
	return foliage_get_index(map, map_x, map_y);
}

int foliage_map_x_from_heightmap_x(float hm_x, int map_width, int hm_size) {
	if (map_width <= 0 || hm_size <= 0) {
		return -1;
	}
	const float clamped = std::clamp(hm_x, 0.0f, static_cast<float>(hm_size) - 0.001f);
	return clamp_cast<int>(static_cast<int>(std::floor(clamped * static_cast<float>(map_width) / static_cast<float>(hm_size))),
	                       0,
	                       map_width - 1);
}

int foliage_map_y_from_heightmap_y(float hm_y, int map_height, int hm_size) {
	if (map_height <= 0 || hm_size <= 0) {
		return -1;
	}
	const float clamped = std::clamp(hm_y, 0.0f, static_cast<float>(hm_size) - 0.001f);
	return clamp_cast<int>(static_cast<int>(std::floor(clamped * static_cast<float>(map_height) / static_cast<float>(hm_size))),
	                       0,
	                       map_height - 1);
}

} // namespace opennova
