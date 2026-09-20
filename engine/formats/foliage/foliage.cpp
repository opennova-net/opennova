#include <formats/foliage/foliage.h>

// Foliage-def normalization and world->foliagemap helpers.
// [orig: jodemo Foliage_BuildPatchData @0x5C0240 / Terrain_GetFoliageMapValue @0x5C65E0]
// [orig: Foliage_SampleFoliageMapMask @0x606620; Foliage_GenerateModelTileInstances @0x600980; docs/foliage/foliage-re.md]

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
	// Clamp each code to a byte (retail stores the atoi result as a byte) and
	// compact the authored codes to the front so a saved/reloaded def compares
	// equal; the OR-of-four consumer reads the same set either way.
	std::array<int, FOLIAGE_MATCH_CODES> compact;
	compact.fill(FOLIAGE_MATCH_UNSET);
	int count = 0;
	for (int code : normalized.match) {
		if (code < 0) {
			continue;
		}
		compact[static_cast<size_t>(count++)] = std::clamp(code, 0, 255);
	}
	normalized.match = compact;
	normalized.attrib_flags = foliage_normalize_attrib_flags(normalized.attrib_flags);
	return normalized;
}

int foliage_def_match_count(const FoliageDef &def) {
	int count = 0;
	for (int code : def.match) {
		if (code >= 0) {
			++count;
		}
	}
	return count;
}

// [orig: Foliage_RemapPixelToDefMask @0x5FF4E0 — per slot: header byte 0 skips
//  the slot, then the pixel is compared against the four bytes at +0x108..+0x10B]
bool foliage_def_matches_pixel(const FoliageDef &def, int pixel) {
	if (pixel == 0 || def.graphic.empty()) {
		return false;
	}
	for (int code : def.match) {
		if (code >= 0 && code == pixel) {
			return true;
		}
	}
	return false;
}

// [orig: Foliage_RemapPixelToDefMask @0x5FF4E0 — pixel 0 returns 0 before the
//  slot walk; each matching slot ORs `1 << slot`]
uint32_t foliage_remap_pixel_to_def_mask(const std::vector<FoliageDef> &defs, int pixel) {
	if (pixel == 0) {
		return 0u;
	}
	uint32_t mask = 0u;
	const size_t slots = std::min<size_t>(defs.size(), static_cast<size_t>(FOLIAGE_MAX_DEFS));
	for (size_t slot = 0; slot < slots; ++slot) {
		if (foliage_def_matches_pixel(defs[slot], pixel)) {
			mask |= 1u << slot;
		}
	}
	return mask;
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
