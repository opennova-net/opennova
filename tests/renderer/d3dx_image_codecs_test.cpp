// D3DX's PPM, PFM, HDR and DIB codecs (renderer/d3dx_image_codecs.h): the variants each
// codec takes, the ones it fails, the exact bytes its pixels convert to, the HDR run
// forms and axis orders, the PFM row order and byte order, the 8-bit float encode, and
// the DIB's synthesized file header.

#include <runtime/renderer/d3dx_image_codecs.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace opennova::renderer;
using opennova::RgbaImage;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

std::vector<uint8_t> text(const std::string &s) {
	return std::vector<uint8_t>(s.begin(), s.end());
}

void add(std::vector<uint8_t> &bytes, std::initializer_list<uint8_t> more) {
	bytes.insert(bytes.end(), more.begin(), more.end());
}

void add_u16(std::vector<uint8_t> &bytes, uint16_t v) {
	add(bytes, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
}

void add_u32(std::vector<uint8_t> &bytes, uint32_t v) {
	add(bytes, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
					   static_cast<uint8_t>(v >> 24)});
}

uint32_t bits_of(float f) {
	uint32_t v;
	std::memcpy(&v, &f, sizeof(v));
	return v;
}

void add_f32_le(std::vector<uint8_t> &bytes, float f) {
	add_u32(bytes, bits_of(f));
}

void add_f32_be(std::vector<uint8_t> &bytes, float f) {
	const uint32_t v = bits_of(f);
	add(bytes, {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
					   static_cast<uint8_t>(v)});
}

uint32_t u32_at(const std::vector<uint8_t> &bytes, size_t at) {
	return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
			(static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

bool pixel_is(const RgbaImage &image, int x, int y, int r, int g, int b, int a = 255) {
	if (x < 0 || y < 0 || x >= image.width || y >= image.height) return false;
	const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4u;
	if (image.pixels.size() < at + 4) return false;
	const bool same = image.pixels[at] == r && image.pixels[at + 1] == g && image.pixels[at + 2] == b &&
			image.pixels[at + 3] == a;
	if (!same)
		std::fprintf(stderr, "  pixel (%d,%d) is (%d,%d,%d,%d), wanted (%d,%d,%d,%d)\n", x, y, image.pixels[at],
				image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3], r, g, b, a);
	return same;
}

bool ppm(const std::vector<uint8_t> &bytes, RgbaImage &image) {
	std::string error;
	return decode_d3dx_ppm(bytes.data(), bytes.size(), image, error);
}

bool pfm(const std::vector<uint8_t> &bytes, RgbaImage &image) {
	std::string error;
	return decode_d3dx_pfm(bytes.data(), bytes.size(), image, error);
}

bool hdr(const std::vector<uint8_t> &bytes, RgbaImage &image) {
	std::string error;
	return decode_d3dx_hdr(bytes.data(), bytes.size(), image, error);
}

void test_unorm8() {
	// [orig: D3DXTex::CCodec_A8R8G8B8::Encode @ 0x6E57F9] channel * 255 + 0.5, truncated.
	CHECK(d3dx_float_to_unorm8(0.0f) == 0, "0 -> 0");
	CHECK(d3dx_float_to_unorm8(1.0f) == 255, "1 -> 255");
	CHECK(d3dx_float_to_unorm8(0.5f) == 128, "127.5 + 0.5 truncates to 128");
	CHECK(d3dx_float_to_unorm8(0.25f) == 64, "63.75 + 0.5 truncates to 64");
	CHECK(d3dx_float_to_unorm8(127.0f / 255.0f) == 127, "an exact level stays");
	CHECK(d3dx_float_to_unorm8(0.998f) == 254, "254.49 + 0.5 truncates to 254");
	CHECK(d3dx_float_to_unorm8(0.001f) == 0, "0.255 + 0.5 truncates to 0");
	CHECK(d3dx_float_to_unorm8(-0.5f) == 0 && d3dx_float_to_unorm8(-1e30f) == 0, "negatives clamp to 0");
	CHECK(d3dx_float_to_unorm8(300.0f) == 255, "past 1 clamps to 255");
	CHECK(d3dx_float_to_unorm8(8421504.0f) == 255, "the largest sum under 2^31 clamps to 255");
	CHECK(d3dx_float_to_unorm8(8421505.0f) == 0, "a sum at or past 2^31 is the truncation's 0x80000000: 0");
	CHECK(d3dx_float_to_unorm8(std::numeric_limits<float>::infinity()) == 0, "+inf is 0");
	CHECK(d3dx_float_to_unorm8(-std::numeric_limits<float>::infinity()) == 0, "-inf is 0");
	CHECK(d3dx_float_to_unorm8(std::numeric_limits<float>::quiet_NaN()) == 0, "NaN is 0");
}

void test_ppm_binary() {
	RgbaImage image;
	std::vector<uint8_t> bytes = text("P6\n2 1\n255\n");
	add(bytes, {10, 20, 30, 40, 50, 60});
	CHECK(ppm(bytes, image) && image.width == 2 && image.height == 1, "P6 2x1 decodes");
	CHECK(pixel_is(image, 0, 0, 10, 20, 30) && pixel_is(image, 1, 0, 40, 50, 60), "P6 bytes, opaque");

	// maxval 15: each sample 255 * s / 15; a sample past maxval bleeds into the word's
	// higher channels (green 272 = 0x110 sets red's low bit) [orig: @ 0x6DD228..0x6DD266].
	bytes = text("P6\n1 1\n15\n");
	add(bytes, {0, 16, 1});
	CHECK(ppm(bytes, image) && pixel_is(image, 0, 0, 1, 16, 17), "P6 maxval 15 scales and bleeds");

	// One '\r' and then one more byte are skipped after maxval [orig: @ 0x6DD20B..0x6DD21B].
	bytes = text("P6\n1 1\n255\r\n");
	add(bytes, {7, 8, 9});
	CHECK(ppm(bytes, image) && pixel_is(image, 0, 0, 7, 8, 9), "CRLF after maxval");
	bytes = text("P6\n1 1\n255\r");
	add(bytes, {0xAA, 7, 8, 9});
	CHECK(ppm(bytes, image) && pixel_is(image, 0, 0, 7, 8, 9), "a bare CR eats the next byte too");

	// A comment through its '\n', and no whitespace needed after the magic.
	bytes = text("P6# made by hand\n1 1 255\n");
	add(bytes, {1, 2, 3});
	CHECK(ppm(bytes, image) && pixel_is(image, 0, 0, 1, 2, 3), "a comment right after the magic");
	bytes = text("P61\t1 255 ");
	add(bytes, {4, 5, 6, 99});
	CHECK(ppm(bytes, image) && pixel_is(image, 0, 0, 4, 5, 6), "the width right after the magic; extra bytes ignored");

	bytes = text("P6\n2 1\n255\n");
	add(bytes, {1, 2, 3, 4, 5});
	CHECK(!ppm(bytes, image), "a pixel short (not a multiple of 3: retail reads past its buffer)");
	bytes = text("P6\n2 1\n255\n");
	add(bytes, {1, 2, 3});
	CHECK(!ppm(bytes, image), "a pixel short on a pixel boundary fails");
	bytes = text("P6\n1 1\n256\n");
	add(bytes, {1, 2, 3});
	CHECK(!ppm(bytes, image), "P6 maxval past 255 fails");
	bytes = text("P6\n1 1\n0\n");
	add(bytes, {1, 2, 3});
	CHECK(!ppm(bytes, image), "maxval 0 fails");
	bytes = text("P6\n0 1\n255\n");
	add(bytes, {1, 2, 3});
	CHECK(!ppm(bytes, image), "zero width fails");
	CHECK(!ppm(text("P6\n100000 100000\n255\n"), image), "a header past the pixel bound fails");
	CHECK(!ppm(text("P6\n4000 4000\n255\n"), image), "a header its data cannot back fails");
}

void test_ppm_ascii() {
	RgbaImage image;
	// CRLF, comments between tokens, a tab, maxval past 255 (ASCII never checks it),
	// and the last value at the end of the data.
	CHECK(ppm(text("P3\r\n# comment\r\n2 1\r\n1000\r\n0 500 1000\t# mid\n250 999 1"), image) &&
					image.width == 2 && image.height == 1,
			"P3 decodes");
	CHECK(pixel_is(image, 0, 0, 0, 127, 255) && pixel_is(image, 1, 0, 63, 254, 0), "255 * s / 1000, truncated");
	CHECK(ppm(text("P3 1 1 255 1 2 3 xyz"), image) && pixel_is(image, 0, 0, 1, 2, 3),
			"the decode ends at the last pixel; what follows is never read");
	// maxval 1, green 2: 510 = 0x1FE bleeds into red.
	CHECK(ppm(text("P3 1 1 1 0 2 0\n"), image) && pixel_is(image, 0, 0, 1, 254, 0), "an ASCII sample past maxval bleeds");
	CHECK(!ppm(text("P3 1 1 255 1 2x 3\n"), image), "a token holding a non-digit fails");
	CHECK(!ppm(text("P3 1 1 255 1 2"), image), "the data ending inside a pixel fails");
	CHECK(!ppm(text("P3 1 1 255 1 2 3#c\n"), image), "a '#' glued to a value is a non-digit, even the last");
	CHECK(ppm(text("P3 1 1 255 1 2 3 #c"), image), "a comment after the last value is never read");
}

void test_ppm_rejects() {
	RgbaImage image;
	CHECK(!ppm(text("P"), image), "under two bytes");
	CHECK(!ppm(text("P6"), image), "nothing after the magic");
	CHECK(!ppm(text("P5\n1 1\n255\n\x01"), image), "P5 (grey) is not taken");
	CHECK(!ppm(text("P2 1 1 255 1\n"), image), "P2 is not taken");
	CHECK(!ppm(text("p6 1 1 255 abc"), image), "lower-case magic");
	CHECK(!ppm(text("P6\n# no newline"), image), "a comment running past the data");
}

void test_pfm() {
	RgbaImage image;
	// Little-endian (negative scale) RGB: 0, 0.5, 1 | 2, -1, 0.25.
	std::vector<uint8_t> bytes = text("PF\n2 1\n-1.0\n");
	for (float f : {0.0f, 0.5f, 1.0f, 2.0f, -1.0f, 0.25f}) add_f32_le(bytes, f);
	CHECK(pfm(bytes, image) && image.width == 2 && image.height == 1, "PF 2x1 decodes");
	CHECK(pixel_is(image, 0, 0, 0, 128, 255) && pixel_is(image, 1, 0, 255, 0, 64), "floats through the 8-bit encode");

	// A positive scale is big-endian [orig: @ 0x6DE31E..0x6DE334; MSBConvert @ 0x6DB3C0].
	bytes = text("PF\n1 1\n1.0\n");
	for (float f : {1.0f, 0.5f, 0.0f}) add_f32_be(bytes, f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 255, 128, 0), "positive scale: big-endian");
	// -0 is not below 0: big-endian.
	bytes = text("PF\n1 1\n-0\n");
	for (int i = 0; i < 3; ++i) add_f32_be(bytes, 0.25f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 64, 64, 64), "-0 reads big-endian");
	// The scale's magnitude scales nothing.
	bytes = text("PF\n1 1\n-37.5\n");
	for (int i = 0; i < 3; ++i) add_f32_le(bytes, 0.25f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 64, 64, 64), "the scale's magnitude is ignored");
	// The CRT's %f takes an exponent letter with no digits [orig: CRT_input_l @ 0x77B604..0x77B6AC].
	bytes = text("PF\n1 1\n-1e\n");
	for (int i = 0; i < 3; ++i) add_f32_le(bytes, 0.25f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 64, 64, 64), "\"-1e\" is a scale of -1");

	// Grey, two rows: the file's first row is the image's bottom one.
	bytes = text("Pf\n1 2\n-1\n");
	add_f32_le(bytes, 0.0f);
	add_f32_le(bytes, 1.0f);
	CHECK(pfm(bytes, image) && image.width == 1 && image.height == 2, "Pf 1x2 decodes");
	CHECK(pixel_is(image, 0, 0, 255, 255, 255) && pixel_is(image, 0, 1, 0, 0, 0),
			"rows bottom-up; grey sets red, green and blue");

	// A '\r' before the size line's '\n' is whitespace to sscanf; extra bytes are fine.
	bytes = text("PF\n1 1\r\n-1\n");
	for (float f : {0.5f, 0.5f, 0.5f, 9.0f}) add_f32_le(bytes, f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 128, 128, 128), "CR in the size line, trailing bytes");

	// Out-of-range floats: +inf, NaN and a huge value are 0, a negative 0.
	bytes = text("PF\n1 1\n-1\n");
	add_f32_le(bytes, std::numeric_limits<float>::infinity());
	add_f32_le(bytes, std::numeric_limits<float>::quiet_NaN());
	add_f32_le(bytes, 1e10f);
	CHECK(pfm(bytes, image) && pixel_is(image, 0, 0, 0, 0, 0), "inf, NaN, 1e10 encode to 0");

	std::vector<uint8_t> body;
	for (int i = 0; i < 3; ++i) add_f32_le(body, 0.5f);
	const auto with_body = [&](const std::string &header) {
		std::vector<uint8_t> b = text(header);
		b.insert(b.end(), body.begin(), body.end());
		return b;
	};
	CHECK(!pfm(with_body("PF 1 1\n-1\n"), image), "the magic needs its newline");
	CHECK(!pfm(with_body("PF\r\n1 1\n-1\n"), image), "a CR after the magic fails");
	CHECK(!pfm(with_body("pf\n1 1\n-1\n"), image), "lower-case magic");
	CHECK(!pfm(with_body("PF\n1\n-1\n"), image), "one size number");
	CHECK(!pfm(with_body("PF\n1 1 x\n-1\n"), image), "more after the sizes");
	CHECK(!pfm(with_body("PF\n\n1 1\n-1\n"), image), "an empty size line");
	CHECK(!pfm(with_body("PF\n1 1\nabc\n"), image), "a scale that is no number");
	CHECK(!pfm(with_body("PF\n1 1\n-1 x\n"), image), "more after the scale");
	CHECK(!pfm(with_body("PF\n0 1\n-1\n"), image), "a zero side");
	CHECK(!pfm(with_body("PF\n2 1\n-1\n"), image), "the pixel data is short");
	CHECK(!pfm(with_body("PF\n" + std::string(300, ' ') + "1 1\n-1\n"), image),
			"a size line past 256 bytes has no newline in reach");
	CHECK(!pfm(text("PF\n"), image), "under four bytes");
}

// "#?RADIANCE", a FORMAT line, `extra` header lines, the blank line, the resolution line.
std::vector<uint8_t> hdr_head(const std::string &resolution, const std::string &extra = "") {
	return text("#?RADIANCE\n# made by hand\nFORMAT=32-bit_rle_rgbe\n" + extra + "\n" + resolution + "\n");
}

// A new run-length scanline's opening word for `length` pixels.
void rle_word(std::vector<uint8_t> &bytes, int length) {
	add(bytes, {2, 2, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)});
}

// The four channels of H1's scanline: R run 127 x4; G literal 0, 63, 127, 255;
// B run 255 x2, literal 0, 0; E run 128 x4 (E 128 maps a byte to itself).
void h1_channels(std::vector<uint8_t> &bytes) {
	add(bytes, {0x84, 127});
	add(bytes, {4, 0, 63, 127, 255});
	add(bytes, {0x82, 255, 2, 0, 0});
	add(bytes, {0x84, 128});
}

void test_hdr_new_rle() {
	RgbaImage image;
	std::vector<uint8_t> bytes = hdr_head("-Y 1 +X 4");
	rle_word(bytes, 4);
	h1_channels(bytes);
	CHECK(hdr(bytes, image) && image.width == 4 && image.height == 1, "new RLE decodes");
	CHECK(pixel_is(image, 0, 0, 127, 0, 255) && pixel_is(image, 1, 0, 127, 63, 255) &&
					pixel_is(image, 2, 0, 127, 127, 0) && pixel_is(image, 3, 0, 127, 255, 0),
			"runs and literals per channel; (b + 0.5) * 2^(E-136) through the 8-bit encode");

	// The second header form: byte 1 is 2 and byte 2 under 0x80, whatever byte 0 is
	// [orig: @ 0x6DEF17..0x6DEF20].
	bytes = hdr_head("-Y 1 +X 4");
	add(bytes, {5, 2, 0, 4});
	h1_channels(bytes);
	CHECK(hdr(bytes, image) && pixel_is(image, 1, 0, 127, 63, 255), "a (x, 2, <0x80, n) word opens new RLE");

	// EXPOSURE lines multiply: 2 * 0.5 is 1, so nothing changes; 0.5 alone doubles.
	bytes = hdr_head("-Y 1 +X 4", "EXPOSURE=2.0\nEXPOSURE= 0.5\n");
	rle_word(bytes, 4);
	h1_channels(bytes);
	CHECK(hdr(bytes, image) && pixel_is(image, 1, 0, 127, 63, 255), "exposures 2 and 0.5 multiply to 1");
	bytes = hdr_head("-Y 1 +X 4", "EXPOSURE=0.5\n");
	rle_word(bytes, 4);
	h1_channels(bytes);
	CHECK(hdr(bytes, image) && pixel_is(image, 0, 0, 254, 1, 255) && pixel_is(image, 1, 0, 254, 127, 255),
			"exposure 0.5 doubles: (127.5/128)*255 -> 254, (0.5/128)*255 -> 1");

	// XYZE decodes as RGBE; blanks before the FORMAT value.
	std::vector<uint8_t> xyze = text("#?RADIANCE\nFORMAT= \t32-bit_rle_xyze\n\n-Y 1 +X 4\n");
	rle_word(xyze, 4);
	h1_channels(xyze);
	CHECK(hdr(xyze, image) && pixel_is(image, 3, 0, 127, 255, 0), "32-bit_rle_xyze decodes alike");

	// A run past the scanline, a short scanline, a length that is not the width.
	bytes = hdr_head("-Y 1 +X 4");
	rle_word(bytes, 4);
	add(bytes, {0x85, 127});
	CHECK(!hdr(bytes, image), "a run past the scanline fails");
	bytes = hdr_head("-Y 1 +X 4");
	rle_word(bytes, 4);
	add(bytes, {0x84, 127, 4, 0});
	CHECK(!hdr(bytes, image), "a literal run past the data fails");
	bytes = hdr_head("-Y 1 +X 4");
	rle_word(bytes, 5);
	h1_channels(bytes);
	CHECK(!hdr(bytes, image), "a scanline length that is not the width fails");
}

void test_hdr_old_rle() {
	RgbaImage image;
	// Flat: the first word is held, never written; the rest land one pixel early and the
	// last pixel stays unwritten (zero: E 0 is (0.5) * 2^-136, black) [orig: @ 0x6DEF26..0x6DF00A].
	std::vector<uint8_t> bytes = hdr_head("-Y 1 +X 3");
	add(bytes, {10, 20, 30, 128, 40, 50, 60, 128, 70, 80, 90, 128});
	CHECK(hdr(bytes, image) && image.width == 3, "flat decodes");
	CHECK(pixel_is(image, 0, 0, 40, 50, 60) && pixel_is(image, 1, 0, 70, 80, 90) && pixel_is(image, 2, 0, 0, 0, 0),
			"the first pixel dropped, the last unwritten");

	// (1, 1, 1, n) repeats the held pixel n times.
	bytes = hdr_head("-Y 1 +X 4");
	add(bytes, {10, 20, 30, 128, 1, 1, 1, 2, 70, 80, 90, 128});
	CHECK(hdr(bytes, image) && pixel_is(image, 0, 0, 10, 20, 30) && pixel_is(image, 1, 0, 10, 20, 30) &&
					pixel_is(image, 2, 0, 70, 80, 90) && pixel_is(image, 3, 0, 0, 0, 0),
			"an old run repeats the held pixel");

	// Back-to-back runs shift by 8: 1, then 1 << 8.
	bytes = hdr_head("-Y 1 +X 258");
	add(bytes, {10, 20, 30, 128, 1, 1, 1, 1, 1, 1, 1, 1});
	CHECK(hdr(bytes, image) && image.width == 258, "shifted runs decode");
	CHECK(pixel_is(image, 0, 0, 10, 20, 30) && pixel_is(image, 256, 0, 10, 20, 30) &&
					pixel_is(image, 257, 0, 0, 0, 0),
			"1 + 256 copies, the last pixel unwritten");

	// A flat scanline whose first red is 2 opens new RLE, and its length is wrong.
	bytes = hdr_head("-Y 1 +X 3");
	add(bytes, {2, 50, 60, 128, 40, 50, 60, 128, 70, 80, 90, 128});
	CHECK(!hdr(bytes, image), "a first red of 2 reads as a run-length header");
	// An old run past the scanline.
	bytes = hdr_head("-Y 1 +X 3");
	add(bytes, {10, 20, 30, 128, 1, 1, 1, 3});
	CHECK(!hdr(bytes, image), "an old run past the scanline fails");
	// Short data.
	bytes = hdr_head("-Y 1 +X 3");
	add(bytes, {10, 20, 30, 128, 40, 50});
	CHECK(!hdr(bytes, image), "a flat scanline past the data fails");
}

void test_hdr_axes() {
	RgbaImage image;
	// +Y -X: scanlines from the bottom row up, each right to left.
	std::vector<uint8_t> bytes = hdr_head("+Y 2 -X 2");
	for (const std::initializer_list<uint8_t> &reds : {std::initializer_list<uint8_t>{10, 20},
				 std::initializer_list<uint8_t>{30, 40}}) {
		rle_word(bytes, 2);
		bytes.push_back(2);
		bytes.insert(bytes.end(), reds.begin(), reds.end());
		add(bytes, {0x82, 0, 0x82, 0, 0x82, 128});
	}
	CHECK(hdr(bytes, image) && image.width == 2 && image.height == 2, "+Y -X decodes");
	CHECK(pixel_is(image, 1, 1, 10, 0, 0) && pixel_is(image, 0, 1, 20, 0, 0) && pixel_is(image, 1, 0, 30, 0, 0) &&
					pixel_is(image, 0, 0, 40, 0, 0),
			"the first scanline is the bottom row, right to left");

	// +X -Y: X first, so the scanlines are columns, left to right, each top to bottom;
	// the first number is the width.
	bytes = hdr_head("+X 2 -Y 3");
	for (const std::initializer_list<uint8_t> &reds : {std::initializer_list<uint8_t>{1, 2, 3},
				 std::initializer_list<uint8_t>{4, 5, 6}}) {
		rle_word(bytes, 3);
		bytes.push_back(3);
		bytes.insert(bytes.end(), reds.begin(), reds.end());
		add(bytes, {0x83, 0, 0x83, 0, 0x83, 128});
	}
	CHECK(hdr(bytes, image) && image.width == 2 && image.height == 3, "+X -Y decodes as 2 x 3");
	CHECK(pixel_is(image, 0, 0, 1, 0, 0) && pixel_is(image, 0, 2, 3, 0, 0) && pixel_is(image, 1, 0, 4, 0, 0) &&
					pixel_is(image, 1, 2, 6, 0, 0),
			"columns left to right, each top to bottom");
}

void test_hdr_rejects() {
	RgbaImage image;
	std::vector<uint8_t> body;
	rle_word(body, 4);
	h1_channels(body);
	const auto with_body = [&](const std::string &head) {
		std::vector<uint8_t> b = text(head);
		b.insert(b.end(), body.begin(), body.end());
		return b;
	};
	CHECK(hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 4\n"), image), "the control decodes");
	CHECK(!hdr(with_body("#?RGBE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 4\n"), image), "#?RGBE is not taken");
	CHECK(!hdr(with_body("#?RADIANCE\n\n-Y 1 +X 4\n"), image), "no FORMAT line");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgb\n\n-Y 1 +X 4\n"), image), "an unknown FORMAT");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=x\n\n-Y 1 +X 4\n"), image), "a bad EXPOSURE");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\nY 1 +X 4\n"), image), "no first sign");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Z 1 +X 4\n"), image), "no first axis");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 X 4\n"), image), "no second sign");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +Q 4\n"), image), "no second axis");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 0 +X 4\n"), image), "a zero side");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y\n"), image), "a resolution line of two bytes");
	CHECK(!hdr(with_body("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 9 +X 4\n"), image),
			"more scanlines than the data can open");
	CHECK(!hdr(text("#?RADIANCE"), image), "ten bytes");
}

// A 40-byte BITMAPINFOHEADER.
std::vector<uint8_t> info_header(int32_t width, int32_t height, uint16_t bits, uint32_t compression,
		uint32_t colors_used, uint32_t header_size = 40) {
	std::vector<uint8_t> b;
	add_u32(b, header_size);
	add_u32(b, static_cast<uint32_t>(width));
	add_u32(b, static_cast<uint32_t>(height));
	add_u16(b, 1);
	add_u16(b, bits);
	add_u32(b, compression);
	add_u32(b, 0);
	add_u32(b, 0);
	add_u32(b, 0);
	add_u32(b, colors_used);
	add_u32(b, 0);
	return b;
}

bool dib(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &bmp) {
	std::string error;
	return d3dx_dib_to_bmp(bytes.data(), bytes.size(), bmp, error);
}

void test_dib() {
	std::vector<uint8_t> bmp;
	// 24-bit 2x2: the pixels right after the header.
	std::vector<uint8_t> bytes = info_header(2, 2, 24, 0, 0);
	bytes.insert(bytes.end(), 16, 0x11);
	CHECK(dib(bytes, bmp) && bmp.size() == bytes.size() + 14, "24-bit DIB re-headed");
	CHECK(bmp.size() >= 14 && bmp[0] == 'B' && bmp[1] == 'M' && u32_at(bmp, 2) == bytes.size() + 14 &&
					u32_at(bmp, 6) == 0 && u32_at(bmp, 10) == 54,
			"BM, bfSize the whole stream, bfOffBits 14 + 40");
	CHECK(bmp.size() == bytes.size() + 14 && std::equal(bytes.begin(), bytes.end(), bmp.begin() + 14),
			"the DIB follows unchanged");

	// 8-bit: 256 four-byte entries when biClrUsed is 0, else biClrUsed.
	bytes = info_header(2, 2, 8, 0, 0);
	bytes.insert(bytes.end(), 1024 + 8, 0);
	CHECK(dib(bytes, bmp) && u32_at(bmp, 10) == 14 + 40 + 1024, "8-bit, full palette");
	bytes = info_header(2, 2, 8, 0, 2);
	bytes.insert(bytes.end(), 8 + 8, 0);
	CHECK(dib(bytes, bmp) && u32_at(bmp, 10) == 14 + 40 + 8, "8-bit, biClrUsed 2");
	// A 24-bit image's biClrUsed still moves the pixels.
	bytes = info_header(1, 1, 24, 0, 3);
	bytes.insert(bytes.end(), 12 + 4, 0);
	CHECK(dib(bytes, bmp) && u32_at(bmp, 10) == 14 + 40 + 12, "24-bit with a 3-entry palette");

	// BITMAPCOREHEADER: three-byte entries.
	std::vector<uint8_t> core;
	add_u32(core, 12);
	add_u16(core, 2);
	add_u16(core, 2);
	add_u16(core, 1);
	add_u16(core, 8);
	core.insert(core.end(), 768 + 8, 0);
	CHECK(dib(core, bmp) && u32_at(bmp, 10) == 14 + 12 + 768, "core header, 8-bit");
	core[10] = 4;
	core.resize(12 + 48 + 4 + 4);
	CHECK(dib(core, bmp) && u32_at(bmp, 10) == 14 + 12 + 48, "core header, 4-bit");

	// BI_BITFIELDS needs the masks inside a header of 52 bytes or more.
	bytes = info_header(2, 1, 32, 3, 0);
	add_u32(bytes, 0x00FF0000u);
	add_u32(bytes, 0x0000FF00u);
	add_u32(bytes, 0x000000FFu);
	bytes.insert(bytes.end(), 8, 0);
	CHECK(!dib(bytes, bmp), "BI_BITFIELDS after a 40-byte header fails");
	const auto v4 = [&](uint32_t red, uint32_t green, uint32_t blue, size_t pixel_bytes) {
		std::vector<uint8_t> b = info_header(2, 1, 32, 3, 0, 108);
		add_u32(b, red);
		add_u32(b, green);
		add_u32(b, blue);
		add_u32(b, 0);
		b.resize(108, 0);
		b.insert(b.end(), pixel_bytes, 0);
		return b;
	};
	CHECK(dib(v4(0x00FF0000u, 0x0000FF00u, 0x000000FFu, 8), bmp) && u32_at(bmp, 10) == 14 + 108,
			"V4 BGRX masks: the pixels after the header");
	CHECK(!dib(v4(0xFF000000u, 0x00FF0000u, 0x0000FF00u, 8), bmp),
			"RGBX masks read one byte later, so the same data is one byte short");
	CHECK(dib(v4(0xFF000000u, 0x00FF0000u, 0x0000FF00u, 9), bmp) && u32_at(bmp, 10) == 14 + 108 + 1,
			"RGBX masks: bfOffBits one past the header");

	// RLE8 has no up-front data check.
	bytes = info_header(2, 2, 8, 1, 0);
	bytes.insert(bytes.end(), 1024, 0);
	add(bytes, {0, 1});
	CHECK(dib(bytes, bmp) && u32_at(bmp, 10) == 14 + 40 + 1024, "RLE8 re-headed");
	// Top-down (negative height).
	bytes = info_header(1, -2, 24, 0, 0);
	bytes.insert(bytes.end(), 8, 0);
	CHECK(dib(bytes, bmp) && u32_at(bmp, 10) == 54, "a negative height is two rows");

	// What the core fails.
	CHECK(!dib(std::vector<uint8_t>{40, 0, 0}, bmp), "under four bytes");
	bytes = info_header(1, 1, 24, 0, 0, 200);
	bytes.insert(bytes.end(), 4, 0);
	CHECK(!dib(bytes, bmp), "a header past the data");
	bytes = info_header(1, 1, 24, 0, 0, 20);
	bytes.insert(bytes.end(), 4, 0);
	CHECK(!dib(bytes, bmp), "a 20-byte header");
	bytes = info_header(1, 1, 24, 0, 0);
	bytes[12] = 2;
	bytes.insert(bytes.end(), 4, 0);
	CHECK(!dib(bytes, bmp), "two planes");
	bytes = info_header(1, 1, 24, 4, 0);
	bytes.insert(bytes.end(), 4, 0);
	CHECK(!dib(bytes, bmp), "BI_JPEG");
	bytes = info_header(1, 1, 2, 0, 0);
	bytes.insert(bytes.end(), 64, 0);
	CHECK(!dib(bytes, bmp), "2 bits per pixel");
	bytes = info_header(2, 2, 24, 0, 0);
	bytes.insert(bytes.end(), 13, 0);
	CHECK(!dib(bytes, bmp), "the last row short");
	bytes = info_header(1, 1, 8, 0, 300);
	bytes.insert(bytes.end(), 1200 + 4, 0);
	CHECK(!dib(bytes, bmp), "more than 256 palette entries (retail overruns its palette)");
	bytes = info_header(0, 1, 24, 0, 0);
	bytes.insert(bytes.end(), 4, 0);
	CHECK(!dib(bytes, bmp), "zero width");
}

void test_bmp_rehead() {
	std::vector<uint8_t> dib_bytes = info_header(2, 2, 24, 0, 0);
	dib_bytes.insert(dib_bytes.end(), 16, 0x22);
	std::vector<uint8_t> file = text("BM");
	add_u32(file, static_cast<uint32_t>(dib_bytes.size() + 14));
	add_u32(file, 0);
	add_u32(file, 999); // a bfOffBits the codec never reads
	file.insert(file.end(), dib_bytes.begin(), dib_bytes.end());
	std::vector<uint8_t> bmp;
	std::string error;
	CHECK(d3dx_bmp_rehead(file.data(), file.size(), bmp, error) && u32_at(bmp, 10) == 54,
			"the core's offset replaces the file's bfOffBits");
	file[2] = static_cast<uint8_t>(file.size() + 1);
	CHECK(!d3dx_bmp_rehead(file.data(), file.size(), bmp, error), "bfSize past the data fails");
	file[2] = static_cast<uint8_t>(file.size());
	file[1] = 'X';
	CHECK(!d3dx_bmp_rehead(file.data(), file.size(), bmp, error), "no BM");
	CHECK(!d3dx_bmp_rehead(file.data(), 13, bmp, error), "under 14 bytes");
}

} // namespace

int main() {
	test_unorm8();
	test_ppm_binary();
	test_ppm_ascii();
	test_ppm_rejects();
	test_pfm();
	test_hdr_new_rle();
	test_hdr_old_rle();
	test_hdr_axes();
	test_hdr_rejects();
	test_dib();
	test_bmp_rehead();
	if (failures != 0) {
		std::fprintf(stderr, "d3dx_image_codecs: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("d3dx_image_codecs: ok\n");
	return 0;
}
