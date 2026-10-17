// The PNG reader and writer (formats/png): the reader over every color type and depth with the five
// filters and its refusals (moved from the editor's import test), a palette image's indices kept beside
// its colours, the grey reading at the image's own depth, the IHDR size read without a decode; and the
// writer's RGBA image read back as it was written.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
	// The signature and one length-0 IEND header with no CRC: 16 bytes, fewer than a chunk's 12
	// framing bytes after its start (the bound once wrapped and the walk read the CRC past the end).
	const std::vector<uint8_t> cut_chunk = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 'I', 'E', 'N', 'D'};
	TEST_EXPECT(!decode_png(cut_chunk, image, error) && error.find("runs past the end") != std::string::npos);
	GrayImage gray_cut;
	TEST_EXPECT(!decode_png_gray(cut_chunk, gray_cut, error) && error.find("runs past the end") != std::string::npos);
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
	// A palette image (colour type 3): its indices and its palette's entries read back as written.
	const std::vector<uint8_t> indices = {0, 1, 2, 3, 19, 2, 1, 0, 7, 7};
	std::vector<uint8_t> palette;
	for (int i = 0; i < 20; ++i) palette.insert(palette.end(), {uint8_t(i * 10), uint8_t(200 - i), uint8_t(i * 3)});
	const std::vector<uint8_t> indexed = encode_png_indexed(indices.data(), 5, 2, palette.data(), 20);
	TEST_EXPECT(png_header_size(indexed, width, height) && width == 5 && height == 2 && indexed[25] == 3);
	IndexedImage8 read;
	TEST_EXPECT(decode_png(indexed, back, error, &read) && read.width == 5 && read.height == 2 && read.indices == indices);
	TEST_EXPECT(read.palette[19][0] == 190 && read.palette[19][1] == 181 && read.palette[19][2] == 57);
	// The PLTE chunk right after the 25-byte IHDR chunk: 20 entries, 60 bytes; no tRNS where none is given.
	const auto chunk_at = [](const std::vector<uint8_t> &png, size_t at, const char *type, uint32_t &length) {
		if (png.size() < at + 8) return false;
		length = (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
		return std::memcmp(&png[at + 4], type, 4) == 0;
	};
	uint32_t length = 0;
	TEST_EXPECT(chunk_at(indexed, 33, "PLTE", length) && length == 60);
	TEST_EXPECT(chunk_at(indexed, 33 + 12 + 60, "IDAT", length));
	TEST_EXPECT(encode_png_indexed(indices.data(), 5, 2, palette.data(), 0).empty() &&
	            encode_png_indexed(indices.data(), 5, 2, palette.data(), 257).empty());
	// An index past the palette's entries writes nothing (decode_png refuses such a file).
	TEST_EXPECT(encode_png_indexed(indices.data(), 5, 2, palette.data(), 19).empty());
	// The transparency kept: a tRNS of 3 entries after the PLTE, read back as those entries' alpha (the rest opaque).
	const uint8_t alpha[3] = {0, 128, 255};
	const std::vector<uint8_t> clear = encode_png_indexed(indices.data(), 5, 2, palette.data(), 20, alpha, 3);
	TEST_EXPECT(chunk_at(clear, 33 + 12 + 60, "tRNS", length) && length == 3);
	TEST_EXPECT(decode_png(clear, back, error) && back.pixels[3] == 0 && back.pixels[4 + 3] == 128 &&
	            back.pixels[2 * 4 + 3] == 255 && back.pixels[8 * 4 + 3] == 255);
	TEST_EXPECT(encode_png_indexed(indices.data(), 5, 2, palette.data(), 20, alpha, 21).empty() &&
	            encode_png_indexed(indices.data(), 5, 2, palette.data(), 20, nullptr, 3).empty());
	// What its chunks hold, handed back beside the texels: the PLTE's 20 entries and the tRNS's 3 bytes; none for
	// a palette image without a tRNS, nothing for a true-colour one; the IHDR's depth and colour type.
	PngPalette held;
	TEST_EXPECT(decode_png(clear, back, error, &read, &held) && held.entries == 20 &&
	            held.alpha == std::vector<uint8_t>(alpha, alpha + 3));
	TEST_EXPECT(decode_png(indexed, back, error, nullptr, &held) && held.entries == 20 && held.alpha.empty());
	TEST_EXPECT(decode_png(bytes, back, error, nullptr, &held) && held.entries == 0 && held.alpha.empty());
	uint8_t depth = 0, colour = 0;
	TEST_EXPECT(png_header_form(clear, depth, colour) && depth == 8 && colour == 3);
	TEST_EXPECT(png_header_form(bytes, depth, colour) && depth == 8 && colour == 6);
	TEST_EXPECT(!png_header_form(std::vector<uint8_t>(clear.begin(), clear.begin() + 25), depth, colour));
	// A grey image (colour type 0, 8 bits a sample): its levels read back as written.
	const std::vector<uint8_t> grey = encode_png_gray(indices.data(), 5, 2);
	GrayImage levels;
	TEST_EXPECT(!grey.empty() && grey[24] == 8 && grey[25] == 0 && decode_png_gray(grey, levels, error) &&
	            levels.width == 5 && levels.max_value == 255 && levels.samples[4] == 19 && levels.samples[8] == 7);
	TEST_EXPECT(encode_png_gray(nullptr, 5, 2).empty() && encode_png_gray(indices.data(), 0, 2).empty());
	// An RGB image (colour type 2, three samples a pixel): read back as written, opaque.
	std::vector<uint8_t> rgb;
	for (size_t i = 0; i < image.pixels.size(); i += 4)
		rgb.insert(rgb.end(), image.pixels.begin() + std::ptrdiff_t(i), image.pixels.begin() + std::ptrdiff_t(i + 3));
	const std::vector<uint8_t> three = encode_png_rgb(rgb.data(), 5, 3);
	TEST_EXPECT(png_header_size(three, width, height) && width == 5 && height == 3 && three[24] == 8 && three[25] == 2);
	TEST_EXPECT(decode_png(three, back, error) && back.width == 5 && back.height == 3);
	bool same = back.pixels.size() == image.pixels.size();
	for (size_t i = 0; same && i < back.pixels.size(); i += 4)
		same = back.pixels[i] == image.pixels[i] && back.pixels[i + 1] == image.pixels[i + 1] &&
		       back.pixels[i + 2] == image.pixels[i + 2] && back.pixels[i + 3] == 255;
	TEST_EXPECT(same);
	TEST_EXPECT(encode_png_rgb(nullptr, 5, 3).empty() && encode_png_rgb(rgb.data(), 5, 0).empty());
	std::printf("encode: an RGBA image, an RGB image, a palette image (its PLTE, its tRNS) and a grey image written "
	            "and read back as they were\n");
	return 0;
}

// The 16-bit grey writer (a heightmap): colour type 0 at 16 bits, every sample read back as written, the extremes
// and a byte-order witness included; nothing for an empty image.
int test_png_encode_gray16() {
	std::vector<uint16_t> samples;
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 6; ++x) samples.push_back(uint16_t(x * 13107 + y * 257));
	samples[0] = 0;
	samples[1] = 65535;
	samples[2] = 0x0102;
	const std::vector<uint8_t> bytes = encode_png_gray16(samples.data(), 6, 4);
	uint32_t width = 0, height = 0;
	TEST_EXPECT(png_header_size(bytes, width, height) && width == 6 && height == 4 && bytes[24] == 16 && bytes[25] == 0);
	GrayImage back;
	std::string error;
	TEST_EXPECT(decode_png_gray(bytes, back, error) && back.width == 6 && back.height == 4 && back.max_value == 65535);
	bool same = back.samples.size() == samples.size();
	for (size_t i = 0; same && i < samples.size(); ++i) same = back.samples[i] == samples[i];
	TEST_EXPECT(same);
	TEST_EXPECT(encode_png_gray16(samples.data(), 0, 4).empty() && encode_png_gray16(nullptr, 6, 4).empty());
	std::printf("encode: a 16-bit grey image written and read back as it was\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_png_decode();
	failures += test_png_indexed_gray_header();
	failures += test_png_encode();
	failures += test_png_encode_gray16();
	if (failures == 0) std::printf("png_codec: all passed\n");
	return failures == 0 ? 0 : 1;
}
