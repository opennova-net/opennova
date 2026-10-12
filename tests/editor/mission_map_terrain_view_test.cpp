// The 2D map's backgrounds (ADR 0046 S25): Nile's Height map (each texel's height above the lowest times 255 over the
// range, green; the lowest seeded at 0x7FFF) and Delta map (the same brightness, green where a texel equals its four
// neighbours as Nile reads them over its quadrant-major copy, red where not) [orig: gobj.dll sub_10010720,
// sub_100307BA]; the original mission editor's flatness (0x404040 where a texel differs from its four wrapped
// neighbours) [orig: JOTACmed.exe sub_44C2B0 @ 0x44c7e9..0x44c8ac] and its water overlay (the channels halved and 128
// added to the low byte where the height byte is below the water's) [orig: sub_4222A0 @ 0x422508..0x422525]; and
// the view made again only when what it reads moves.
#include <cstdio>
#include <vector>

#include <editor/preview/mission_map_terrain_view.h>

#include "common/test_expect.h"

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

} // namespace

int main() {
	if (test_height() != 0) return 1;
	if (test_delta() != 0) return 1;
	if (test_flatness_and_water() != 0) return 1;
	return test_follow();
}
