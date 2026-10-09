// The PNG reader and writer (formats/png): the reader over every color type and depth with the five
// filters and its refusals (moved from the editor's import test), a palette image's indices kept beside
// its colours, the grey reading at the image's own depth, the IHDR size read without a decode; and the
// writer's RGBA image read back as it was written.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/pcx/pcx.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>

#include "common/png_test_support.h"
#include "common/test_expect.h"

using opennova::IndexedImage8;
using opennova::RgbaImage;
using namespace opennova::png;
using test_png::make_png;
using test_png::PngSpec;

namespace {

const uint8_t *pixel(const RgbaImage &image, int x, int y) { return &image.pixels[size_t((y * image.width + x) * 4)]; }

int test_png_decode() {
	// RGBA 8-bit, two rows, the second filtered with Sub (each byte adds the one bpp back).
	PngSpec rgba;
	rgba.width = 2;
	rgba.height = 2;
	rgba.rows = {0, 10, 20, 30, 255, 40, 50, 60, 128,
	             1, 1, 2, 3, 4, 1, 1, 1, 1};
	RgbaImage image;
	std::string error;
	TEST_EXPECT(decode_png(make_png(rgba), image, error));
	TEST_EXPECT(image.width == 2 && image.height == 2);
	TEST_EXPECT(pixel(image, 0, 0)[0] == 10 && pixel(image, 0, 0)[3] == 255 && pixel(image, 1, 0)[3] == 128);
	TEST_EXPECT(pixel(image, 0, 1)[0] == 1 && pixel(image, 0, 1)[1] == 2 && pixel(image, 1, 1)[0] == 2 && pixel(image, 1, 1)[3] == 5);
	// RGB 8-bit with Up (row 2 adds row 1) and Average and Paeth rows.
	PngSpec rgb;
	rgb.width = 1;
	rgb.height = 4;
	rgb.color_type = 2;
	rgb.rows = {0, 100, 110, 120, 2, 1, 1, 1, 3, 2, 2, 2, 4, 3, 3, 3};
	TEST_EXPECT(decode_png(make_png(rgb), image, error));
	TEST_EXPECT(pixel(image, 0, 1)[0] == 101 && pixel(image, 0, 2)[0] == 52 && pixel(image, 0, 3)[0] == 55 && pixel(image, 0, 3)[3] == 255);
	// A 2-bit palette image with transparency.
	PngSpec paletted;
	paletted.width = 4;
	paletted.height = 1;
	paletted.depth = 2;
	paletted.color_type = 3;
	paletted.palette = {255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9};
	paletted.palette_alpha = {255, 128};
	paletted.rows = {0, 0x1B}; // indices 0,1,2,3
	TEST_EXPECT(decode_png(make_png(paletted), image, error));
	TEST_EXPECT(image.width == 4 && pixel(image, 0, 0)[0] == 255 && pixel(image, 1, 0)[1] == 255 && pixel(image, 1, 0)[3] == 128 &&
	            pixel(image, 2, 0)[2] == 255 && pixel(image, 3, 0)[0] == 9 && pixel(image, 3, 0)[3] == 255);
	// 1-bit grayscale, 16-bit grayscale + alpha.
	PngSpec gray;
	gray.width = 8;
	gray.height = 1;
	gray.depth = 1;
	gray.color_type = 0;
	gray.rows = {0, 0xA5};
	TEST_EXPECT(decode_png(make_png(gray), image, error));
	TEST_EXPECT(pixel(image, 0, 0)[0] == 255 && pixel(image, 1, 0)[0] == 0 && pixel(image, 7, 0)[0] == 255);
	PngSpec deep;
	deep.width = 1;
	deep.height = 1;
	deep.depth = 16;
	deep.color_type = 4;
	deep.rows = {0, 0x12, 0x34, 0xAB, 0xCD};
	TEST_EXPECT(decode_png(make_png(deep), image, error));
	TEST_EXPECT(pixel(image, 0, 0)[0] == 0x12 && pixel(image, 0, 0)[3] == 0xAB);
	// Refusals name the reason.
	PngSpec interlaced = rgba;
	interlaced.interlace = 1;
	TEST_EXPECT(!decode_png(make_png(interlaced), image, error) && error.find("interlaced") != std::string::npos);
	PngSpec bad_crc = rgba;
	bad_crc.corrupt_crc = true;
	TEST_EXPECT(!decode_png(make_png(bad_crc), image, error) && error.find("CRC") != std::string::npos);
	PngSpec bad_depth = paletted;
	bad_depth.depth = 16;
	TEST_EXPECT(!decode_png(make_png(bad_depth), image, error) && error.find("Unsupported") != std::string::npos);
	std::vector<uint8_t> truncated = make_png(rgba);
	truncated.resize(truncated.size() - 20);
	TEST_EXPECT(!decode_png(truncated, image, error));
	TEST_EXPECT(!decode_png({1, 2, 3}, image, error) && error.find("Not a PNG") != std::string::npos);
	std::printf("decode: every color type and depth, the five filters, the refusals\n");
	return 0;
}

// A palette image's indices and palette beside its colours, where asked; the grey reading; the size an
// IHDR states.
int test_png_indexed_gray_header() {
	PngSpec paletted;
	paletted.width = 4;
	paletted.height = 1;
	paletted.depth = 2;
	paletted.color_type = 3;
	paletted.palette = {255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9};
	paletted.rows = {0, 0x1B}; // indices 0,1,2,3
	RgbaImage image;
	IndexedImage8 indexed;
	std::string error;
	TEST_EXPECT(decode_png(make_png(paletted), image, error, &indexed));
	TEST_EXPECT(indexed.width == 4 && indexed.height == 1 && indexed.indices == std::vector<uint8_t>({0, 1, 2, 3}) &&
	            indexed.palette[1][1] == 255 && indexed.palette[3][0] == 9 && indexed.palette[4][0] == 0);
	// A colour image leaves the indices empty.
	TEST_EXPECT(decode_png(test_png::gradient_png(2, 2), image, error, &indexed) && indexed.indices.empty());
	// Grey at its own depth: 16 bits whole, a sub-byte grey scaled, a colour image's channels averaged.
	PngSpec deep;
	deep.width = 2;
	deep.height = 1;
	deep.depth = 16;
	deep.color_type = 0;
	deep.rows = {0, 0x12, 0x34, 0xFF, 0xFF};
	GrayImage grey;
	TEST_EXPECT(decode_png_gray(make_png(deep), grey, error));
	TEST_EXPECT(grey.width == 2 && grey.max_value == 65535 && grey.samples == std::vector<uint16_t>({0x1234, 0xFFFF}));
	PngSpec bits;
	bits.width = 2;
	bits.height = 1;
	bits.depth = 2;
	bits.color_type = 0;
	bits.rows = {0, 0x70}; // samples 1, 3
	TEST_EXPECT(decode_png_gray(make_png(bits), grey, error) && grey.max_value == 255 &&
	            grey.samples == std::vector<uint16_t>({85, 255}));
	PngSpec colour;
	colour.width = 1;
	colour.height = 1;
	colour.color_type = 2;
	colour.rows = {0, 30, 60, 91};
	TEST_EXPECT(decode_png_gray(make_png(colour), grey, error) && grey.samples == std::vector<uint16_t>({(30 + 60 + 91 + 1) / 3}));
	// The IHDR's size, read without a decode; refused for bytes that are not a PNG.
	uint32_t width = 0, height = 0;
	const std::vector<uint8_t> big = test_png::gradient_png(7, 3);
	TEST_EXPECT(is_png(big) && png_header_size(big, width, height) && width == 7 && height == 3);
	TEST_EXPECT(!is_png({1, 2, 3}) && !png_header_size({0x89, 'P', 'N', 'G'}, width, height));
	std::printf("indexed, grey and header: a palette image's indices, grey at its depth, the IHDR's size\n");
	return 0;
}

// The writer: an RGBA image as a PNG of colour type 6, read back texel for texel; nothing for an empty
// image.
int test_png_encode() {
	RgbaImage image;
	image.width = 5;
	image.height = 3;
	for (int y = 0; y < image.height; ++y)
		for (int x = 0; x < image.width; ++x)
			image.pixels.insert(image.pixels.end(), {uint8_t(x * 50), uint8_t(y * 80), uint8_t((x + y) * 20), uint8_t(255 - x * 40)});
	const std::vector<uint8_t> bytes = encode_png_rgba(image.pixels.data(), 5, 3);
	uint32_t width = 0, height = 0;
	TEST_EXPECT(png_header_size(bytes, width, height) && width == 5 && height == 3 && bytes[25] == 6);
	RgbaImage back;
	std::string error;
	TEST_EXPECT(decode_png(bytes, back, error) && back.width == 5 && back.height == 3 && back.pixels == image.pixels);
	TEST_EXPECT(encode_png_rgba(image.pixels.data(), 0, 3).empty() && encode_png_rgba(nullptr, 5, 3).empty());
	std::printf("encode: an RGBA image written and read back as it was\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_png_decode();
	failures += test_png_indexed_gray_header();
	failures += test_png_encode();
	if (failures == 0) std::printf("png_codec: all passed\n");
	return failures == 0 ? 0 : 1;
}
