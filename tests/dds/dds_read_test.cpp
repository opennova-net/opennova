// D3DX's DDS loader as engine/formats/dds dds_read ports it (D3DXTex::CImage::LoadDDS @ 0x6DDA66 and
// its pixel-format table at 0x8564A0) over files minted here: the A8R8G8B8 writer's image read back
// texel for texel; a DXT5 file of three levels from the DXT writer, its levels' sides, offsets and bytes
// (the blocks left to the D3DX codec's port), the mip count read whatever the header's flags say; the
// masked forms the table matches (R5G6B5, X8R8G8B8, L8, A8, A4R4G4B4) decoded; and what the loader
// refuses: a pixel format its table lacks, a partial cube map, data that stops short of a level, a
// palette cut short. A cube map of six faces reads its first face's chain; bytes past the last level
// are counted, never read; no magic, or a short header, is no DDS.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/le.h>
#include <formats/dds/dds.h>

#include "common/test_expect.h"

using namespace opennova::dds;

namespace {

// A header of `bits` a texel under the masks, `flags` its pixel format's, one level.
std::vector<uint8_t> masked_header(uint32_t width, uint32_t height, uint32_t flags, uint32_t bits, uint32_t r, uint32_t g,
                                   uint32_t b, uint32_t a) {
	std::vector<uint8_t> out = {'D', 'D', 'S', ' '};
	opennova::io::append_u32_le(out, 124);
	opennova::io::append_u32_le(out, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT);
	opennova::io::append_u32_le(out, height);
	opennova::io::append_u32_le(out, width);
	opennova::io::append_u32_le(out, 0);
	opennova::io::append_u32_le(out, 0);
	opennova::io::append_u32_le(out, 0);
	for (int i = 0; i < 11; ++i) opennova::io::append_u32_le(out, 0);
	opennova::io::append_u32_le(out, 32);
	opennova::io::append_u32_le(out, flags);
	opennova::io::append_u32_le(out, 0);
	opennova::io::append_u32_le(out, bits);
	opennova::io::append_u32_le(out, r);
	opennova::io::append_u32_le(out, g);
	opennova::io::append_u32_le(out, b);
	opennova::io::append_u32_le(out, a);
	opennova::io::append_u32_le(out, DDSCAPS_TEXTURE);
	for (int i = 0; i < 4; ++i) opennova::io::append_u32_le(out, 0);
	return out;
}

bool texel_is(const DdsLevel &level, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	const uint8_t *t = level.rgba.data() + (size_t(y) * level.width + x) * 4;
	return t[0] == r && t[1] == g && t[2] == b && t[3] == a;
}

} // namespace

int main() {
	std::string error;
	// The A8R8G8B8 writer's image read back.
	std::vector<uint8_t> rgba;
	for (int k = 0; k < 6; ++k)
		for (int c = 0; c < 4; ++c) rgba.push_back(uint8_t(17 * k + c));
	std::vector<uint8_t> file;
	TEST_EXPECT(dds_write_a8r8g8b8(rgba.data(), 3, 2, file, error));
	DdsImage image;
	TEST_EXPECT(dds_read(file.data(), file.size(), image, error) && image.loads && image.refusal.empty());
	TEST_EXPECT(image.format.d3d == 0x15 && std::string(image.format.name) == "A8R8G8B8" && image.format.decoded &&
	            !image.format.compressed && image.format.bits == 32);
	TEST_EXPECT(image.levels.size() == 1 && image.levels[0].width == 3 && image.levels[0].height == 2 &&
	            image.levels[0].rgba == rgba && image.extra_bytes == 0 && image.data_bytes == 24);

	// DXT5 of three levels (4 x 4, 2 x 2, 1 x 1: a block each), the blocks the bytes 0..47.
	std::vector<std::vector<uint8_t>> blocks(3);
	for (size_t level = 0; level < 3; ++level)
		for (size_t i = 0; i < 16; ++i) blocks[level].push_back(uint8_t(level * 16 + i));
	TEST_EXPECT(dds_write_dxt(dds_fourcc('D', 'X', 'T', '5'), 4, 4, blocks, file, error));
	TEST_EXPECT(dds_read(file.data(), file.size(), image, error) && image.loads);
	TEST_EXPECT(std::string(image.format.name) == "DXT5" && image.format.compressed && image.format.block_bytes == 16 &&
	            !image.format.decoded);
	TEST_EXPECT(image.header.mip_count == 3 && (image.header.flags & DDSD_MIPMAPCOUNT) && image.levels.size() == 3);
	TEST_EXPECT(image.levels[1].width == 2 && image.levels[1].height == 2 && image.levels[2].width == 1);
	for (size_t level = 0; level < 3; ++level) {
		const DdsLevel &at = image.levels[level];
		TEST_EXPECT(at.bytes == 16 && at.offset == DDS_HEADER_SIZE + 16 * level && at.rgba.empty());
		TEST_EXPECT(std::vector<uint8_t>(file.begin() + long(at.offset), file.begin() + long(at.offset + at.bytes)) ==
		            blocks[level]);
	}
	// The mip count read whatever the flags say (D3DX reads the field alone).
	std::vector<uint8_t> unflagged = file;
	unflagged[8] &= uint8_t(~(DDSD_MIPMAPCOUNT & 0xFF));
	unflagged[10] &= uint8_t(~((DDSD_MIPMAPCOUNT >> 16) & 0xFF));
	TEST_EXPECT(dds_read(unflagged.data(), unflagged.size(), image, error) && image.levels.size() == 3);
	// Short of its last level: refused.
	TEST_EXPECT(dds_read(file.data(), file.size() - 1, image, error) && !image.loads && image.levels.empty() &&
	            image.refusal.find("level 2") != std::string::npos);
	// Bytes past the last level: counted, never read.
	std::vector<uint8_t> longer = file;
	longer.insert(longer.end(), 5, 0xEE);
	TEST_EXPECT(dds_read(longer.data(), longer.size(), image, error) && image.loads && image.extra_bytes == 5);

	// R5G6B5: pure red, green and blue widened to 8 bits.
	std::vector<uint8_t> r565 = masked_header(3, 1, DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0);
	for (uint16_t texel : {uint16_t(0xF800), uint16_t(0x07E0), uint16_t(0x001F)}) opennova::io::append_u16_le(r565, texel);
	TEST_EXPECT(dds_read(r565.data(), r565.size(), image, error) && image.loads && image.format.d3d == 0x17 &&
	            image.format.decoded);
	TEST_EXPECT(texel_is(image.levels[0], 0, 0, 255, 0, 0, 255) && texel_is(image.levels[0], 1, 0, 0, 255, 0, 255) &&
	            texel_is(image.levels[0], 2, 0, 0, 0, 255, 255));
	// X8R8G8B8: the top byte is no alpha.
	std::vector<uint8_t> x888 = masked_header(1, 1, DDPF_RGB, 32, 0xFF0000, 0xFF00, 0xFF, 0);
	opennova::io::append_u32_le(x888, 0x12345678u);
	TEST_EXPECT(dds_read(x888.data(), x888.size(), image, error) && image.format.d3d == 0x16 &&
	            texel_is(image.levels[0], 0, 0, 0x34, 0x56, 0x78, 255));
	// L8 as grey, A8 as alpha over black, A4R4G4B4 widened.
	std::vector<uint8_t> l8 = masked_header(1, 1, DDPF_LUMINANCE, 8, 0xFF, 0, 0, 0);
	l8.push_back(77);
	TEST_EXPECT(dds_read(l8.data(), l8.size(), image, error) && image.format.d3d == 0x32 &&
	            texel_is(image.levels[0], 0, 0, 77, 77, 77, 255));
	std::vector<uint8_t> a8 = masked_header(1, 1, DDPF_ALPHA, 8, 0, 0, 0, 0xFF);
	a8.push_back(99);
	TEST_EXPECT(dds_read(a8.data(), a8.size(), image, error) && image.format.d3d == 0x1C &&
	            texel_is(image.levels[0], 0, 0, 0, 0, 0, 99));
	std::vector<uint8_t> a4 = masked_header(1, 1, DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000);
	opennova::io::append_u16_le(a4, 0x8F31);
	TEST_EXPECT(dds_read(a4.data(), a4.size(), image, error) && image.format.d3d == 0x1A &&
	            texel_is(image.levels[0], 0, 0, 0xFF, 0x33, 0x11, 0x88));

	// A pixel format the table lacks: refused (a 16-bit RGB under masks no row has).
	std::vector<uint8_t> odd = masked_header(1, 1, DDPF_RGB, 16, 0x00FF, 0xFF00, 0, 0);
	opennova::io::append_u16_le(odd, 0);
	TEST_EXPECT(dds_read(odd.data(), odd.size(), image, error) && !image.loads && image.format.d3d == 0 &&
	            !image.refusal.empty());
	// A cube map of six faces reads its first face's chain; one of fewer faces is refused.
	std::vector<uint8_t> cube = masked_header(1, 1, DDPF_LUMINANCE, 8, 0xFF, 0, 0, 0);
	opennova::io::append_u32_le(cube, 0); // overwritten below: caps2 is at offset 112
	cube.resize(DDS_HEADER_SIZE);
	cube[112] = 0x00;
	cube[113] = 0xFC; // DDSCAPS2_CUBEMAP and its six faces
	for (uint8_t face = 0; face < 6; ++face) cube.push_back(uint8_t(10 + face));
	TEST_EXPECT(dds_read(cube.data(), cube.size(), image, error) && image.loads && image.faces == 6 &&
	            image.levels.size() == 1 && texel_is(image.levels[0], 0, 0, 10, 10, 10, 255));
	cube[113] = 0x7C; // five faces
	TEST_EXPECT(dds_read(cube.data(), cube.size(), image, error) && !image.loads && image.refusal.find("cube") != std::string::npos);
	// A P8's palette cut short: refused.
	std::vector<uint8_t> p8 = masked_header(1, 1, DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0);
	p8.resize(p8.size() + 100, 0);
	TEST_EXPECT(dds_read(p8.data(), p8.size(), image, error) && !image.loads && image.refusal.find("palette") != std::string::npos);

	// No DDS at all.
	TEST_EXPECT(!dds_read(rgba.data(), rgba.size(), image, error));
	TEST_EXPECT(!dds_read(file.data(), DDS_HEADER_SIZE - 1, image, error));
	// The DXT writer refuses another FourCC and levels that are not their blocks.
	TEST_EXPECT(!dds_write_dxt(dds_fourcc('A', 'B', 'C', 'D'), 4, 4, blocks, file, error));
	std::vector<std::vector<uint8_t>> wrong = blocks;
	wrong[1].pop_back();
	TEST_EXPECT(!dds_write_dxt(dds_fourcc('D', 'X', 'T', '5'), 4, 4, wrong, file, error));
	std::printf("dds_read: the A8R8G8B8 writer's image, a DXT5 chain's levels, the masked forms decoded, and what D3DX "
	            "refuses\n");
	return 0;
}
