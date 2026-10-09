#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Foliage-def sync into placement/render globals and the world->foliagemap helpers.
// [orig: jodemo Foliage_BuildPatchData @0x5C0240, Terrain_GetFoliageMapValue @0x5C65E0]
// [orig: Foliage_SampleFoliageMapMask @0x606620; Foliage_GenerateModelTileInstances @0x600980;
//  Foliage_GenerateInstances_0 @0x5ffdd0 — the retail map sampler and the two tier builders]

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

// The match codes a definition slot CONSUMES. The .trn parser stores up to 7
// `match` args as bytes at slot+0x108.. (`match_idx` 1..7 -> bytes[536*slot +
// 6227 + idx], slot base 5964, stride 0x218) [orig: Terrain_ParseConfigCallback
// @0x60F330], but the foliagemap remap compares the pixel against only the
// FOUR bytes at +0x108..+0x10B [orig: Foliage_RemapPixelToDefMask @0x5FF4E0],
// so the reimpl keeps four; args five to seven are parsed and dropped.
constexpr int FOLIAGE_MATCH_CODES = 4;
// An unset code. Retail leaves the byte 0, and since pixel 0 never matches a
// zero byte is equally inert; -1 keeps "unset" distinct from an authored 0.
constexpr int FOLIAGE_MATCH_UNSET = -1;

struct FoliageDef {
	std::string graphic;
	int color_lower = static_cast<int>(FoliageColorMode::MatchGround);
	int color_upper = static_cast<int>(FoliageColorMode::MatchGround);
	// The consumed codes in authored order; FOLIAGE_MATCH_UNSET pads the tail
	// (foliage_normalize_def compacts authored codes to the front, which the
	// OR-of-four consumer cannot distinguish from the authored positions).
	std::array<int, FOLIAGE_MATCH_CODES> match = {
		FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET, FOLIAGE_MATCH_UNSET
	};
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

// Number of authored (non-unset) codes after normalization.
int foliage_def_match_count(const FoliageDef &def);

// The foliagemap pixel -> definition-slot mask [orig: Foliage_RemapPixelToDefMask
// @0x5FF4E0]: pixel 0 never matches; a slot whose header byte (+0, the graphic
// name's first char) is 0 is skipped; otherwise bit `1 << slot` is set when
// the pixel equals ANY of the slot's four stored codes. `defs` is indexed by
// slot; entries past `FOLIAGE_MAX_DEFS` are ignored.
bool foliage_def_matches_pixel(const FoliageDef &def, int pixel);
uint32_t foliage_remap_pixel_to_def_mask(const std::vector<FoliageDef> &defs, int pixel);
// The remap the foliage map's load applies to every texel, as a table over the 256 codes: each code's
// definition-slot mask (foliage_remap_pixel_to_def_mask over `defs`) [orig: Foliage_LoadFoliageMapPCX @0x605AD0,
// the remap @0x605B73..0x605B8A].
std::array<uint8_t, 256> foliage_pixel_masks(const std::vector<FoliageDef> &defs);

FoliageMap foliage_make_default_map(int width, int height, uint8_t fill_index = 0);
bool foliage_has_size(const FoliageMap &map);
uint8_t foliage_get_index(const FoliageMap &map, int x, int y);

// Detail foliage uses retail's flat, repeating 1024 lookup at the candidate's
// source-atlas position rather than the sector-routed terrain lookup used by
// the MODEL tier. Coordinates are in the reimpl plane, where Z is already the
// negation of retail Z.
uint8_t foliage_sample_detail_flat_wrap(const FoliageMap &map,
                                         int32_t atlas_x_fixed,
                                         int32_t atlas_z_fixed);

int foliage_map_x_from_heightmap_x(float hm_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_map_y_from_heightmap_y(float hm_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);

} // namespace opennova
