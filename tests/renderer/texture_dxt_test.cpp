// Retail's DXT texture path: the format the texture creator picks, the level
// count it asks for, the D3DX9 block coders and the level chain. Every byte
// vector below was derived by executing the codec's semantics as transcribed
// from the binary, operation for operation in single precision (a second,
// independent transcription agrees with this one on 3000 random blocks).
// [orig: GTexture_CreateFromPixelData_0 @ 0x687717..0x687801;
//  D3DXTex_D3DXEncodeDXT1 @ 0x72170F; D3DXTex_EncodeDXT1Block @ 0x720AB7;
//  D3DXTex_OptimizeRGB @ 0x720537; D3DXTex_EncodeDXT5Block
//  @ 0x721962; D3DXTex_OptimizeAlpha @ 0x720210; D3DXTex_D3DXDecodeDXT1
//  @ 0x72140A; D3DXTex_D3DXDecodeDXT5 @ 0x7215D1; D3DXFilterTexture
//  @ 0x6910C9]

#include <runtime/renderer/texture_dxt.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::renderer;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

DxtBlockColors block_from_rgba(const std::vector<uint8_t> &rgba) {
	const std::vector<DxtColor> colors = decode_rgba8(rgba.data(), 4, 4);
	DxtBlockColors block{};
	for (size_t i = 0; i < 16; ++i) block[i] = colors[i];
	return block;
}

template <size_t N>
bool bytes_equal(const uint8_t *actual, const std::array<uint8_t, N> &expected) {
	return std::memcmp(actual, expected.data(), N) == 0;
}

uint32_t bits(float value) {
	uint32_t out;
	std::memcpy(&out, &value, sizeof(out));
	return out;
}

std::vector<uint8_t> solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	std::vector<uint8_t> rgba;
	for (int i = 0; i < 16; ++i) rgba.insert(rgba.end(), {r, g, b, a});
	return rgba;
}

void test_format_selection() {
	// The .til atlas (0x100203) asks for DXT5 and has no DXT1 fallback.
	CHECK(select_texture_dxt_format(0x100203u, kReferenceTextureDxtCaps) ==
			TextureDxtFormat::Dxt5, "the tile-set atlas is DXT5");
	// The detail layers (0x400208) fall back to DXT1 unless the adapter is on
	// the NVIDIA list that keeps DXT5.
	CHECK(select_texture_dxt_format(0x400208u, kReferenceTextureDxtCaps) ==
			TextureDxtFormat::Dxt1, "detail layers are DXT1 on the reference adapter");
	TextureDxtCaps keep{};
	keep.keep_dxt5 = true;
	CHECK(select_texture_dxt_format(0x400208u, keep) == TextureDxtFormat::Dxt5,
			"the NVIDIA quirk keeps DXT5");
	CHECK(select_texture_dxt_format(0x400108u, keep) == TextureDxtFormat::Dxt1,
			"without 0x200 the 0x400000 textures are DXT1 on every adapter");
	// The colormap quadrants at full texture quality (0x100001) stay raw.
	CHECK(select_texture_dxt_format(0x100001u, kReferenceTextureDxtCaps) ==
			TextureDxtFormat::None, "the colormap is not compressed");
	TextureDxtCaps none{};
	none.dxt1 = false;
	none.dxt5 = false;
	CHECK(select_texture_dxt_format(0x100203u, none) == TextureDxtFormat::None,
			"an adapter without DXT keeps the source format");
}

void test_level_count() {
	CHECK(texture_level_count(512, 512, 0x100203u) == 8, "512 square stops at 4x4");
	CHECK(texture_level_count(512, 256, 0x100203u) == 7, "the smaller side decides");
	CHECK(texture_level_count(4, 4, 0) == 1, "a 4x4 texture keeps one level");
	CHECK(texture_level_count(256, 256, 0x40000u) == 1, "0x40000 keeps one level");
	CHECK(texture_level_count(256, 256, 0x80000u) == 3, "0x80000 keeps three");
	CHECK(texture_level_count(2, 2, 0) == 2, "0 is D3DX's full chain to 1x1");
}

void test_dxt1_blocks() {
	uint8_t block[8];
	// A solid colour: both endpoints the same 5:6:5 word, every index 0.
	encode_dxt1_block(block_from_rgba(solid(200, 100, 50, 255)), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 8>{
			0x26, 0xC3, 0x26, 0xC3, 0x00, 0x00, 0x00, 0x00}), "solid DXT1 block");

	// A two-axis ramp runs the endpoint search and the four-colour palette.
	std::vector<uint8_t> ramp;
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			ramp.insert(ramp.end(), {static_cast<uint8_t>(40 + 60 * x),
					static_cast<uint8_t>(30 + 50 * y),
					static_cast<uint8_t>(90 + 10 * ((x + y) & 1)), 255});
		}
	}
	encode_dxt1_block(block_from_rgba(ramp), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 8>{
			0x8C, 0x85, 0xEB, 0x80, 0x55, 0xFF, 0xAA, 0x00}), "ramp DXT1 block");
	DxtBlockColors decoded;
	decode_dxt1_block(block, decoded);
	CHECK(bits(decoded[0].r) == 0x3F042108u && bits(decoded[0].g) == 0x3DE38E3Au &&
			bits(decoded[0].b) == 0x3EB5AD6Bu && bits(decoded[0].a) == 0x3F800000u,
			"texel 0 decodes to endpoint 0");
	CHECK(bits(decoded[15].r) == 0x3F042108u && bits(decoded[15].g) == 0x3F32CB2Du &&
			bits(decoded[15].b) == 0x3EC6318Cu, "texel 15 decodes to endpoint 1");

	// Alpha below 0.5 selects the three-colour block's transparent index 3.
	std::vector<uint8_t> keyed;
	for (int i = 0; i < 16; ++i) {
		keyed.insert(keyed.end(), {180, static_cast<uint8_t>(60 + 10 * i), 20,
				static_cast<uint8_t>(i % 3 == 0 ? 0 : 255)});
	}
	encode_dxt1_block(block_from_rgba(keyed), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 8>{
			0x82, 0xB2, 0xE2, 0xB5, 0xC3, 0xB8, 0x6E, 0xD7}), "colorkeyed DXT1 block");
	decode_dxt1_block(block, decoded);
	CHECK(decoded[0].a == 0.0f && decoded[3].a == 0.0f && decoded[1].a == 1.0f,
			"keyed texels decode transparent");

	// Every texel keyed: the fixed transparent block.
	encode_dxt1_block(block_from_rgba(solid(10, 20, 30, 0)), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 8>{
			0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "all-transparent DXT1 block");
}

void test_dxt5_blocks() {
	uint8_t block[16];
	// Opaque: alpha endpoints 0xFF, zero indices, colour as DXT1.
	encode_dxt5_block(block_from_rgba(solid(200, 100, 50, 255)), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 16>{
			0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x26, 0xC3, 0x26, 0xC3, 0x00, 0x00, 0x00, 0x00}), "opaque DXT5 block");

	// No 0 and no 255: the eight-alpha palette. Its endpoint search weighs
	// the top step by the table's 8/7 (a table ending at 1 would give
	// C9 25 F9 5D 72 12 00 00).
	static const uint8_t kEightAlphas[16] = {
			40, 50, 60, 80, 100, 120, 140, 160, 175, 185, 190, 195, 198, 199, 200, 200};
	std::vector<uint8_t> eight;
	for (uint8_t alpha : kEightAlphas) eight.insert(eight.end(), {120, 140, 160, alpha});
	encode_dxt5_block(block_from_rgba(eight), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 16>{
			0xD9, 0x21, 0xF9, 0xDD, 0x72, 0x93, 0x24, 0x49,
			0x73, 0x7C, 0x73, 0x7C, 0x00, 0x00, 0x00, 0x00}), "eight-alpha DXT5 block");

	// 0 and 255 present: the six-alpha palette with its explicit 0 and 1.
	static const uint8_t kSixAlphas[16] = {
			0, 0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 255, 255};
	std::vector<uint8_t> six;
	for (uint8_t alpha : kSixAlphas) six.insert(six.end(), {30, 200, 90, alpha});
	encode_dxt5_block(block_from_rgba(six), false, block);
	CHECK(bytes_equal(block, std::array<uint8_t, 16>{
			0x0E, 0xF6, 0x36, 0x24, 0x6D, 0x64, 0xDB, 0xFC,
			0x2B, 0x26, 0x2B, 0x26, 0x00, 0x00, 0x00, 0x00}), "six-alpha DXT5 block");
	DxtBlockColors decoded;
	decode_dxt5_block(block, decoded);
	CHECK(decoded[0].a == 0.0f && decoded[15].a == 1.0f,
			"the six-alpha palette's explicit 0 and 1");
}

void test_level_chain() {
	// 8x8 through D3DXLoadSurfaceFromMemory then one D3DXFilterTexture level,
	// the second built from the first level's decoded blocks.
	std::vector<uint8_t> rgba;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			rgba.push_back(static_cast<uint8_t>((x * 37 + y * 11) & 255));
			rgba.push_back(static_cast<uint8_t>((x * 13 + y * 29 + 40) & 255));
			rgba.push_back(static_cast<uint8_t>((200 - x * 9 - y * 7) & 255));
			rgba.push_back(static_cast<uint8_t>((60 + x * 20 + y * 3) & 255));
		}
	}
	const std::vector<DxtSurface> levels =
			build_dxt_texture_levels(rgba.data(), 8, 8, TextureDxtFormat::Dxt5, 2);
	CHECK(levels.size() == 2 && levels[0].is_valid() && levels[1].is_valid(),
			"two DXT5 levels");
	if (levels.size() != 2) return;
	static const std::array<uint8_t, 64> kLevel0 = {
			0x89, 0x3C, 0x31, 0x17, 0x53, 0x37, 0xF5, 0x52, 0x13, 0x7D, 0x78, 0x11, 0xF5, 0xBF, 0xAB, 0x0A,
			0xDE, 0x8D, 0x71, 0x17, 0x73, 0x31, 0x75, 0x53, 0x73, 0xC3, 0xEE, 0x5E, 0x80, 0xE8, 0xFA, 0x7F,
			0x93, 0x48, 0x31, 0x15, 0x53, 0x37, 0xF5, 0x4E, 0xB1, 0x80, 0x72, 0x6F, 0xFF, 0x5F, 0x55, 0x01,
			0xE8, 0x98, 0x31, 0x17, 0x53, 0x37, 0xF5, 0x52, 0x0F, 0x97, 0x2D, 0x60, 0x00, 0x50, 0xD5, 0xFF};
	static const std::array<uint8_t, 16> kLevel1 = {
			0xE8, 0x47, 0x31, 0x17, 0x53, 0x37, 0x75, 0x53, 0xF1, 0x8E, 0xF2, 0x59, 0xBD, 0xAB, 0xC2, 0x5E};
	CHECK(levels[0].blocks.size() == kLevel0.size() &&
			bytes_equal(levels[0].blocks.data(), kLevel0), "level 0 blocks");
	CHECK(levels[1].blocks.size() == kLevel1.size() &&
			bytes_equal(levels[1].blocks.data(), kLevel1), "level 1 blocks");
	// The composer's bytes: D3DX's A8R8G8B8 rounding of the decoded level.
	static const std::array<uint8_t, 64> kLevel1Rgba = {
			0x5A, 0x3D, 0x94, 0x47, 0x6B, 0x73, 0x91, 0x75, 0x6B, 0x73, 0x91, 0xA3, 0x7B, 0xA9, 0x8F, 0xBA,
			0x6B, 0x73, 0x91, 0x47, 0x7B, 0xA9, 0x8F, 0x75, 0x7B, 0xA9, 0x8F, 0xA3, 0x7B, 0xA9, 0x8F, 0xD1,
			0x7B, 0xA9, 0x8F, 0x5E, 0x8C, 0xDF, 0x8C, 0x75, 0x8C, 0xDF, 0x8C, 0xA3, 0x6B, 0x73, 0x91, 0xD1,
			0x7B, 0xA9, 0x8F, 0x5E, 0x6B, 0x73, 0x91, 0x75, 0x5A, 0x3D, 0x94, 0xA3, 0x5A, 0x3D, 0x94, 0xD1};
	const std::vector<uint8_t> level1_rgba = encode_rgba8(decode_dxt_surface(levels[1]));
	CHECK(level1_rgba.size() == kLevel1Rgba.size() &&
			bytes_equal(level1_rgba.data(), kLevel1Rgba), "level 1 decoded bytes");
}

void test_edge_blocks_repeat_texels() {
	// A 2x2 surface fills its block as [a b a b / c d c d / a b a b / c d c d].
	// [orig: D3DXTex::CCodecDXT::Encode @ 0x6EDE37..0x6EDEC6]
	const std::vector<uint8_t> rgba = {
			255, 0, 0, 255, 0, 255, 0, 128,
			0, 0, 255, 64, 255, 255, 0, 0};
	const DxtSurface surface = encode_dxt_surface(
			decode_rgba8(rgba.data(), 2, 2), 2, 2, TextureDxtFormat::Dxt5);
	const std::vector<DxtColor> texels = decode_rgba8(rgba.data(), 2, 2);
	DxtBlockColors expanded{};
	static constexpr int kSource[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
	for (int i = 0; i < 16; ++i) expanded[i] = texels[kSource[i]];
	uint8_t block[16];
	encode_dxt5_block(expanded, false, block);
	CHECK(surface.blocks.size() == 16 && std::memcmp(surface.blocks.data(), block, 16) == 0,
			"a partial block repeats the edge texels");
	const std::vector<DxtColor> decoded = decode_dxt_surface(surface);
	CHECK(decoded.size() == 4, "the decoded surface keeps its own size");
}

void test_box_filter() {
	// ((p01 + p00) + p10) + p11, times 0.25.
	std::vector<DxtColor> colors(4);
	colors[0] = {0.1f, 0.2f, 0.3f, 0.4f};
	colors[1] = {0.5f, 0.6f, 0.7f, 0.8f};
	colors[2] = {0.9f, 0.0f, 0.25f, 1.0f};
	colors[3] = {0.3f, 0.3f, 0.3f, 0.3f};
	const std::vector<DxtColor> half = box_filter_half(colors, 2, 2);
	CHECK(half.size() == 1, "2x2 halves to 1x1");
	const float r = (((colors[1].r + colors[0].r) + colors[2].r) + colors[3].r) * 0.25f;
	CHECK(half.size() == 1 && bits(half[0].r) == bits(r), "the box sum order");
}

// A DDS's levels as D3DX reads them: a DXT1's and a DXT5's blocks decoded through the codec, a level count
// leaving the rest coded, a form dds_read decodes itself left as it read it.
void test_dds_levels() {
	std::vector<uint8_t> rgba(8 * 8 * 4);
	for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<uint8_t>(i * 7);
	for (const TextureDxtFormat format : {TextureDxtFormat::Dxt1, TextureDxtFormat::Dxt5}) {
		const std::vector<DxtSurface> surfaces = build_dxt_texture_levels(rgba.data(), 8, 8, format, 4);
		std::vector<std::vector<uint8_t>> blocks;
		for (const DxtSurface &surface : surfaces) blocks.push_back(surface.blocks);
		const uint32_t fourcc = format == TextureDxtFormat::Dxt1 ? opennova::dds::dds_fourcc('D', 'X', 'T', '1')
		                                                         : opennova::dds::dds_fourcc('D', 'X', 'T', '5');
		std::vector<uint8_t> file;
		std::string error;
		CHECK(opennova::dds::dds_write_dxt(fourcc, 8, 8, blocks, file, error), "a DXT file written");
		opennova::dds::DdsImage image;
		CHECK(opennova::dds::dds_read(file.data(), file.size(), image, error) && image.loads && image.levels.size() == 4,
				"its chain read");
		opennova::dds::DdsImage first = image;
		CHECK(decode_dds_levels(file.data(), file.size(), image), "a DXT's texels decoded");
		bool same = image.levels.size() == surfaces.size();
		for (size_t i = 0; same && i < surfaces.size(); ++i)
			same = image.levels[i].rgba == encode_rgba8(decode_dxt_surface(surfaces[i]));
		CHECK(same, "each level its blocks decoded");
		CHECK(decode_dds_levels(file.data(), file.size(), first, 1) && !first.levels[0].rgba.empty() &&
				first.levels[1].rgba.empty(), "the first level alone");
	}
	std::vector<uint8_t> argb;
	std::string error;
	CHECK(opennova::dds::dds_write_a8r8g8b8(rgba.data(), 8, 8, argb, error), "an A8R8G8B8 file written");
	opennova::dds::DdsImage plain;
	CHECK(opennova::dds::dds_read(argb.data(), argb.size(), plain, error) && plain.format.decoded, "dds_read decodes it");
	const std::vector<uint8_t> texels = plain.levels[0].rgba;
	CHECK(decode_dds_levels(argb.data(), argb.size(), plain) && plain.levels[0].rgba == texels, "left as dds_read read it");
}

} // namespace

int main() {
	test_dds_levels();
	test_format_selection();
	test_level_count();
	test_dxt1_blocks();
	test_dxt5_blocks();
	test_level_chain();
	test_edge_blocks_repeat_texels();
	test_box_filter();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("texture_dxt: OK");
	return 0;
}
