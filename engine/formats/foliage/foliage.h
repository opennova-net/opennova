#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Foliage-def sync into placement/render globals and the world->foliagemap helpers.
// [orig: jodemo Foliage_BuildPatchData @0x5C0240, Terrain_GetFoliageMapValue @0x5C65E0]
// [orig: Foliage_SampleFoliageMapMask @0x606620; Foliage_GenerateModelTileInstances @0x600980;
//  generate_foliage_instances_0 @0x5ffdd0 — the retail map sampler and the two tier builders]

constexpr int FOLIAGE_MAX_DEFS = 4;
constexpr int FOLIAGE_HEIGHTMAP_SIZE = 1024;

// The canonical foliage-map match codes, one per definition slot:
// slot 0 paints palette index 254, slot 1 -> 253, slot 2 -> 252, slot 3 ->
// 251. Shipped .trn foliage maps carry exactly these codes for their (at most
// FOLIAGE_MAX_DEFS) definitions.
constexpr std::array<uint8_t, FOLIAGE_MAX_DEFS> FOLIAGE_CANONICAL_MATCHES = {
	254, 253, 252, 251
};

// Canonical match code for a def slot; -1 when the slot is outside
// [0, FOLIAGE_MAX_DEFS).
inline int foliage_canonical_match_for_slot(int def_index) {
	if (def_index < 0 || def_index >= FOLIAGE_MAX_DEFS) {
		return -1;
	}
	return static_cast<int>(FOLIAGE_CANONICAL_MATCHES[static_cast<size_t>(def_index)]);
}
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

FoliageMap foliage_make_default_map(int width, int height, uint8_t fill_index = 0);
bool foliage_has_size(const FoliageMap &map);
uint8_t foliage_get_index(const FoliageMap &map, int x, int y);

// Detail foliage uses retail's flat, repeating world lookup rather than the
// sector-routed terrain lookup used by the MODEL tier. Coordinates are in the
// reimpl plane, where world Z is already the negation of retail Z.
uint8_t foliage_sample_detail_flat_wrap(const FoliageMap &map,
                                         int32_t world_x_fixed,
                                         int32_t world_z_fixed);

int foliage_map_x_from_heightmap_x(float hm_x, int map_width, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);
int foliage_map_y_from_heightmap_y(float hm_y, int map_height, int hm_size = FOLIAGE_HEIGHTMAP_SIZE);

} // namespace opennova
