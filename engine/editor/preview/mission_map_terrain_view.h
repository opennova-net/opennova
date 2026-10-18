#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::editor {

// What the map draws under its marks (the map's option `background`): the terrain's colour map as the game's
// commander map draws it, the original tools' Height map and Delta map (Nile [orig: gobj.dll sub_10010720]), the
// original mission editor's flatness view, the surface classes the char map holds in its legend's colours, or
// nothing; its option `water` lays that editor's water overlay over the heights' views.
enum class MissionMapBackground : uint8_t { Colour, Height, Delta, Flatness, Surface, None, kCount };
// "colour", "height", "delta", "flatness", "surface", "none" (preview/mission_map.cpp).
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
// The atlas copied into its four quadrants first, one after another [orig: gobj.dll sub_100307BA @ 0x100307ba].
void nile_height_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out);
// Nile's Delta map [orig: gobj.dll sub_10010720, the 2dDeltaMap pass]: the same brightness, green where the texel
// equals its four neighbours, red where it does not; the neighbours as the original reads them, over its
// quadrant-major copy with a 1024-word row (so a "row" of it is two rows of a quadrant, and its neighbours above and
// below two rows away), wrapped.
void nile_delta_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out);
// The original mission editor's flatness view ([orig: JOTACmed.exe MissionDocument_LoadTerrain @ 0x44c7e9..0x44c8ac], drawn
// by its MapView_RenderTerrain in its third mode): black where the texel equals its four neighbours, wrapped at the 1024
// edges, else 0x404040.
void med_flatness_map(const uint16_t *atlas, std::vector<uint32_t> &out);
// The char map's surface classes over the atlas (an editor view, no tool's: the 3D view's surfaces overlay's colours):
// each atlas texel the class the game's char map sampler reads there, the atlas texel shifted down by the map's sample
// shift [terrain::surface_type_at_fixed's read; class 1 past the map's rows or columns, and everywhere with no char
// map (`classes` null), as the sampler reads one], in the legend's colour (formats/trn/charmap_legend.h; white past
// it). The placed tiles' classes are not in it: the map draws the tiles over it (the 3D view's overlay reads them).
void surface_class_map(const uint8_t *classes, int width, int height, std::vector<uint32_t> &out);
// Its water overlay over any picture [orig: JOTACmed.exe MapView_RenderTerrain @ 0x422508..0x422525, toggled by its Water
// button @ 0x4434f3]: where the texel's height byte (bits 8..15 of twice its depth word, as the original holds
// it) is below the water's (its 16.16 height >> 15), the pixel's channels halved and 128 added to its low byte.
void med_water_overlay(const uint16_t *atlas, int32_t water_1616, std::vector<uint32_t> &pixels);

// A region of the atlas, its texels [x0, x1) x [y0, y1) in the atlas's own rows.
struct MissionMapAtlasRect {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The char map the Surface background is made of: the classes as the game's load keeps them (MissionGround's
// surface_sampler; null with none read, class 1 everywhere), its size, and what moves with each read and patch.
struct MissionMapSurface {
	const uint8_t *classes = nullptr;
	int width = 0;
	int height = 0;
	uint64_t revision = 0;
	bool operator==(const MissionMapSurface &o) const {
		return classes == o.classes && width == o.width && height == o.height && revision == o.revision;
	}
	bool operator!=(const MissionMapSurface &o) const { return !(*this == o); }
};
// The atlas texels a region of the char map `width` wide covers (each map texel the atlas texels its sample shift
// reads it at); the whole atlas for a map wider than it.
MissionMapAtlasRect surface_atlas_rect(int width, int x, int y, int region_width, int region_height);

// The map's background as the options ask it, made again when the atlas, the background or the water moved: the
// picture (empty for the colour map and for none, which the device draws its own way) and a serial that moves with
// each one made or patched (the device's one upload).
class MissionMapTerrainView {
public:
	// `atlas` 1024 x 1024 depth words (null: no terrain); `revision` what moves whenever the atlas does (the ground's
	// reads, a stroke's patches); `water_1616` the mission's water height (0: none drawn); `surface` the char map the
	// Surface background is made of (a terrain read: `terrain`).
	void follow(MissionMapBackground background, bool water, const uint16_t *atlas, uint64_t revision, int32_t water_1616,
	            const MissionMapSurface &surface = MissionMapSurface(), bool terrain = false);
	// The char map changed in place within `regions` of the atlas (a Surface stroke's patch, surface_atlas_rect), its
	// revision now `surface`'s: the Surface background made again there alone; a follow with that surface then keeps
	// it. Nothing for another background.
	void patch_surface(const MissionMapSurface &surface, const std::vector<MissionMapAtlasRect> &regions);
	// The same atlas changed in place within `regions` (a stroke's patch), its revision now `revision`: the picture made
	// again there alone, each pixel whose background reads a changed texel (Height the texel's own; Delta the texel and
	// Nile's four neighbours of it, over his quadrant-major copy; flatness the region and a texel around it, wrapped;
	// the water over each), Height and Delta over the scale the picture was made with (settle takes the heights' lowest
	// and highest again). A follow with that revision then keeps it. Nothing for an atlas it does not show.
	void patch(const uint16_t *atlas, const std::vector<MissionMapAtlasRect> &regions, uint64_t revision);
	// A stroke's end (S25): Height and Delta keep the scale they were made over while a stroke patches them (a texel
	// past it shown at its end, none of 1M words scanned per dab); here the picture takes the scale the heights' lowest
	// and highest give it now, made again whole where it moved.
	void settle(const uint16_t *atlas);
	const std::vector<uint32_t> &picture() const { return picture_; }
	uint64_t serial() const { return serial_; }
	// The serial of its last whole make, and the atlas rects its patches changed since (a device uploads them alone
	// where it holds the picture of that make).
	uint64_t whole_serial() const { return whole_serial_; }
	const std::vector<MissionMapAtlasRect> &dirty() const { return dirty_; }
	// How many times it made the picture whole, and patched it (a test's counts).
	int made() const { return made_; }
	int patched() const { return patched_; }

private:
	void make_();

	MissionMapBackground background_ = MissionMapBackground::Colour;
	bool water_ = false;
	const uint16_t *atlas_ = nullptr;
	MissionMapSurface surface_;
	bool terrain_ = false;
	uint64_t revision_ = 0;
	int32_t water_1616_ = 0;
	bool followed_ = false;
	std::vector<uint32_t> picture_;
	uint64_t serial_ = 0;
	uint64_t whole_serial_ = 0;
	std::vector<MissionMapAtlasRect> dirty_;
	bool stale_ = false; // a patch kept a scale the heights may have moved past
	int made_ = 0;
	int patched_ = 0;
	// The heights' lowest and scale the picture was made over (Height, Delta).
	uint32_t lowest_ = 0;
	float scale_ = 1.0f;
};

} // namespace opennova::editor
