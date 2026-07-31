#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Engine: jodemo.exe foliage-def sync into placement/render globals,
// Foliage_BuildPatchData@0x005C0240, Terrain_GetFoliageMapValue@0x005C65E0.

constexpr int FOLIAGE_MAX_DEFS = 4;
constexpr int FOLIAGE_HEIGHTMAP_SIZE = 1024;
constexpr uint8_t FOLIAGE_ATTRIB_FORCE_ON = 1 << 0;
constexpr uint8_t FOLIAGE_ATTRIB_SHADOW = 1 << 1;
constexpr uint8_t FOLIAGE_ATTRIB_KNOWN_MASK = FOLIAGE_ATTRIB_FORCE_ON | FOLIAGE_ATTRIB_SHADOW;

// Values observed in shipped .trn files. The placement paths preserve them for
// the render emitter; the previously cited 0x005BF5F0 address is stale and the
// final color-mode consumer still needs a verified retail anchor.
enum class FoliageColorMode : int {
	MatchGround = 0,
	Blend50 = 1,
	RetainFullColor = 2,
};

struct FoliageDef {
	std::string graphic;
	int color_lower = static_cast<int>(FoliageColorMode::MatchGround);
	int color_upper = static_cast<int>(FoliageColorMode::MatchGround);
	// Fidelity: bounded deviation. Engine stores up to 4 match codes per slot;
	// the shared port still exposes one authored match until the terrain/TRN
	// wrappers are widened.
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
bool foliage_paint_detail_circle_wrap(FoliageMap &map,
                                      int center_x,
                                      int center_y,
                                      int radius,
                                      float hardness,
                                      float strength,
                                      uint8_t index);
int foliage_count_index(const FoliageMap &map, uint8_t index);
int foliage_remap_index(FoliageMap &map, uint8_t from_index, uint8_t to_index);
// Single-pass remap of many indices at once via a 256-entry lookup table
// (new = lut[old]). One read+write per cell, so {1->2, 2->3} maps original 1s
// to 2 and original 2s to 3 without chaining 1->2->3. Returns cells changed.
int foliage_remap_indices(FoliageMap &map, const std::array<uint8_t, 256> &lut);

// Detail foliage uses retail's flat, repeating world lookup rather than the
// sector-routed terrain lookup used by the MODEL tier. Coordinates are in the
// reimpl plane, where world Z is already the negation of retail Z.
int foliage_detail_sample_resolution(const FoliageMap &map);
bool foliage_detail_flat_wrap_position(const FoliageMap &map,
                                        int32_t world_x_fixed,
                                        int32_t world_z_fixed,
                                        int &map_x, int &map_y);
uint8_t foliage_sample_detail_flat_wrap(const FoliageMap &map,
                                         int32_t world_x_fixed,
                                         int32_t world_z_fixed);

int foliage_map_x_from_heightmap_x(float hm_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_map_y_from_heightmap_y(float hm_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
float foliage_heightmap_x_from_map_x(int map_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
float foliage_heightmap_y_from_map_y(int map_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_sector_id_from_heightmap(float hm_x, float hm_y, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);

} // namespace opennova
