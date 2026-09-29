// The TGA writer (engine/formats/tga) over a 3 by 2 image of six distinct pixels: every byte
// of the header (image type 2, 32 bits a pixel, 8 alpha bits, the origin at the bottom left),
// the pixels B, G, R, A from the bottom row up, the image read back the way the game's TGA
// readers read one (the block copied as it is, then the rows turned upright: tga.cpp's
// witnesses), and what the writer refuses (no pixels, a side past the header's 16 bits).
// The header's size read back (tga_header_size): the written file's, each image type that
// holds an image, and what it refuses (a short header, an image type that holds none).
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <formats/tga/tga.h>

#include "common/test_expect.h"

using namespace opennova::tga;

int main() {
	// Pixel k (the top row 0, 1, 2, the bottom row 3, 4, 5) is R, G, B, A = 10k .. 10k + 3.
	std::vector<uint8_t> rgba;
	for (int k = 0; k < 6; ++k)
		for (int c = 0; c < 4; ++c) rgba.push_back(uint8_t(10 * k + c));
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(tga_write_rgba32(rgba.data(), 3, 2, out, error));
	TEST_EXPECT(out.size() == TGA_HEADER_SIZE + 6 * 4);
	const std::vector<uint8_t> header = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2, 0, 32, 8};
	TEST_EXPECT(std::vector<uint8_t>(out.begin(), out.begin() + TGA_HEADER_SIZE) == header);
	const std::vector<uint8_t> pixels = {32, 31, 30, 33, 42, 41, 40, 43, 52, 51, 50, 53,
	                                     2,  1,  0,  3,  12, 11, 10, 13, 22, 21, 20, 23};
	TEST_EXPECT(std::vector<uint8_t>(out.begin() + TGA_HEADER_SIZE, out.end()) == pixels);

	// As the game reads it: the image ID skipped (byte 0 long), the block copied, the rows
	// turned upright; the pixels it holds then are the image's, B and R swapped.
	const size_t width = size_t(out[12]) | size_t(out[13]) << 8, height = size_t(out[14]) | size_t(out[15]) << 8;
	TEST_EXPECT(out[2] == 2 && out[16] == 32 && width == 3 && height == 2);
	std::vector<uint8_t> read(out.begin() + TGA_HEADER_SIZE + out[0], out.end());
	for (size_t row = 0; row < height / 2; ++row)
		for (size_t i = 0; i < width * 4; ++i) std::swap(read[row * width * 4 + i], read[(height - 1 - row) * width * 4 + i]);
	for (size_t i = 0; i < width * height; ++i)
		TEST_EXPECT(read[i * 4] == rgba[i * 4 + 2] && read[i * 4 + 1] == rgba[i * 4 + 1] && read[i * 4 + 2] == rgba[i * 4] &&
		            read[i * 4 + 3] == rgba[i * 4 + 3]);

	// Refused: no pixels, a side past the header's 16 bits.
	TEST_EXPECT(!tga_write_rgba32(rgba.data(), 0, 2, out, error) && out.empty() && !error.empty());
	TEST_EXPECT(!tga_write_rgba32(nullptr, 3, 2, out, error));
	const std::vector<uint8_t> wide(size_t(65536) * 4, 0);
	TEST_EXPECT(!tga_write_rgba32(wide.data(), 65536, 1, out, error));

	// The header's size.
	TEST_EXPECT(tga_write_rgba32(rgba.data(), 3, 2, out, error));
	uint32_t w = 0, h = 0;
	TEST_EXPECT(tga_header_size(out.data(), out.size(), w, h) && w == 3 && h == 2);
	std::vector<uint8_t> big = header;
	big[12] = 0x34;
	big[13] = 0x12;
	big[14] = 0x78;
	big[15] = 0x56;
	for (int type : {1, 2, 3, 9, 10, 11}) {
		big[2] = uint8_t(type);
		TEST_EXPECT(tga_header_size(big.data(), big.size(), w, h) && w == 0x1234 && h == 0x5678);
	}
	for (int type : {0, 4, 8, 12, 32, 33}) {
		big[2] = uint8_t(type);
		TEST_EXPECT(!tga_header_size(big.data(), big.size(), w, h));
	}
	TEST_EXPECT(!tga_header_size(out.data(), TGA_HEADER_SIZE - 1, w, h) && !tga_header_size(nullptr, 18, w, h));
	std::printf("tga_write: the header, the pixels from the bottom row up, read back as the game reads them; "
	            "the header's size\n");
	return 0;
}
