// The format's own TGA decode (editor/import/tga_source.h), the one an import source is read by: a 2 x 2
// image of four known pixels stored every way the format allows, each decoded to the same top-row-first
// picture: bottom-left and top-left origins, right-to-left rows, 24 and 32 bits, 16 bits with and
// without an alpha bit, 8-bit grey and 16-bit grey with alpha, a colour map of 24-bit and of 16-bit
// entries starting past entry 0, and the run-length forms; and what it refuses: a file that ends before
// its pixels, an index past the map, a depth the format does not define, a header cut short. The game's
// reader of the same top-left file reads it upside down (tga_read.h), which an import does not.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/import/tga_source.h>
#include <formats/tga/tga_read.h>

#include "common/test_expect.h"

using namespace opennova::tga;
using opennova::editor::decode_tga_source;

namespace {

// The picture, the top row first: red, green; blue, white (alphas 10, 20, 30, 40 where a form holds one).
const uint8_t kPicture[4][4] = {{255, 0, 0, 10}, {0, 255, 0, 20}, {0, 0, 255, 30}, {255, 255, 255, 40}};

std::vector<uint8_t> header(uint8_t type, uint8_t bits, uint8_t descriptor, uint8_t map_type = 0, uint16_t map_first = 0,
                            uint16_t map_length = 0, uint8_t map_bits = 0) {
	return {0,
	        map_type,
	        type,
	        uint8_t(map_first),
	        uint8_t(map_first >> 8),
	        uint8_t(map_length),
	        uint8_t(map_length >> 8),
	        map_bits,
	        0,
	        0,
	        0,
	        0,
	        2,
	        0,
	        2,
	        0,
	        bits,
	        descriptor};
}

// The picture's pixels in the order the descriptor stores them.
std::vector<int> order(uint8_t descriptor) {
	const bool top = descriptor & 0x20, right = descriptor & 0x10;
	std::vector<int> out;
	for (int r = 0; r < 2; ++r)
		for (int c = 0; c < 2; ++c) out.push_back((top ? r : 1 - r) * 2 + (right ? 1 - c : c));
	return out;
}

void bgra(std::vector<uint8_t> &out, int pixel, int bytes) {
	const uint8_t *p = kPicture[pixel];
	out.push_back(p[2]);
	out.push_back(p[1]);
	out.push_back(p[0]);
	if (bytes == 4) out.push_back(p[3]);
}

uint16_t five(int pixel, bool alpha) {
	const uint8_t *p = kPicture[pixel];
	return uint16_t((alpha && p[3] >= 128 ? 0x8000 : 0) | (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3));
}

bool decodes_to(const std::vector<uint8_t> &file, const std::vector<uint8_t> &expected) {
	TgaImage image;
	std::string error;
	if (!decode_tga_source(file.data(), file.size(), image, error)) {
		std::fprintf(stderr, "refused: %s\n", error.c_str());
		return false;
	}
	if (image.width != 2 || image.height != 2 || image.rgba != expected) {
		std::fprintf(stderr, "decoded otherwise:");
		for (uint8_t b : image.rgba) std::fprintf(stderr, " %u", b);
		std::fprintf(stderr, "\n");
		return false;
	}
	return true;
}

std::vector<uint8_t> picture(bool alpha) {
	std::vector<uint8_t> out;
	for (const auto &p : kPicture) out.insert(out.end(), {p[0], p[1], p[2], alpha ? p[3] : uint8_t(255)});
	return out;
}

} // namespace

int main() {
	// True colour at 32 and 24 bits, every origin.
	for (const uint8_t descriptor : {uint8_t(0x08), uint8_t(0x28), uint8_t(0x18), uint8_t(0x38)}) {
		std::vector<uint8_t> file = header(2, 32, descriptor);
		for (const int pixel : order(descriptor)) bgra(file, pixel, 4);
		TEST_EXPECT(decodes_to(file, picture(true)));
	}
	std::vector<uint8_t> rgb = header(2, 24, 0x20);
	for (const int pixel : order(0x20)) bgra(rgb, pixel, 3);
	TEST_EXPECT(decodes_to(rgb, picture(false)));
	// The game's reader takes that top-left file's first row as its bottom: upside down.
	TgaImage game;
	std::string error;
	TEST_EXPECT(tga_decode_retail(rgb.data(), rgb.size(), game, error) && game.rgba[0] == 0 && game.rgba[2] == 255);
	// 16 bits: 5 a channel, the top bit the alpha where the descriptor gives one alpha bit.
	const auto widen = [](uint8_t c) { return uint8_t(((c >> 3) << 3) | ((c >> 3) >> 2)); };
	std::vector<uint8_t> sixteen = picture(false);
	for (size_t i = 0; i < sixteen.size(); i += 4)
		for (size_t c = 0; c < 3; ++c) sixteen[i + c] = widen(sixteen[i + c]);
	for (const uint8_t alpha_bits : {uint8_t(0), uint8_t(1)}) {
		std::vector<uint8_t> file = header(2, 16, uint8_t(alpha_bits | 0x20));
		for (const int pixel : order(0x20)) {
			const uint16_t v = five(pixel, alpha_bits == 1);
			file.push_back(uint8_t(v));
			file.push_back(uint8_t(v >> 8));
		}
		std::vector<uint8_t> expected = sixteen;
		if (alpha_bits == 1)
			for (size_t i = 3; i < expected.size(); i += 4) expected[i] = 0; // every alpha below 128: the bit clear
		TEST_EXPECT(decodes_to(file, expected));
	}
	// Grey at 8 bits, and at 16 (grey then alpha).
	std::vector<uint8_t> grey8 = header(3, 8, 0x20), grey16 = header(3, 16, 0x28);
	std::vector<uint8_t> grey_expected, grey_alpha_expected;
	for (int pixel = 0; pixel < 4; ++pixel) {
		const uint8_t g = uint8_t(60 * pixel);
		grey8.push_back(g);
		grey16.insert(grey16.end(), {g, uint8_t(pixel * 50)});
		grey_expected.insert(grey_expected.end(), {g, g, g, 255});
		grey_alpha_expected.insert(grey_alpha_expected.end(), {g, g, g, uint8_t(pixel * 50)});
	}
	TEST_EXPECT(decodes_to(grey8, grey_expected) && decodes_to(grey16, grey_alpha_expected));
	// A colour map of 24-bit entries, indexed 8 bits; one of 16-bit entries from entry 5, indexed 16 bits.
	{
		std::vector<uint8_t> mapped = header(1, 8, 0x20, 1, 0, 4, 24);
		for (int pixel = 3; pixel >= 0; --pixel) bgra(mapped, pixel, 3); // the map reversed: entry k is pixel 3 - k
		for (const int pixel : order(0x20)) mapped.push_back(uint8_t(3 - pixel));
		TEST_EXPECT(decodes_to(mapped, picture(false)));
		std::vector<uint8_t> wide = header(1, 16, 0x20, 1, 5, 4, 16);
		for (int pixel = 0; pixel < 4; ++pixel) {
			const uint16_t v = five(pixel, false);
			wide.push_back(uint8_t(v));
			wide.push_back(uint8_t(v >> 8));
		}
		for (const int pixel : order(0x20)) wide.insert(wide.end(), {uint8_t(5 + pixel), 0});
		TEST_EXPECT(decodes_to(wide, sixteen));
		// An index past the map refused.
		std::vector<uint8_t> past = mapped;
		past.back() = 9;
		TgaImage refused;
		TEST_EXPECT(!decode_tga_source(past.data(), past.size(), refused, error) && error.find("past its colour map") != std::string::npos);
	}
	// Run-length: one run of the first row's two pixels' worth and a raw packet, at 32 bits.
	{
		std::vector<uint8_t> rle = header(10, 32, 0x28);
		rle.push_back(0x80); // a run of one: pixel 0
		bgra(rle, 0, 4);
		rle.push_back(0x02); // raw, three pixels
		for (const int pixel : {1, 2, 3}) bgra(rle, pixel, 4);
		TEST_EXPECT(decodes_to(rle, picture(true)));
		// Cut short: refused, never read past its end.
		std::vector<uint8_t> cut(rle.begin(), rle.end() - 2);
		TgaImage refused;
		TEST_EXPECT(!decode_tga_source(cut.data(), cut.size(), refused, error) && error.find("ends before its pixels") != std::string::npos);
	}
	// Refused: a raw file cut short, a depth no form defines, a header cut short, an image type of none.
	{
		std::vector<uint8_t> cut = header(2, 32, 0x08);
		for (int i = 0; i < 10; ++i) cut.push_back(1);
		TgaImage refused;
		TEST_EXPECT(!decode_tga_source(cut.data(), cut.size(), refused, error));
		std::vector<uint8_t> odd = header(2, 12, 0);
		odd.resize(odd.size() + 64, 0);
		TEST_EXPECT(!decode_tga_source(odd.data(), odd.size(), refused, error));
		TEST_EXPECT(!decode_tga_source(odd.data(), 10, refused, error));
		std::vector<uint8_t> none = header(7, 32, 0);
		none.resize(none.size() + 64, 0);
		TEST_EXPECT(!decode_tga_source(none.data(), none.size(), refused, error));
	}
	std::printf("tga_source: every origin, depth, colour map and run-length form decoded upright; the refusals\n");
	return 0;
}
