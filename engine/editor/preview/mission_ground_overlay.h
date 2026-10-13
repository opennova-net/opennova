#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>

namespace opennova::editor {

class MissionGround;
struct TerrainPatches;

// What the mission view tints its terrain with (DI-29): nothing, the surface classes the game reads at each
// point, or the foliage the terrain's foliage map grows there.
enum class MissionGroundOverlay : uint8_t { None, Surfaces, Foliage };
// "none", "surfaces", "foliage".
const char *mission_ground_overlay_token(MissionGroundOverlay overlay);
bool mission_ground_overlay_from_token(const std::string &token, MissionGroundOverlay &out);

// One row of an overlay's legend: its key ("3", "foliage 1"), its swatch, its share of the picture's texels and
// what it means to the game in words.
struct MissionOverlayRow {
	std::string key;
	uint8_t rgb[3] = {};
	double share = 0.0;
	std::string words;
};

// A ground overlay as a picture laid over the mission's terrain from above: a texel each `texel` metres from
// its north-west corner (west, north), the north row first, each texel what the game's own sampler reads at its
// middle (mission_ground_facts.h's), its colour the legend's and its alpha the tint's strength (0: none). Past
// the picture the game reads one value everywhere when `outside` (the grid's empty sectors all round it, no
// wrap): `outside_rgba` is its tint. Built over what the ground last read (MissionGround::reads).
struct MissionOverlayImage {
	MissionGroundOverlay kind = MissionGroundOverlay::None;
	int width = 0;
	int height = 0;
	double west = 0.0;
	double north = 0.0;
	double texel = 1.0;
	std::vector<uint8_t> rgba;
	bool outside = false;
	uint8_t outside_rgba[4] = {};
	std::string title;
	std::vector<MissionOverlayRow> legend;
	std::string words;
	// What each texel shows, for a patch of a region (mission_ground_overlay_patch, S25): the surfaces' class; the
	// foliage's grown mask (1..15), kKeptOffKey for kept off, kNothingKey for nothing grown. And how many texels show
	// each (256), and the surfaces' tiles outlined.
	std::vector<uint8_t> keys;
	std::vector<size_t> counts;
	size_t deciding = 0;
	bool empty() const { return width <= 0 || height <= 0; }
};
inline constexpr uint8_t kKeptOffKey = 16;
inline constexpr uint8_t kNothingKey = 17;

// A rectangle of the mission's plane (x east, y north, units), half-open.
struct MissionOverlayRect {
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// The texels an overlay holds at most a side: a finer one is made coarser, a power of two of the map's own
// texel, until it fits.
inline constexpr int kMissionOverlayMaxSide = 2048;
// A tint's strength (alpha) over the terrain, and a placed tile's outline in the surface overlay.
inline constexpr uint8_t kMissionOverlayAlpha = 170;

// The overlay of `kind` over what `ground` read (an empty picture, its words saying why, where it read no
// terrain). Surfaces: each class the char map legend's colour (formats/trn/charmap_legend.h; white past it),
// read by the game's sampler with the placed tiles deciding [orig: Terrain_GetSurfaceTypeAtPosition @
// 0x606510], each placed tile's square outlined; its legend the classes it holds with their shares. Foliage:
// where the foliage map's code selects a definition [orig: Foliage_SampleFoliageMapMask @ 0x606620], each
// definition its own colour, the placed tiles' squares keeping the definitions without forceon off [orig:
// Foliage_PathBlockedByPlacedTile @ 0x606490] (shown grey); its legend each definition that grows, with the
// codes it matches. The extent: the sector grid's mapped cells and the placed tiles (the whole grid on an axis
// that wraps), a texel the map's own (1024 / its side) until the picture would pass kMissionOverlayMaxSide.
MissionOverlayImage mission_ground_overlay(const MissionGround &ground, MissionGroundOverlay kind);

// The overlay `image` (mission_ground_overlay's over `ground`, of its kind) made again over `rects` alone (S25: what a
// paint of its map changed, not the whole picture per dab), its legend's shares with it; a surfaces overlay's placed
// tiles outlined again. False where it cannot be (no picture, no keys, no terrain): the caller makes it whole.
bool mission_ground_overlay_patch(const MissionGround &ground, MissionOverlayImage &image,
                                  const std::vector<MissionOverlayRect> &rects);

// The plane's rects a frame's map patches changed for an overlay of `kind` (the surface map's regions for Surfaces, the
// foliage map's for Foliage), each map region through the terrain's routing (every sector drawing the quadrant:
// terrain_atlas_world_rects); none for another plane's patch.
std::vector<MissionOverlayRect> mission_overlay_patch_rects(const MissionGround &ground, MissionGroundOverlay kind,
                                                            const TerrainPatches &patches);

// On the wire (the mission viewport's body): {kind, title, legend: [{key, rgb, share, words}], words, west,
// north, texel, width, height, outside (the rgba past the picture, or null)}; no texels.
io::JsonValue mission_overlay_to_json(const MissionOverlayImage &image);

} // namespace opennova::editor
