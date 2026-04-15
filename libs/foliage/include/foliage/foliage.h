#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

constexpr int FOLIAGE_MAX_DEFS = 4;
constexpr int FOLIAGE_HEIGHTMAP_SIZE = 1024;
constexpr uint8_t FOLIAGE_ATTRIB_FORCE_ON = 1 << 0;
constexpr uint8_t FOLIAGE_ATTRIB_SHADOW = 1 << 1;
constexpr uint8_t FOLIAGE_ATTRIB_KNOWN_MASK = FOLIAGE_ATTRIB_FORCE_ON | FOLIAGE_ATTRIB_SHADOW;

enum class FoliageColorMode : int {
	MatchGround = 0,
	Blend50 = 1,
	RetainFullColor = 2,
};

struct FoliageDef {
	std::string graphic;
	int color_lower = static_cast<int>(FoliageColorMode::MatchGround);
	int color_upper = static_cast<int>(FoliageColorMode::MatchGround);
	int match = -1;
	uint8_t attrib_flags = 0;
};

struct FoliageMap {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> indices;
	uint8_t palette[256][3] = {};
};

int foliage_normalize_color_mode(int value);
uint8_t foliage_normalize_attrib_flags(uint8_t flags);
FoliageDef foliage_normalize_def(const FoliageDef &def);

void foliage_fill_grayscale_palette(uint8_t palette[256][3]);
FoliageMap foliage_make_default_map(int width, int height, uint8_t fill_index = 0);
bool foliage_has_size(const FoliageMap &map);
bool foliage_is_valid_index(const FoliageMap &map, int x, int y);
uint8_t foliage_get_index(const FoliageMap &map, int x, int y);
bool foliage_set_index(FoliageMap &map, int x, int y, uint8_t index);
bool foliage_paint_circle(FoliageMap &map,
                          int center_x,
                          int center_y,
                          int radius,
                          float hardness,
                          float strength,
                          uint8_t index);
int foliage_count_index(const FoliageMap &map, uint8_t index);
int foliage_remap_index(FoliageMap &map, uint8_t from_index, uint8_t to_index);

int foliage_map_x_from_heightmap_x(float hm_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_map_y_from_heightmap_y(float hm_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
float foliage_heightmap_x_from_map_x(int map_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
float foliage_heightmap_y_from_map_y(int map_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_sector_id_from_heightmap(float hm_x, float hm_y, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);

} // namespace opennova
