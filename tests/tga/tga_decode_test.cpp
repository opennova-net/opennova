// The game's TGA reader (engine/formats/tga tga_decode_game, a structural port of
// CTerrainTileData_LoadTGAFromArchive @ 0x56E570, whose decode CUIImage_LoadTGA @ 0x6647D0 repeats)
// over images minted here: the writer's 32-bit image read back texel for texel; a 24-bit one opaque;
// an 8-bit grey one; a colour-mapped one with 24-bit entries (its map and indices kept, an index past
// the map read from the bytes after it as the reader does); run-length 24 and 32-bit images, a run and
// a literal packet each; the rows always taken bottom up, a top-left descriptor included (the reader
// ignores the bit); the forms it zeroes (types 9 and 11, a 16-bit true-colour image, a 16-bit map's
// entries) and those it leaves unset (a 16-bit grey image, type 0); a short file's missing bytes read as
// 0; a side of 0 no texel; and a header shorter than 18 bytes refused.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/tga/tga.h>

#include "common/test_expect.h"

using namespace opennova::tga;

namespace {

// An 18-byte header.
std::vector<uint8_t> header(uint8_t type, uint16_t width, uint16_t height, uint8_t bits, uint8_t descriptor = 0,
                            uint8_t map_type = 0, uint16_t map_length = 0, uint8_t map_bits = 0) {
	return {0,
	        map_type,
	        type,
	        0,
	        0,
	        uint8_t(map_length),
	        uint8_t(map_length >> 8),
	        map_bits,
	        0,
	        0,
	        0,
	        0,
	        uint8_t(width),
	        uint8_t(width >> 8),
	        uint8_t(height),
	        uint8_t(height >> 8),
	        bits,
	        descriptor};
}

bool texel_is(const TgaImage &image, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	const uint8_t *t = image.rgba.data() + (size_t(y) * image.width + x) * 4;
	return t[0] == r && t[1] == g && t[2] == b && t[3] == a;
}

bool all_zero(const TgaImage &image) {
	for (uint8_t byte : image.rgba)
		if (byte) return false;
	return !image.rgba.empty();
}

} // namespace

int main() {
	std::string error;
	// The writer's image read back: every texel as written, the top row first.
	std::vector<uint8_t> rgba;
	for (int k = 0; k < 6; ++k)
		for (int c = 0; c < 4; ++c) rgba.push_back(uint8_t(10 * k + c));
	std::vector<uint8_t> file;
	TEST_EXPECT(tga_write_rgba32(rgba.data(), 3, 2, file, error));
	TgaImage image;
	TEST_EXPECT(tga_decode_game(file.data(), file.size(), image, error));
	TEST_EXPECT(image.pixels == TgaPixels::Decoded && image.width == 3 && image.height == 2 && image.rgba == rgba);
	TEST_EXPECT(image.header.image_type == 2 && image.header.bits == 32 && image.header.alpha_bits() == 8 &&
	            !image.header.top_first());

	// 24 bits a texel, 2 x 2, the file's rows bottom up: B, G, R each, alpha 0xFF.
	std::vector<uint8_t> t24 = header(2, 2, 2, 24);
	for (uint8_t v : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) t24.push_back(v);
	TEST_EXPECT(tga_decode_game(t24.data(), t24.size(), image, error) && image.pixels == TgaPixels::Decoded);
	// The file's first row (bytes 1..6) is the bottom one.
	TEST_EXPECT(texel_is(image, 0, 1, 3, 2, 1, 255) && texel_is(image, 1, 1, 6, 5, 4, 255));
	TEST_EXPECT(texel_is(image, 0, 0, 9, 8, 7, 255) && texel_is(image, 1, 0, 12, 11, 10, 255));

	// A top-left descriptor: the reader takes the rows bottom up all the same.
	std::vector<uint8_t> top = t24;
	top[17] = 0x20;
	TgaImage flipped;
	TEST_EXPECT(tga_decode_game(top.data(), top.size(), flipped, error) && flipped.header.top_first());
	TEST_EXPECT(flipped.rgba == image.rgba);

	// 8-bit grey.
	std::vector<uint8_t> grey = header(3, 2, 1, 8);
	grey.push_back(40);
	grey.push_back(200);
	TEST_EXPECT(tga_decode_game(grey.data(), grey.size(), image, error) && image.pixels == TgaPixels::Decoded);
	TEST_EXPECT(texel_is(image, 0, 0, 40, 40, 40, 255) && texel_is(image, 1, 0, 200, 200, 200, 255));

	// Colour-mapped, two 24-bit entries (B, G, R), then four indices; index 2 reads past the map, from the
	// bytes after it (the indices themselves), as the reader does.
	std::vector<uint8_t> mapped = header(1, 2, 2, 8, 0, 1, 2, 24);
	for (uint8_t v : {10, 20, 30, 40, 50, 60}) mapped.push_back(v);
	for (uint8_t v : {0, 1, 1, 2}) mapped.push_back(v);
	TEST_EXPECT(tga_decode_game(mapped.data(), mapped.size(), image, error) && image.pixels == TgaPixels::Decoded);
	TEST_EXPECT(image.palette == std::vector<uint8_t>({30, 20, 10, 60, 50, 40}));
	// The file's indices 0, 1 are the bottom row; the map's entry 0 is R 30, G 20, B 10.
	TEST_EXPECT(texel_is(image, 0, 1, 30, 20, 10, 255) && texel_is(image, 1, 1, 60, 50, 40, 255));
	TEST_EXPECT(texel_is(image, 0, 0, 60, 50, 40, 255));
	// Entry 2 is the three bytes after the map: indices 0, 1, 1 read as B, G, R.
	TEST_EXPECT(texel_is(image, 1, 0, 1, 1, 0, 255));
	TEST_EXPECT(image.indices == std::vector<uint8_t>({1, 2, 0, 1}));

	// Run-length, 24 bits: a run of 3 of one texel, then a literal of 1.
	std::vector<uint8_t> rle = header(10, 4, 1, 24);
	for (uint8_t v : {0x82, 1, 2, 3, 0x00, 4, 5, 6}) rle.push_back(v);
	TEST_EXPECT(tga_decode_game(rle.data(), rle.size(), image, error) && image.pixels == TgaPixels::Decoded);
	TEST_EXPECT(texel_is(image, 0, 0, 3, 2, 1, 255) && texel_is(image, 2, 0, 3, 2, 1, 255) &&
	            texel_is(image, 3, 0, 6, 5, 4, 255));
	// 32 bits: a literal of 2, then a run of 2 that the image ends inside.
	std::vector<uint8_t> rle32 = header(10, 3, 1, 32);
	for (uint8_t v : {0x01, 1, 2, 3, 4, 5, 6, 7, 8, 0x81, 9, 10, 11, 12}) rle32.push_back(v);
	TEST_EXPECT(tga_decode_game(rle32.data(), rle32.size(), image, error) && image.pixels == TgaPixels::Decoded);
	TEST_EXPECT(texel_is(image, 0, 0, 3, 2, 1, 4) && texel_is(image, 1, 0, 7, 6, 5, 8) && texel_is(image, 2, 0, 11, 10, 9, 12));

	// The forms the reader zeroes, and those it leaves unset (read as blank).
	for (const std::vector<uint8_t> &zeroed :
	     {header(9, 2, 2, 8, 0, 1, 2, 24), header(11, 2, 2, 8), header(2, 2, 2, 16), header(10, 2, 2, 16),
	      header(1, 2, 2, 8, 0, 1, 2, 16)}) {
		std::vector<uint8_t> bytes = zeroed;
		bytes.resize(bytes.size() + 64, 0x7F);
		TEST_EXPECT(tga_decode_game(bytes.data(), bytes.size(), image, error) && image.pixels == TgaPixels::Blank &&
		            all_zero(image));
	}
	for (const std::vector<uint8_t> &unset : {header(3, 2, 2, 16), header(0, 2, 2, 32), header(5, 2, 2, 32)}) {
		std::vector<uint8_t> bytes = unset;
		bytes.resize(bytes.size() + 64, 0x7F);
		TEST_EXPECT(tga_decode_game(bytes.data(), bytes.size(), image, error) && image.pixels == TgaPixels::Unset &&
		            all_zero(image));
	}

	// A short file: the bytes it lacks read as 0.
	std::vector<uint8_t> shortened = header(2, 2, 1, 32);
	for (uint8_t v : {1, 2, 3, 4}) shortened.push_back(v);
	TEST_EXPECT(tga_decode_game(shortened.data(), shortened.size(), image, error) && image.pixels == TgaPixels::Decoded);
	TEST_EXPECT(texel_is(image, 0, 0, 3, 2, 1, 4) && texel_is(image, 1, 0, 0, 0, 0, 0));

	// A side of 0, or past 32767 (the reader's signed 16-bit side): no texel.
	const std::vector<uint8_t> empty = header(2, 0, 4, 32);
	TEST_EXPECT(tga_decode_game(empty.data(), empty.size(), image, error) && image.rgba.empty() && image.width == 0);
	const std::vector<uint8_t> huge = header(2, 40000, 1, 32);
	TEST_EXPECT(tga_decode_game(huge.data(), huge.size(), image, error) && image.rgba.empty());

	// Refused: no header.
	TEST_EXPECT(!tga_decode_game(file.data(), 17, image, error) && !error.empty());
	std::printf("tga_decode: the writer's image, 24-bit, grey, colour-mapped and run-length forms as the game's reader "
	            "takes them, rows bottom up whatever the descriptor, the forms it zeroes or leaves unset\n");
	return 0;
}
