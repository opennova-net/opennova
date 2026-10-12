#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::editor {

// What the map draws under its marks (the map's option `background`): the terrain's colour map as the game's
// commander map draws it, the original tools' Height map and Delta map (Nile [orig: gobj sub_10010720]), the
// original mission editor's flatness view, or nothing; its option `water` lays that editor's water overlay over it.
enum class MissionMapBackground : uint8_t { Colour, Height, Delta, Flatness, None, kCount };
// "colour", "height", "delta", "flatness", "none" (preview/mission_map.cpp).
const char *mission_map_background_token(MissionMapBackground background);
bool mission_map_background_from_token(const std::string &token, MissionMapBackground &out);

// The 2D map's backgrounds (ADR 0046 S25, In the views): the pictures the original tools drew of a terrain's heights
// in place of its colour map, made from the height atlas the game loads (1024 x 1024 depth words, the .cpt's CDEP or
// DPTH grid in its row order: CptFile::depth_buffer, the live atlas a stroke patches), each a picture of 0xAARRGGBB
// words in the atlas's own layout (its 512-texel quadrants where the colour map's are, so the map's commander-map
// compile lays it over the sector grid as it lays the colour map).

// Nile's Height map [orig: gobj.dll sub_10010720 @ 0x10010720, the 2dHeightMap pass]: each texel's height less the
// lowest, times 255 over the range (1 where the heights are flat), its green channel; the lowest seeded at 0x7FFF and
// the highest at 0 as the original seeds them, so a terrain whose every height is past 0x7FFF reads from 0x7FFF.
// The atlas copied into its four quadrants first, one after another [orig: sub_100307BA @ 0x100307ba].
void nile_height_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out);
// Nile's Delta map [orig: sub_10010720, the 2dDeltaMap pass]: the same brightness, green where the texel equals its
// four neighbours, red where it does not; the neighbours as the original reads them, over its quadrant-major copy
// with a 1024-word row (so a "row" of it is two rows of a quadrant, and its neighbours above and below two rows
// away), wrapped.
void nile_delta_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out);
// The original mission editor's flatness view (JOTACmed.exe [orig: sub_44C2B0 @ 0x44c7e9..0x44c8ac], drawn by
// sub_4222A0 in its third mode): black where the texel equals its four neighbours, wrapped at the 1024 edges, else
// 0x404040.
void med_flatness_map(const uint16_t *atlas, std::vector<uint32_t> &out);
// Its water overlay over any picture [orig: sub_4222A0 @ 0x422508..0x422525, toggled by its Water button @
// 0x4434f3]: where the texel's height byte (bits 8..15 of twice its depth word, as the original holds it) is below
// the water's (its 16.16 height >> 15), the pixel's channels halved and 128 added to its low byte.
void med_water_overlay(const uint16_t *atlas, int32_t water_1616, std::vector<uint32_t> &pixels);

// The map's background as the options ask it, made again when the atlas, the background or the water moved: the
// picture (empty for the colour map and for none, which the device draws its own way) and a serial that moves with
// each one made (the device's one upload).
class MissionMapTerrainView {
public:
	// `atlas` 1024 x 1024 depth words (null: no terrain); `revision` what moves whenever the atlas does (the ground's
	// reads, a stroke's patches); `water_1616` the mission's water height (0: none drawn).
	void follow(MissionMapBackground background, bool water, const uint16_t *atlas, uint64_t revision, int32_t water_1616);
	const std::vector<uint32_t> &picture() const { return picture_; }
	uint64_t serial() const { return serial_; }
	// How many times it made the picture (a test's count).
	int made() const { return made_; }

private:
	MissionMapBackground background_ = MissionMapBackground::Colour;
	bool water_ = false;
	const uint16_t *atlas_ = nullptr;
	uint64_t revision_ = 0;
	int32_t water_1616_ = 0;
	bool followed_ = false;
	std::vector<uint32_t> picture_;
	uint64_t serial_ = 0;
	int made_ = 0;
};

} // namespace opennova::editor
