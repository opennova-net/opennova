// The Screen Shot action's TGA under /sstga (formats/tga/tga.h tga_write_screenshot) over small
// frames, every byte pinned as the game's writer makes them: the header (type 10, 24 bits, the
// descriptor 0); the rows from the bottom up; a run of a pixel equal to the one after it (0x80 |
// count - 1, then B, G, R) and a literal otherwise (count - 1, then each pixel); both at most 128;
// "the pixel after" being the next one in the frame's memory, so a run reaching a row's end tests
// the first pixel of the row below on screen while it takes the first pixel of the row above in its
// colour, and a run never goes on into the next row on that row's own colour; the alpha unread;
// the last pixel written ends its packet; and what the game's TGA reader decodes from the file.
// [orig: Screenshot_SaveTGARLE @0x51FF20 - the header @0x51FFA8..0x51FFE7, the rows @0x51FFEC /
//  @0x5201AD, the run test @0x520016..0x520032, the run @0x520050..0x5200AF, the literal
//  @0x5200BC..0x520193, the caps @0x520050 / @0x520114, the next row's start @0x520069..0x520072 /
//  @0x520126..0x520135, the 24-bit mask @0x52001B]
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>

#include "common/test_expect.h"

using namespace opennova::tga;

namespace {

constexpr uint32_t kA = 0x112233, kB = 0x445566, kC = 0x778899, kK = 0x000000;

// A frame, the top row first, each pixel R, G, B and the alpha given (255 unless named).
std::vector<uint8_t> frame(const std::vector<std::vector<uint32_t>> &rows, const std::vector<uint8_t> &alphas = {}) {
	std::vector<uint8_t> rgba;
	size_t i = 0;
	for (const auto &row : rows)
		for (uint32_t c : row) {
			rgba.push_back(uint8_t(c >> 16));
			rgba.push_back(uint8_t(c >> 8));
			rgba.push_back(uint8_t(c));
			rgba.push_back(i < alphas.size() ? alphas[i] : 255);
			++i;
		}
	return rgba;
}

std::vector<uint8_t> packets(const std::vector<uint8_t> &file) {
	return std::vector<uint8_t>(file.begin() + TGA_HEADER_SIZE, file.end());
}

// The colour the game's reader decodes at (x, y), the top row 0.
uint32_t read_at(const TgaImage &image, int x, int y) {
	const uint8_t *p = image.rgba.data() + (size_t(y) * image.width + x) * 4;
	return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
}

// The rows go from the bottom up and a packet carries on into the next row written. Row 2 (written
// first) opens a literal (A != B) that runs through its four pixels and on into row 1's B, C: its last
// pixel's pixel-after lies past the frame and matches nothing, and the literal stops after C, as the
// next two (A, A) pair. Row 1's A, A opens a run; at the row's end the pixel after is row 2's first
// (A, the row below on screen), so the run takes row 0's first pixel (C) in A. Row 0's B, B, B ends
// the frame: its run stops at the last pixel, where the game's run (row 1's first pixel, B, the pixel
// after) goes on into the memory before its buffer. The alpha is not compared (the 24-bit mask).
int test_run_across_rows_and_the_last_pixel() {
	const auto rgba = frame({{kC, kB, kB, kB}, {kB, kC, kA, kA}, {kA, kB, kA, kB}},
	                        {255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 255, 255});
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(tga_write_screenshot(rgba.data(), 4, 3, out, error));
	const std::vector<uint8_t> header = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 24, 0};
	TEST_EXPECT(std::vector<uint8_t>(out.begin(), out.begin() + TGA_HEADER_SIZE) == header);
	const std::vector<uint8_t> want = {
	    0x05, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44, // A B A B
	    0x66, 0x55, 0x44, 0x99, 0x88, 0x77,                                           // B C
	    0x82, 0x33, 0x22, 0x11,                                                       // A A + row 0's first
	    0x82, 0x66, 0x55, 0x44,                                                       // B B B, the last pixel
	};
	TEST_EXPECT(packets(out) == want);
	// What a reader sees: row 0's first pixel in the run's A, every other pixel as the frame holds it.
	TgaImage image;
	TEST_EXPECT(tga_decode_retail(out.data(), out.size(), image, error) && image.width == 4 && image.height == 3);
	TEST_EXPECT(read_at(image, 0, 0) == kA && read_at(image, 1, 0) == kB && read_at(image, 3, 0) == kB);
	TEST_EXPECT(read_at(image, 0, 1) == kB && read_at(image, 1, 1) == kC && read_at(image, 3, 1) == kA);
	TEST_EXPECT(read_at(image, 0, 2) == kA && read_at(image, 3, 2) == kB);
	return 0;
}

// A run that reaches a row's end stops there when the pixel after (the row below's first, C) differs,
// even though the next row written starts in its colour: row 1's A, A is one run and row 0's A opens a
// literal. Row 2's C, C ends at the frame's end (the pixel after lies past it).
int test_run_stops_at_a_row_end() {
	const auto rgba = frame({{kA, kB}, {kA, kA}, {kC, kC}});
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(tga_write_screenshot(rgba.data(), 2, 3, out, error));
	const std::vector<uint8_t> want = {0x81, 0x99, 0x88, 0x77, 0x81, 0x33, 0x22, 0x11,
	                                   0x01, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44};
	TEST_EXPECT(packets(out) == want);
	TgaImage image;
	TEST_EXPECT(tga_decode_retail(out.data(), out.size(), image, error));
	TEST_EXPECT(read_at(image, 0, 0) == kA && read_at(image, 1, 0) == kB && read_at(image, 0, 2) == kC);
	return 0;
}

// A literal that reaches the image's last pixel ends there: C, A (row 1) and A, B (row 0) are one
// literal of four, where the game's goes on into the memory before its buffer (its pair ahead, row 1's
// C and A, differs).
int test_literal_ends_at_the_last_pixel() {
	const auto rgba = frame({{kA, kB}, {kC, kA}});
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(tga_write_screenshot(rgba.data(), 2, 2, out, error));
	const std::vector<uint8_t> want = {0x03, 0x99, 0x88, 0x77, 0x33, 0x22, 0x11,
	                                   0x33, 0x22, 0x11, 0x66, 0x55, 0x44};
	TEST_EXPECT(packets(out) == want);
	return 0;
}

// The caps: a run of 128 (0xFF) and a literal of 128 (0x7F), each followed by the rest of its row.
int test_caps() {
	std::vector<uint32_t> top(130), bottom(130, kK);
	for (size_t i = 0; i < top.size(); ++i) top[i] = i % 2 == 0 ? kA : kB;
	const auto rgba = frame({top, bottom});
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(tga_write_screenshot(rgba.data(), 130, 2, out, error));
	std::vector<uint8_t> want = {0xFF, 0, 0, 0, 0x81, 0, 0, 0, 0x7F};
	for (int i = 0; i < 128; ++i) {
		const std::vector<uint8_t> px = i % 2 == 0 ? std::vector<uint8_t>{0x33, 0x22, 0x11}
		                                           : std::vector<uint8_t>{0x66, 0x55, 0x44};
		want.insert(want.end(), px.begin(), px.end());
	}
	const std::vector<uint8_t> tail = {0x01, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44};
	want.insert(want.end(), tail.begin(), tail.end());
	TEST_EXPECT(packets(out) == want);
	return 0;
}

// Refused as the other writers refuse: no pixels, a side past the header's 16 bits.
int test_refusals() {
	const auto rgba = frame({{kA}});
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(!tga_write_screenshot(rgba.data(), 0, 1, out, error) && out.empty() && !error.empty());
	TEST_EXPECT(!tga_write_screenshot(nullptr, 1, 1, out, error));
	const std::vector<uint8_t> wide(size_t(65536) * 4, 0);
	TEST_EXPECT(!tga_write_screenshot(wide.data(), 65536, 1, out, error));
	TEST_EXPECT(tga_write_screenshot(rgba.data(), 1, 1, out, error));
	TEST_EXPECT(packets(out) == std::vector<uint8_t>({0x00, 0x33, 0x22, 0x11}));
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_run_across_rows_and_the_last_pixel();
	failures += test_run_stops_at_a_row_end();
	failures += test_literal_ends_at_the_last_pixel();
	failures += test_caps();
	failures += test_refusals();
	if (failures == 0) std::printf("screenshot_tga: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
