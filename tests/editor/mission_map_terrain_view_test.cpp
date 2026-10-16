// The 2D map's backgrounds (ADR 0046 S25): Nile's Height map (each texel's height above the lowest times 255 over the
// range, green; the lowest seeded at 0x7FFF) and Delta map (the same brightness, green where a texel equals its four
// neighbours as Nile reads them over its quadrant-major copy, red where not) [orig: gobj.dll sub_10010720,
// sub_100307BA]; the original mission editor's flatness (0x404040 where a texel differs from its four wrapped
// neighbours) [orig: JOTACmed.exe sub_44C2B0 @ 0x44c7e9..0x44c8ac] and its water overlay (the channels halved and 128
// added to the low byte where the height byte is below the water's) [orig: JOTACmed.exe sub_4222A0 @
// 0x422508..0x422525]; and
// the view made again only when what it reads moves, and patched where a stroke changed the atlas in place (the same
// picture a whole make gives; whole where the heights' range moved).
#include <cstdio>
#include <string>
#include <vector>

#include <editor/preview/mission_map_terrain_view.h>
#include <formats/trn/charmap_legend.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr int kSide = 1024;

uint32_t at(const std::vector<uint32_t> &picture, int row, int column) { return picture[size_t(row) * kSide + size_t(column)]; }

// Nile's brightness of a height between the lowest and the highest, its scale a float [@ 0x10010807..0x10010869].
uint32_t lit(uint32_t height, uint32_t lowest, uint32_t highest) {
	const float scale = highest == lowest ? 1.0f : float(1.0 / (double(highest - lowest) * 0.0039215689));
	return uint32_t(uint64_t(double(height - lowest) * double(scale)));
}

int test_height() {
	std::vector<uint16_t> atlas(size_t(kSide) * kSide, 0x100);
	atlas[size_t(10) * kSide + 20] = 0x200;
	std::vector<uint32_t> picture;
	nile_height_map(atlas.data(), kSide, kSide, picture);
	// 0x100 the lowest, 0x200 the highest: the highest bright green (255 over 256 of the way, truncated), the rest 0.
	TEST_EXPECT(at(picture, 10, 20) == ((lit(0x200, 0x100, 0x200) << 8) | 0xFF000000u) && lit(0x200, 0x100, 0x200) >= 254 &&
	            at(picture, 0, 0) == 0xFF000000u);
	// The lowest seeded at 0x7FFF: every height past it reads from 0x7FFF (the highest 255 still).
	std::fill(atlas.begin(), atlas.end(), uint16_t(0x9000));
	atlas[0] = 0xA000;
	nile_height_map(atlas.data(), kSide, kSide, picture);
	TEST_EXPECT(at(picture, 0, 0) == ((lit(0xA000, 0x7FFF, 0xA000) << 8) | 0xFF000000u) &&
	            at(picture, 5, 5) == ((lit(0x9000, 0x7FFF, 0xA000) << 8) | 0xFF000000u) && lit(0x9000, 0x7FFF, 0xA000) > 0);
	std::printf("height: green over the range, the lowest seeded at 0x7FFF\n");
	return 0;
}

int test_delta() {
	// The lowest far away, a base of 10 and a bump of 20 at (100, 100).
	std::vector<uint16_t> atlas(size_t(kSide) * kSide, 10);
	atlas[size_t(900) * kSide + 900] = 0;
	atlas[size_t(100) * kSide + 100] = 20;
	std::vector<uint32_t> picture;
	nile_delta_map(atlas.data(), kSide, kSide, picture);
	const auto red = [](uint32_t p) { return (p & 0x00FF0000u) != 0 && (p & 0x0000FF00u) == 0; };
	const auto green = [](uint32_t p) { return (p & 0x0000FF00u) != 0 && (p & 0x00FF0000u) == 0; };
	TEST_EXPECT(red(at(picture, 100, 100)));
	// Nile's neighbours above and below are two rows of the quadrant away (its 1024-word row over 512-word quadrant
	// rows): the texel two rows below the bump differs from its neighbour, the one right below it does not.
	TEST_EXPECT(red(at(picture, 102, 100)) && red(at(picture, 98, 100)));
	TEST_EXPECT(green(at(picture, 101, 100)) && green(at(picture, 99, 100)));
	TEST_EXPECT(red(at(picture, 100, 101)) && red(at(picture, 100, 99)));
	TEST_EXPECT(green(at(picture, 300, 300)));
	std::printf("delta: red where a texel differs from Nile's four neighbours, green where not\n");
	return 0;
}

int test_flatness_and_water() {
	std::vector<uint16_t> atlas(size_t(kSide) * kSide, 5120); // 20 units
	atlas[size_t(100) * kSide + 100] = 7680;                  // 30 units
	std::vector<uint32_t> picture;
	med_flatness_map(atlas.data(), picture);
	TEST_EXPECT(at(picture, 100, 100) == 0xFF404040u && at(picture, 101, 100) == 0xFF404040u &&
	            at(picture, 99, 100) == 0xFF404040u && at(picture, 100, 101) == 0xFF404040u &&
	            at(picture, 100, 99) == 0xFF404040u && at(picture, 102, 100) == 0xFF000000u);
	// The edges wrap: a texel on the first row against the last.
	atlas[size_t(1023) * kSide + 5] = 1;
	med_flatness_map(atlas.data(), picture);
	TEST_EXPECT(at(picture, 0, 5) == 0xFF404040u);
	// Water 25 units: the 20-unit ground under it (its height byte 40 < 50) tinted, the 30-unit bump (60) not.
	std::vector<uint32_t> green(size_t(kSide) * kSide, 0xFF00FF00u);
	med_water_overlay(atlas.data(), 25 << 16, green);
	TEST_EXPECT(at(green, 0, 0) == 0xFF007F80u && at(green, 100, 100) == 0xFF00FF00u);
	std::printf("flatness and water: the original editor's\n");
	return 0;
}

int test_follow() {
	std::vector<uint16_t> atlas(size_t(kSide) * kSide, 10);
	MissionMapTerrainView view;
	view.follow(MissionMapBackground::Colour, false, atlas.data(), 1, 0);
	TEST_EXPECT(view.picture().empty() && view.made() == 1);
	view.follow(MissionMapBackground::Height, false, atlas.data(), 1, 0);
	TEST_EXPECT(view.picture().size() == size_t(kSide) * kSide && view.made() == 2);
	const uint64_t serial = view.serial();
	view.follow(MissionMapBackground::Height, false, atlas.data(), 1, 0);
	TEST_EXPECT(view.made() == 2 && view.serial() == serial); // nothing moved
	view.follow(MissionMapBackground::Height, false, atlas.data(), 2, 0);
	TEST_EXPECT(view.made() == 3 && view.serial() != serial); // the atlas moved
	view.follow(MissionMapBackground::Delta, true, atlas.data(), 2, 30 << 16);
	TEST_EXPECT(view.made() == 4);
	std::printf("follow: made again only when the atlas, the background or the water moves\n");
	return 0;
}

// A patch of the atlas in place: the picture made again where the changed texels read, the same as a whole make; the
// rects it changed for a device's upload; Height and Delta over the scale they were made with while the lowest or the
// highest moves, the stroke's settle making them whole again then.
int test_patch() {
	struct Case {
		MissionMapBackground background;
		bool water;
	};
	const Case cases[] = {{MissionMapBackground::Height, false}, {MissionMapBackground::Delta, false},
	                      {MissionMapBackground::Flatness, false}, {MissionMapBackground::Delta, true},
	                      {MissionMapBackground::Flatness, true}};
	for (const Case &each : cases) {
		std::vector<uint16_t> atlas(size_t(kSide) * kSide, 5120);
		atlas[0] = 0;      // the lowest
		atlas[1] = 0x7000; // the highest
		// A region by a quadrant's edge and across the atlas's middle, so Nile's neighbours cross his copy's rows.
		for (int y = 3; y < 9; ++y)
			for (int x = 509; x < 515; ++x) atlas[size_t(y) * kSide + size_t(x)] = uint16_t(5120 + 37 * (x + y));
		MissionMapTerrainView view;
		view.follow(each.background, each.water, atlas.data(), 1, 25 << 16);
		const int made = view.made();
		const uint64_t serial = view.serial();
		// A stroke: a dent inside the range, and a bump by a wrapped edge.
		for (int y = 500; y < 530; ++y)
			for (int x = 500; x < 530; ++x) atlas[size_t(y) * kSide + size_t(x)] = uint16_t(4000 + 13 * ((x * 7 + y) % 50));
		for (int y = 1020; y < 1024; ++y)
			for (int x = 0; x < 3; ++x) atlas[size_t(y) * kSide + size_t(x)] = 6000;
		view.patch(atlas.data(), {{500, 500, 530, 530}, {0, 1020, 3, 1024}}, 2);
		TEST_EXPECT(view.made() == made && view.patched() == 1 && view.serial() != serial);
		TEST_EXPECT(view.whole_serial() == serial && view.dirty().size() == 2);
		// The settle of a stroke that moved neither the lowest nor the highest: nothing made again.
		view.settle(atlas.data());
		TEST_EXPECT(view.made() == made);
		MissionMapTerrainView whole;
		whole.follow(each.background, each.water, atlas.data(), 2, 25 << 16);
		TEST_EXPECT(view.picture() == whole.picture());
		// The follow at the patched revision keeps it.
		view.follow(each.background, each.water, atlas.data(), 2, 25 << 16);
		TEST_EXPECT(view.made() == made);
		// The highest moved: every background patched in the stroke (Height and Delta over the scale they had, no 1M
		// words scanned per dab); its settle makes Height and Delta whole over the new range.
		atlas[size_t(600) * kSide + 600] = 0x7800;
		view.patch(atlas.data(), {{600, 600, 601, 601}}, 3);
		const bool scaled = each.background != MissionMapBackground::Flatness;
		TEST_EXPECT(view.made() == made && view.patched() == 2);
		view.settle(atlas.data());
		TEST_EXPECT(view.made() == made + (scaled ? 1 : 0));
		whole.follow(each.background, each.water, atlas.data(), 3, 25 << 16);
		TEST_EXPECT(view.picture() == whole.picture());
	}
	// A view of the colour map has nothing to patch, and takes the revision.
	std::vector<uint16_t> atlas(size_t(kSide) * kSide, 10);
	MissionMapTerrainView colour;
	colour.follow(MissionMapBackground::Colour, false, atlas.data(), 1, 0);
	colour.patch(atlas.data(), {{0, 0, 4, 4}}, 2);
	colour.follow(MissionMapBackground::Colour, false, atlas.data(), 2, 0);
	TEST_EXPECT(colour.made() == 1 && colour.patched() == 0);
	std::printf("patch: the texels a stroke changed made again as a whole make makes them; whole where the range moved\n");
	return 0;
}

// The surface classes over the atlas: each atlas texel the class the char map sampler reads (a 256-wide map's texel
// under four by four of the atlas's), in the legend's colours (white past the twenty); class 1 past the map and with
// none; a stroke's char map patch made again in place as a whole make makes it.
int test_surface() {
	constexpr int kMap = 256;
	std::vector<uint8_t> classes(size_t(kMap) * kMap, 2);
	classes[0] = 5;                         // sand at the atlas's (0..3, 0..3)
	classes[size_t(10) * kMap + 200] = 40;  // a class past the legend at (800..803, 40..43)
	std::vector<uint32_t> picture;
	surface_class_map(classes.data(), kMap, kMap, picture);
	const auto rgb = [](int surface) {
		const CharmapLegendColour &c = surface < kCharmapLegendCount ? kCharmapLegend[surface] : kCharmapLegendRest;
		return 0xFF000000u | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | uint32_t(c.b);
	};
	TEST_EXPECT(picture.size() == size_t(kSide) * kSide && at(picture, 0, 0) == rgb(5) && at(picture, 3, 3) == rgb(5) &&
	            at(picture, 0, 4) == rgb(2) && at(picture, 42, 801) == rgb(40) && at(picture, 42, 801) == 0xFFFFFFFFu);
	// A map shorter than its width: the rows past it read class 1, as the sampler reads them.
	std::vector<uint8_t> short_map(size_t(kMap) * 128, 3);
	surface_class_map(short_map.data(), kMap, 128, picture);
	TEST_EXPECT(at(picture, 10, 10) == rgb(3) && at(picture, 600, 10) == rgb(1));
	surface_class_map(nullptr, 0, 0, picture);
	TEST_EXPECT(at(picture, 500, 500) == rgb(1));
	// The atlas rects a map region covers.
	const MissionMapAtlasRect rect = surface_atlas_rect(kMap, 10, 20, 3, 2);
	TEST_EXPECT(rect.x0 == 40 && rect.y0 == 80 && rect.x1 == 52 && rect.y1 == 88);
	// The view: made over a terrain read, patched in place, the same as a whole make; a follow at the patched
	// revision keeps it; with no terrain read, nothing (the device draws none).
	MissionMapTerrainView view;
	view.follow(MissionMapBackground::Surface, false, nullptr, 0, 0, MissionMapSurface{classes.data(), kMap, kMap, 1}, true);
	TEST_EXPECT(view.picture().size() == size_t(kSide) * kSide && view.made() == 1 && at(view.picture(), 0, 0) == rgb(5));
	for (int y = 100; y < 104; ++y)
		for (int x = 50; x < 60; ++x) classes[size_t(y) * kMap + size_t(x)] = 12;
	view.patch_surface(MissionMapSurface{classes.data(), kMap, kMap, 2}, {surface_atlas_rect(kMap, 50, 100, 10, 4)});
	TEST_EXPECT(view.made() == 1 && view.patched() == 1 && view.dirty().size() == 1 && at(view.picture(), 400, 200) == rgb(12));
	MissionMapTerrainView whole;
	whole.follow(MissionMapBackground::Surface, false, nullptr, 0, 0, MissionMapSurface{classes.data(), kMap, kMap, 2}, true);
	TEST_EXPECT(view.picture() == whole.picture());
	view.follow(MissionMapBackground::Surface, false, nullptr, 0, 0, MissionMapSurface{classes.data(), kMap, kMap, 2}, true);
	TEST_EXPECT(view.made() == 1);
	MissionMapTerrainView none;
	none.follow(MissionMapBackground::Surface, false, nullptr, 0, 0, MissionMapSurface(), false);
	TEST_EXPECT(none.picture().empty());
	TEST_EXPECT(std::string(mission_map_background_token(MissionMapBackground::Surface)) == "surface");
	MissionMapBackground parsed = MissionMapBackground::Colour;
	TEST_EXPECT(mission_map_background_from_token("surface", parsed) && parsed == MissionMapBackground::Surface);
	std::printf("surface: the char map's classes over the atlas in the legend's colours, patched in place\n");
	return 0;
}

} // namespace

int main() {
	if (test_height() != 0) return 1;
	if (test_delta() != 0) return 1;
	if (test_flatness_and_water() != 0) return 1;
	if (test_patch() != 0) return 1;
	if (test_surface() != 0) return 1;
	return test_follow();
}
