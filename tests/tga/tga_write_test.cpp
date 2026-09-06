// The TGA encoder: the on-disk shape retail's texture loader and the menu
// cursor path depend on (tests/fixtures/minimal_art_validate.cpp pins the
// same predicates over the shipped cursor), the bottom-up row order, and the
// rejections. Mirrors the small-format tests (dbf, lwf).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include <formats/tga/tga_write.h>

using namespace opennova::tga;

namespace {

TgaImage gradient(int width, int height, int bpp) {
	TgaImage img;
	img.width = width;
	img.height = height;
	img.bpp = bpp;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			img.pixels.push_back(static_cast<uint8_t>(x));       // B
			img.pixels.push_back(static_cast<uint8_t>(y));       // G
			img.pixels.push_back(static_cast<uint8_t>(x + y));   // R
			if (bpp == 32) img.pixels.push_back(static_cast<uint8_t>(0x80 + y)); // A
		}
	}
	return img;
}

int check_header(const std::vector<uint8_t> &b, int width, int height, int bpp) {
	TEST_EXPECT(b.size() == TGA_HEADER_SIZE + static_cast<size_t>(width) * height * (bpp / 8));
	TEST_EXPECT(b[0] == 0 && b[1] == 0);            // no id field, no color map
	TEST_EXPECT(b[2] == TGA_IMAGE_TYPE_TRUECOLOR);
	TEST_EXPECT(std::memcmp(&b[3], "\0\0\0\0\0", 5) == 0); // color map spec
	TEST_EXPECT(std::memcmp(&b[8], "\0\0\0\0", 4) == 0);   // origin
	TEST_EXPECT((b[12] | (b[13] << 8)) == width);
	TEST_EXPECT((b[14] | (b[15] << 8)) == height);
	TEST_EXPECT(b[16] == bpp);
	TEST_EXPECT((b[17] & 0x0F) == (bpp == 32 ? 8 : 0)); // alpha bits
	TEST_EXPECT((b[17] & 0x20) == 0);                     // bottom-up rows
	return 0;
}

} // namespace

int main() {
	// 24 bpp: header, and the stored rows are the input rows reversed.
	{
		const TgaImage img = gradient(3, 2, 24);
		std::vector<uint8_t> out;
		TEST_EXPECT(tga_encode(img, out));
		if (check_header(out, 3, 2, 24) != 0) return 1;
		// First stored row == input row 1 (the bottom row); second == row 0.
		TEST_EXPECT(std::memcmp(&out[18], &img.pixels[9], 9) == 0);
		TEST_EXPECT(std::memcmp(&out[27], &img.pixels[0], 9) == 0);
		// Byte order is BGR: pixel (2, 0) has B=2, G=0, R=2.
		TEST_EXPECT(out[27 + 6] == 2 && out[27 + 7] == 0 && out[27 + 8] == 2);
	}
	// 32 bpp: the cursor shape (type 2, 8 alpha bits, bottom-up, full payload).
	{
		const TgaImage img = gradient(32, 32, 32);
		std::vector<uint8_t> out;
		TEST_EXPECT(tga_encode(img, out));
		if (check_header(out, 32, 32, 32) != 0) return 1;
		TEST_EXPECT(out[18 + 3] == static_cast<uint8_t>(0x80 + 31)); // bottom row's alpha
	}
	// A uniform 16x16 swatch is what assets/wall|roof|wood.tga are.
	{
		TgaImage img;
		img.width = 16;
		img.height = 16;
		img.bpp = 24;
		for (int i = 0; i < 16 * 16; ++i) {
			img.pixels.push_back(0x33);
			img.pixels.push_back(0x66);
			img.pixels.push_back(0x99);
		}
		std::vector<uint8_t> out;
		TEST_EXPECT(tga_encode(img, out));
		TEST_EXPECT(out.size() == 18 + 16 * 16 * 3);
		TEST_EXPECT(out[18] == 0x33 && out[19] == 0x66 && out[20] == 0x99);
	}
	// Rejections: empty, oversized, wrong depth, payload mismatch.
	{
		std::vector<uint8_t> out;
		TgaImage img;
		TEST_EXPECT(!tga_encode(img, out));
		img = gradient(2, 2, 24);
		img.bpp = 16;
		TEST_EXPECT(!tga_encode(img, out));
		img = gradient(2, 2, 24);
		img.pixels.pop_back();
		TEST_EXPECT(!tga_encode(img, out));
		img = gradient(2, 2, 24);
		img.width = 70000;
		TEST_EXPECT(!tga_encode(img, out));
		TEST_EXPECT(tga_write(nullptr, gradient(2, 2, 24)) == -1);
	}
	std::printf("OK: tga_write\n");
	return 0;
}
