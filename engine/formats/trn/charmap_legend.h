#pragma once

#include <cstdint>

namespace opennova {

// The char map's legend: a colour for each surface class, the twenty of formats/til/til_tsd.h (0 TSD_NULL
// to 19 TSD_FLESH). A char map (the .trn's polytrn_charmap) is an 8-bit PCX whose index IS the surface
// class the game reads at a position, its palette unused [orig: sub_605A10 @ 0x605A31..0x605A7D, the
// indices copied alone; Terrain_GetSurfaceTypeAtPosition @ 0x6065C6]; the legend as its palette makes it
// read as a map in any image viewer, and lets a colour picture be read back as classes.
//
// Classes 0 to 14 are NovaLogic's own tool legend, the palette a shipped char map carries (witnessed in
// JO:CA's dvxg8_m.pcx, Indigo Spectre's, 8-bit and 512 x 512, its entries 15 and above white). Classes 15
// to 19, which that legend leaves white, take OpenNova's colours, each unlike every other entry.
struct CharmapLegendColour {
	uint8_t r, g, b;
};

inline constexpr int kCharmapLegendCount = 20;
inline constexpr CharmapLegendColour kCharmapLegend[kCharmapLegendCount] = {
	{0, 0, 0},       // 0 NULL
	{153, 118, 61},  // 1 DIRT
	{0, 210, 0},     // 2 GRASS
	{204, 239, 244}, // 3 SNOW
	{152, 152, 152}, // 4 CEMENT
	{255, 255, 0},   // 5 SAND
	{255, 128, 0},   // 6 PACKEDDIRT
	{0, 0, 255},     // 7 UNDERWATER
	{255, 0, 0},     // 8 RAILROAD
	{114, 64, 0},    // 9 MUD
	{160, 190, 219}, // 10 ICE
	{161, 0, 161},   // 11 QUICKSAND
	{255, 0, 186},   // 12 STONE
	{158, 78, 0},    // 13 WOOD
	{0, 201, 203},   // 14 METAL
	// OpenNova's, past the shipped legend:
	{192, 255, 255}, // 15 GLASS
	{128, 128, 255}, // 16 CLOTH
	{0, 100, 0},     // 17 FOLIAGE
	{80, 80, 80},    // 18 HMETAL
	{255, 192, 160}, // 19 FLESH
};
// A palette entry past the legend, as the shipped legend leaves its own: white.
inline constexpr CharmapLegendColour kCharmapLegendRest = {255, 255, 255};

// The class whose legend colour `r, g, b` is exactly, or -1 for a colour of none.
inline int charmap_legend_class(uint8_t r, uint8_t g, uint8_t b) {
	for (int i = 0; i < kCharmapLegendCount; ++i)
		if (kCharmapLegend[i].r == r && kCharmapLegend[i].g == g && kCharmapLegend[i].b == b) return i;
	return -1;
}

} // namespace opennova
