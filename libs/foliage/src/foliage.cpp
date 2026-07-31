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

static float smoothstep(float t) {
	const float u = std::clamp(t, 0.0f, 1.0f);
	return u * u * (3.0f - 2.0f * u);
}

static bool circle_pixel_selected(int dx, int dy, int radius,
                                  float hardness, float strength) {
	const float radius_f = static_cast<float>(radius);
	const float dx_f = static_cast<float>(dx);
	const float dy_f = static_cast<float>(dy);
	const float dist_sq = dx_f * dx_f + dy_f * dy_f;
	if (dist_sq > radius_f * radius_f) {
		return false;
	}

	const float distance = std::sqrt(dist_sq);
	const float t = 1.0f - distance / radius_f;
	const float shoulder = 1.0f - std::clamp(hardness, 0.0f, 1.0f);
	float falloff = 1.0f;
	if (shoulder > 0.0001f && t < shoulder) {
		falloff = smoothstep(t / shoulder);
	}
	return falloff * std::clamp(strength, 0.0f, 1.0f) >= 0.5f;
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

static int wrap_detail_index(int value, int resolution) {
	const int wrapped = value % resolution;
	return wrapped < 0 ? wrapped + resolution : wrapped;
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

void foliage_fill_grayscale_palette(uint8_t palette[256][3]) {
	for (int i = 0; i < 256; ++i) {
		palette[i][0] = static_cast<uint8_t>(i);
		palette[i][1] = static_cast<uint8_t>(i);
		palette[i][2] = static_cast<uint8_t>(i);
	}
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

bool foliage_is_valid_index(const FoliageMap &map, int x, int y) {
	return foliage_has_size(map) && x >= 0 && y >= 0 && x < map.width && y < map.height;
}

uint8_t foliage_get_index(const FoliageMap &map, int x, int y) {
	if (!foliage_is_valid_index(map, x, y)) {
		return 0;
	}
	return map.indices[static_cast<size_t>(y * map.width + x)];
}

bool foliage_set_index(FoliageMap &map, int x, int y, uint8_t index) {
	if (!foliage_is_valid_index(map, x, y)) {
		return false;
	}
	const size_t offset = static_cast<size_t>(y * map.width + x);
	if (map.indices[offset] == index) {
		return false;
	}
	map.indices[offset] = index;
	return true;
}

bool foliage_paint_circle(FoliageMap &map,
                          int center_x,
                          int center_y,
                          int radius,
                          float hardness,
                          float strength,
                          uint8_t index) {
	if (!foliage_has_size(map) || radius <= 0 || strength <= 0.0f) {
		return false;
	}

	const int x_min = std::max(center_x - radius, 0);
	const int y_min = std::max(center_y - radius, 0);
	const int x_max = std::min(center_x + radius + 1, map.width);
	const int y_max = std::min(center_y + radius + 1, map.height);
	if (x_max <= x_min || y_max <= y_min) {
		return false;
	}

	bool changed = false;

	for (int y = y_min; y < y_max; ++y) {
		for (int x = x_min; x < x_max; ++x) {
			if (!circle_pixel_selected(
			        x - center_x, y - center_y, radius, hardness, strength)) {
				continue;
			}
			changed |= foliage_set_index(map, x, y, index);
		}
	}

	return changed;
}

bool foliage_paint_detail_circle_wrap(FoliageMap &map,
                                      int center_x,
                                      int center_y,
                                      int radius,
                                      float hardness,
                                      float strength,
                                      uint8_t index) {
	const int resolution = foliage_detail_sample_resolution(map);
	if (resolution <= 0 || radius <= 0 || strength <= 0.0f) {
		return false;
	}

	bool changed = false;
	for (int dy = -radius; dy <= radius; ++dy) {
		for (int dx = -radius; dx <= radius; ++dx) {
			if (!circle_pixel_selected(dx, dy, radius, hardness, strength)) {
				continue;
			}
			const int x = wrap_detail_index(center_x + dx, resolution);
			const int y = wrap_detail_index(center_y + dy, resolution);
			changed |= foliage_set_index(map, x, y, index);
		}
	}
	return changed;
}

int foliage_count_index(const FoliageMap &map, uint8_t index) {
	if (!foliage_has_size(map)) {
		return 0;
	}
	int count = 0;
	for (uint8_t value : map.indices) {
		if (value == index) {
			++count;
		}
	}
	return count;
}

int foliage_remap_index(FoliageMap &map, uint8_t from_index, uint8_t to_index) {
	if (!foliage_has_size(map) || from_index == to_index) {
		return 0;
	}
	int changed = 0;
	for (uint8_t &value : map.indices) {
		if (value != from_index) {
			continue;
		}
		value = to_index;
		++changed;
	}
	return changed;
}

int foliage_remap_indices(FoliageMap &map, const std::array<uint8_t, 256> &lut) {
	if (!foliage_has_size(map)) {
		return 0;
	}
	int changed = 0;
	for (uint8_t &value : map.indices) {
		const uint8_t next = lut[value];
		if (next == value) {
			continue;
		}
		value = next;
		++changed;
	}
	return changed;
}

int foliage_detail_sample_resolution(const FoliageMap &map) {
	const int exponent = detail_map_exponent(map);
	return exponent >= 0 ? (1 << exponent) : 0;
}

bool foliage_detail_flat_wrap_position(const FoliageMap &map,
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

float foliage_heightmap_x_from_map_x(int map_x, int map_width, int hm_size) {
	if (map_width <= 0 || hm_size <= 0) {
		return 0.0f;
	}
	const int clamped = std::clamp(map_x, 0, map_width - 1);
	return static_cast<float>(clamped) * static_cast<float>(hm_size) / static_cast<float>(map_width);
}

float foliage_heightmap_y_from_map_y(int map_y, int map_height, int hm_size) {
	if (map_height <= 0 || hm_size <= 0) {
		return 0.0f;
	}
	const int clamped = std::clamp(map_y, 0, map_height - 1);
	return static_cast<float>(clamped) * static_cast<float>(hm_size) / static_cast<float>(map_height);
}

int foliage_sector_id_from_heightmap(float hm_x, float hm_y, int hm_size) {
	const float midpoint = static_cast<float>(hm_size) * 0.5f;
	const bool east = hm_x >= midpoint;
	const bool south = hm_y >= midpoint;
	if (!east && !south) {
		return 1;
	}
	if (east && !south) {
		return 3;
	}
	if (!east && south) {
		return 2;
	}
	return 4;
}

} // namespace opennova
