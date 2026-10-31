// menu_texture_header: a menu texture's size read from its header by the format the game's
// loader picks, no pixels decoded (the TGA and DDS headers, a PNG's IHDR, a PCX through the port
// of the game's menu decoder), and the decoder that keeps none. Each file is minted by the
// engine's own writers.

#include <runtime/menu/menu_texture_header.h>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/device_texture.h>

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using opennova::menu::MenuTextureFormat;
using opennova::menu::menu_texture_header_size;

namespace {

std::vector<uint8_t> rgba(uint32_t width, uint32_t height) {
	return std::vector<uint8_t>(size_t(width) * height * 4, 0x80);
}

bool sized(MenuTextureFormat format, const std::vector<uint8_t> &bytes, int width, int height) {
	int w = -1, h = -1;
	return menu_texture_header_size(format, bytes, &w, &h) && w == width && h == height;
}

} // namespace

static int test_each_format() {
	std::string error;
	std::vector<uint8_t> tga;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba(5, 3).data(), 5, 3, tga, error));
	TEST_EXPECT(sized(MenuTextureFormat::Tga, tga, 5, 3));
	std::vector<uint8_t> dds;
	TEST_EXPECT(opennova::dds::dds_write_a8r8g8b8(rgba(7, 2).data(), 7, 2, dds, error));
	TEST_EXPECT(sized(MenuTextureFormat::Dds, dds, 7, 2));
	const std::vector<uint8_t> png = opennova::png::encode_png_rgba(rgba(4, 9).data(), 4, 9);
	TEST_EXPECT(sized(MenuTextureFormat::Png, png, 4, 9));
	opennova::IndexedImage8 indexed;
	indexed.width = 6;
	indexed.height = 4;
	indexed.indices.assign(24, 1);
	std::vector<uint8_t> pcx;
	TEST_EXPECT(opennova::encode_pcx_indexed(indexed, pcx, error));
	TEST_EXPECT(sized(MenuTextureFormat::Pcx, pcx, 6, 4));
	// The format the loader picked decides: a TGA read as a DDS holds no size.
	TEST_EXPECT(!sized(MenuTextureFormat::Dds, tga, 5, 3));
	TEST_EXPECT(!sized(MenuTextureFormat::None, tga, 5, 3));
	std::printf("test_each_format passed\n");
	return 0;
}

static int test_refusals() {
	// A PNG's size is its IHDR's, after the signature: a broken signature states none.
	std::vector<uint8_t> png = opennova::png::encode_png_rgba(rgba(4, 9).data(), 4, 9);
	png[1] = 'Q';
	TEST_EXPECT(!sized(MenuTextureFormat::Png, png, 4, 9));
	// A header that states a side of 0 holds no texture.
	std::string error;
	std::vector<uint8_t> tga;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba(5, 3).data(), 5, 3, tga, error));
	tga[12] = 0;
	tga[13] = 0;
	TEST_EXPECT(!sized(MenuTextureFormat::Tga, tga, 0, 3));
	// Too short for its header.
	TEST_EXPECT(!sized(MenuTextureFormat::Tga, std::vector<uint8_t>(10, 0), 0, 0));
	TEST_EXPECT(!sized(MenuTextureFormat::Dds, std::vector<uint8_t>{'D', 'D', 'S', ' '}, 0, 0));
	std::printf("test_refusals passed\n");
	return 0;
}

static int test_probe_keeps_no_pixels() {
	std::string error;
	std::vector<uint8_t> tga;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba(5, 3).data(), 5, 3, tga, error));
	opennova::menu::MenuTextureHeaderProbe probe;
	int w = 0, h = 0;
	TEST_EXPECT(probe.decode("a.tga#1", MenuTextureFormat::Tga, tga, w, h) && w == 5 && h == 3);
	TEST_EXPECT(!probe.decode("b.tga#1", MenuTextureFormat::Tga, {1, 2, 3}, w, h));
	probe.release("a.tga#1");
	std::printf("test_probe_keeps_no_pixels passed\n");
	return 0;
}

// A menu DDS is read back at the sides its header states, out of the texture D3DX made at
// D3DX_DEFAULT sides: a 1600 x 1200 background (dfx2.exe's JO_epil) at 1600 x 1200, not the
// 2048 x 2048 texture; every other format's reader decodes at its own sides, so none is cropped.
// [orig: TextureSlot_LoadFromDXTFile @0x6642E0 - @0x6643A0; GTexture_InitFromMemory - the
//  D3DXIMAGE_INFO sides @0x687F09 / @0x687F0C; GTexture_ReadMipPixels @0x6879D5 / @0x6879D7]
static int test_readback_size() {
	std::string error;
	std::vector<uint8_t> dds;
	TEST_EXPECT(opennova::dds::dds_write_a8r8g8b8(rgba(1600, 1200).data(), 1600, 1200, dds, error));
	// The texture D3DX makes of it is larger.
	TEST_EXPECT(opennova::renderer::d3dx_default_texture_side(1600) == 2048 &&
			opennova::renderer::d3dx_default_texture_side(1200) == 2048);
	int w = -1, h = -1;
	TEST_EXPECT(opennova::menu::menu_image_readback_size(MenuTextureFormat::Dds, dds, &w, &h) &&
			w == 1600 && h == 1200);
	std::vector<uint8_t> tga;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(rgba(5, 3).data(), 5, 3, tga, error));
	TEST_EXPECT(!opennova::menu::menu_image_readback_size(MenuTextureFormat::Tga, tga, &w, &h));
	const std::vector<uint8_t> png = opennova::png::encode_png_rgba(rgba(4, 9).data(), 4, 9);
	TEST_EXPECT(!opennova::menu::menu_image_readback_size(MenuTextureFormat::Png, png, &w, &h));
	// A DDS too short for its header states no sides.
	TEST_EXPECT(!opennova::menu::menu_image_readback_size(
			MenuTextureFormat::Dds, std::vector<uint8_t>{'D', 'D', 'S', ' '}, &w, &h));
	std::printf("test_readback_size passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_each_format();
	failures += test_refusals();
	failures += test_probe_keeps_no_pixels();
	failures += test_readback_size();
	return failures == 0 ? 0 : 1;
}
