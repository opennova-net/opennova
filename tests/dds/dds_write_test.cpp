// The DDS writer (engine/formats/dds) over a 3 by 2 image of six distinct pixels: the magic
// and every field of the 124-byte header (the flags, the size, the row pitch, no mip levels,
// the A8R8G8B8 pixel format, the texture caps), the pixels B, G, R, A from the top row down,
// each one read back through the pixel format's masks as D3DX reads the file, and what the
// writer refuses (no pixels). The header's size read back (dds_header_size), and what it
// refuses (no magic, a header too short to state the sides).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <formats/dds/dds.h>

#include "common/test_expect.h"

using namespace opennova::dds;

namespace {

uint32_t le32(const std::vector<uint8_t> &b, size_t at) {
	return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

} // namespace

int main() {
	// Pixel k (the top row 0, 1, 2, the bottom row 3, 4, 5) is R, G, B, A = 10k .. 10k + 3.
	std::vector<uint8_t> rgba;
	for (int k = 0; k < 6; ++k)
		for (int c = 0; c < 4; ++c) rgba.push_back(uint8_t(10 * k + c));
	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(dds_write_a8r8g8b8(rgba.data(), 3, 2, out, error));
	TEST_EXPECT(out.size() == DDS_HEADER_SIZE + 6 * 4);
	TEST_EXPECT(std::memcmp(out.data(), "DDS ", 4) == 0);
	TEST_EXPECT(le32(out, 4) == 124);                // the header's size
	TEST_EXPECT(le32(out, 8) == 0x100Fu);            // caps, height, width, pitch, pixel format
	TEST_EXPECT(le32(out, 12) == 2 && le32(out, 16) == 3); // height, width
	TEST_EXPECT(le32(out, 20) == 12);                // the row pitch
	TEST_EXPECT(le32(out, 24) == 0 && le32(out, 28) == 0); // depth, mip levels
	for (size_t at = 32; at < 76; at += 4) TEST_EXPECT(le32(out, at) == 0); // reserved
	TEST_EXPECT(le32(out, 76) == 32 && le32(out, 80) == 0x41u && le32(out, 84) == 0); // RGB with alpha, no FourCC
	TEST_EXPECT(le32(out, 88) == 32);
	TEST_EXPECT(le32(out, 92) == 0x00FF0000u && le32(out, 96) == 0x0000FF00u && le32(out, 100) == 0x000000FFu &&
	            le32(out, 104) == 0xFF000000u);
	TEST_EXPECT(le32(out, 108) == 0x1000u);          // a texture
	for (size_t at = 112; at < DDS_HEADER_SIZE; at += 4) TEST_EXPECT(le32(out, at) == 0);
	const std::vector<uint8_t> pixels = {2,  1,  0,  3,  12, 11, 10, 13, 22, 21, 20, 23,
	                                     32, 31, 30, 33, 42, 41, 40, 43, 52, 51, 50, 53};
	TEST_EXPECT(std::vector<uint8_t>(out.begin() + DDS_HEADER_SIZE, out.end()) == pixels);
	// Each pixel's dword through the masks: its own R, G, B and A.
	for (size_t i = 0; i < 6; ++i) {
		const uint32_t dword = le32(out, DDS_HEADER_SIZE + i * 4);
		TEST_EXPECT((dword & 0x00FF0000u) >> 16 == rgba[i * 4] && (dword & 0x0000FF00u) >> 8 == rgba[i * 4 + 1] &&
		            (dword & 0x000000FFu) == rgba[i * 4 + 2] && dword >> 24 == rgba[i * 4 + 3]);
	}
	TEST_EXPECT(!dds_write_a8r8g8b8(rgba.data(), 3, 0, out, error) && out.empty() && !error.empty());
	TEST_EXPECT(!dds_write_a8r8g8b8(nullptr, 3, 2, out, error));

	// The header's size.
	TEST_EXPECT(dds_write_a8r8g8b8(rgba.data(), 3, 2, out, error));
	uint32_t w = 0, h = 0;
	TEST_EXPECT(dds_header_size(out.data(), out.size(), w, h) && w == 3 && h == 2);
	TEST_EXPECT(dds_header_size(out.data(), 20, w, h) && w == 3 && h == 2);
	TEST_EXPECT(!dds_header_size(out.data(), 19, w, h) && !dds_header_size(nullptr, 20, w, h));
	std::vector<uint8_t> other = out;
	other[3] = 'X';
	TEST_EXPECT(!dds_header_size(other.data(), other.size(), w, h));
	std::printf("dds_write: the header, the A8R8G8B8 pixels from the top row down; the header's size\n");
	return 0;
}
