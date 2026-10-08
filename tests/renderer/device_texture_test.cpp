// The device texture a texture file becomes (renderer/device_texture.h): the object texture detail's
// flag per slot and level, the halvings of a texture built from pixels, a DDS's skipped levels and its
// opaque DXT5 stored as DXT1, the bytes each holds and the game's own counter, against vectors worked by
// hand from the witnesses.

#include <runtime/renderer/device_texture.h>

#include <cstdio>
#include <vector>

using namespace opennova::renderer;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

constexpr uint64_t kMB = 1024 * 1024;

void test_object_texdetail_flags() {
	// Level 0: two halvings for slots 1, 2, 8 and 9.
	for (uint8_t slot : {1, 2, 8, 9}) CHECK(object_texdetail_flags(0, slot) == kTextureFlagHalveTwice, "level 0 halves twice");
	// Level 1: one for slots 1 and 2, two for 8 and 9.
	CHECK(object_texdetail_flags(1, 1) == kTextureFlagHalveOnce, "level 1, slot 1: once");
	CHECK(object_texdetail_flags(1, 2) == kTextureFlagHalveOnce, "level 1, slot 2: once");
	CHECK(object_texdetail_flags(1, 8) == kTextureFlagHalveTwice, "level 1, slot 8: twice");
	// Level 2: none for slot 1, one for 2, 8 and 9.
	CHECK(object_texdetail_flags(2, 1) == 0, "level 2, slot 1: none");
	CHECK(object_texdetail_flags(2, 2) == kTextureFlagHalveOnce, "level 2, slot 2: once");
	CHECK(object_texdetail_flags(2, 9) == kTextureFlagHalveOnce, "level 2, slot 9: once");
	// Level 3 and past it, and the normal-map slots at any level: none.
	for (uint8_t slot : {1, 2, 8, 9}) CHECK(object_texdetail_flags(3, slot) == 0, "level 3: none");
	CHECK(object_texdetail_flags(7, 1) == 0, "a level past 3: none");
	for (int level = 0; level < kObjectTexDetailLevels; ++level)
		for (uint8_t slot : {0, 3, 4, 5, 6, 7, 10})
			CHECK(object_texdetail_flags(level, slot) == 0, "slots 3 to 7 and any other: none");
}

void test_pixel_halvings() {
	CHECK(pixel_texture_halvings(2048, 2048, 0) == 0, "no flag: none below the card's side");
	CHECK(pixel_texture_halvings(16384, 16384, 0) == 1, "past the card's 8192: halved to it");
	// The normal map's cap: 4096 is halved three times to 512, a 512 none, a 512 x 1024 once.
	CHECK(pixel_texture_halvings(4096, 4096, kTextureFlagCap512) == 3, "4096 capped at 512");
	CHECK(pixel_texture_halvings(512, 512, kTextureFlagCap512) == 0, "512 kept");
	CHECK(pixel_texture_halvings(512, 1024, kTextureFlagCap512) == 1, "either side past the cap");
	// The last cap flag wins.
	CHECK(pixel_texture_halvings(1024, 1024, kTextureFlagCap512 | kTextureFlagCap128) == 3, "the 128 cap wins");
	// The detail's halvings come after the cap, whatever the size.
	CHECK(pixel_texture_halvings(2048, 2048, kTextureFlagHalveOnce) == 1, "once");
	CHECK(pixel_texture_halvings(2048, 2048, kTextureFlagHalveTwice) == 2, "twice");
	CHECK(pixel_texture_halvings(4, 4, kTextureFlagHalveTwice) == 2, "a small one too");
	CHECK(pixel_texture_halvings(1024, 1024, kTextureFlagCap512 | kTextureFlagHalveOnce) == 2, "cap then detail");
	// 0x20000 wins over 0x10000.
	CHECK(pixel_texture_halvings(256, 256, kTextureFlagHalveOnce | kTextureFlagHalveTwice) == 2, "twice wins");
	// Without a cap flag the card's side is the cap; with one, the card's side halves again after it.
	CHECK(pixel_texture_halvings(65536, 65536, kTextureFlagHalveOnce, 8192) == 4, "the card's side, then the detail");
	CHECK(pixel_texture_halvings(1024, 1024, kTextureFlagCap512, 256) == 2, "the cap, then a card of 256");
}

void test_pixel_device_texture() {
	// The base game's crate diffuse: 2048 x 2048 A8R8G8B8, ten levels down to 4 x 4.
	const DeviceTexture crate = pixel_device_texture(2048, 2048, 0);
	CHECK(crate.format == DeviceTextureFormat::A8R8G8B8, "pixels make A8R8G8B8");
	CHECK(crate.width == 2048 && crate.height == 2048, "kept at full detail");
	CHECK(crate.levels == 10, "the chain ends at 4 x 4");
	uint64_t expected = 0;
	for (uint32_t side = 2048; side >= 4; side /= 2) expected += uint64_t(side) * side * 4;
	CHECK(crate.bytes == expected, "every level's bytes");
	CHECK(crate.bytes > 21 * kMB && crate.bytes < 22 * kMB, "about 21.3 MB");
	CHECK(crate.stat_bytes == 2048ull * 2048 * 4, "the game counts the first level");
	// At the lowest detail (slot 1: twice) it is 512 x 512.
	const DeviceTexture low = pixel_device_texture(2048, 2048, object_texdetail_flags(0, 1));
	CHECK(low.width == 512 && low.height == 512 && low.halvings == 2, "halved twice at detail 0");
	CHECK(low.levels == 8, "512's chain to 4 x 4");
	// A 4096 normal map capped to 512.
	const DeviceTexture normal = pixel_device_texture(4096, 4096, kTextureFlagCap512);
	CHECK(normal.width == 512 && normal.halvings == 3, "the normal map's cap");
	// A non-square texture: its chain by the smaller side (256 x 4: one halving of 4 is past 2, one level);
	// a smaller side of 2 or less asks for none, which D3DX makes its full chain, each level at least 1 a side.
	const DeviceTexture strip = pixel_device_texture(256, 4, 0);
	CHECK(strip.levels == 1 && strip.bytes == 256 * 4 * 4, "a smaller side of 4: one level");
	const DeviceTexture thin = pixel_device_texture(256, 2, 0);
	CHECK(thin.levels == 9, "a smaller side of 2: D3DX's full chain to 1 x 1");
	CHECK(thin.bytes == 4 * (256 * 2 + 128 * 1 + 64 + 32 + 16 + 8 + 4 + 2 + 1), "each level at least 1 a side");
	// Halved to nothing, D3DX makes 1 x 1.
	const DeviceTexture tiny = pixel_device_texture(2, 2, kTextureFlagHalveTwice);
	CHECK(tiny.width == 1 && tiny.height == 1, "a side of 0 made 1");
}

void test_dds_device_texture() {
	// A 1024 DXT5 with its full chain at full detail: its own size, its complete chain.
	DdsSource dxt5;
	dxt5.width = dxt5.height = 1024;
	dxt5.levels = 11;
	dxt5.format = DeviceTextureFormat::Dxt5;
	const DeviceTexture full = dds_device_texture(dxt5, 0);
	CHECK(full.format == DeviceTextureFormat::Dxt5 && full.width == 1024 && full.levels == 11, "kept whole");
	uint64_t expected = 0;
	for (uint32_t side = 1024; side >= 1; side /= 2) expected += device_level_bytes(side, side, DeviceTextureFormat::Dxt5);
	CHECK(full.bytes == expected, "every level's blocks");
	CHECK(full.stat_bytes == 1024ull * 1024, "DXT5 counted a byte a texel");
	// At detail 0 (slot 1) its top two levels are skipped.
	const DeviceTexture low = dds_device_texture(dxt5, object_texdetail_flags(0, 1));
	CHECK(low.width == 256 && low.levels == 9 && low.halvings == 2 && !low.whole, "two levels skipped");
	// A chainless DDS at a detail that skips loads whole, with D3DX's complete chain.
	DdsSource flat = dxt5;
	flat.levels = 1;
	const DeviceTexture whole = dds_device_texture(flat, kTextureFlagHalveOnce);
	CHECK(whole.whole && whole.width == 1024 && whole.levels == 11, "a chain too short loads whole");
	CHECK(whole.stat_bytes == 512ull * 512, "the game counts it as if halved");
	// A chain of exactly skip + 1 levels loads whole too.
	DdsSource two = dxt5;
	two.levels = 2;
	CHECK(dds_device_texture(two, kTextureFlagHalveOnce).whole, "two levels, one skipped: whole");
	// The cap grows a skip once one is taken, never without one.
	CHECK(dds_device_texture(dxt5, kTextureFlagCap512).width == 1024, "no detail skip: the cap is not applied");
	DdsSource big = dxt5;
	big.width = big.height = 2048;
	big.levels = 12;
	const DeviceTexture capped = dds_device_texture(big, kTextureFlagCap512 | kTextureFlagHalveOnce);
	CHECK(capped.width == 512 && capped.halvings == 2 && capped.levels == 10, "skip one for the detail, then the cap's");
	// An opaque DXT5 is stored as DXT1, half the bytes.
	DdsSource opaque = dxt5;
	opaque.dxt5_opaque = true;
	const DeviceTexture as1 = dds_device_texture(opaque, 0);
	CHECK(as1.format == DeviceTextureFormat::Dxt1 && as1.dxt5_as_dxt1, "opaque DXT5 made DXT1");
	CHECK(as1.bytes * 2 == full.bytes, "half the bytes, a block of 8 for each of 16");
	CHECK(as1.stat_bytes == 1024ull * 1024 / 2, "DXT1 counted half a byte a texel");
	// Not on a card that keeps DXT5, nor one without DXT1.
	TextureDxtCaps keep;
	keep.keep_dxt5 = true;
	CHECK(dds_device_texture(opaque, 0, keep).format == DeviceTextureFormat::Dxt5, "the kept DXT5");
	TextureDxtCaps no_dxt1;
	no_dxt1.dxt1 = false;
	CHECK(dds_device_texture(opaque, 0, no_dxt1).format == DeviceTextureFormat::Dxt5, "no DXT1 card");
	// A card without DXT5 decodes it into A8R8G8B8.
	TextureDxtCaps no_dxt5;
	no_dxt5.dxt5 = false;
	const DeviceTexture decoded = dds_device_texture(dxt5, 0, no_dxt5);
	CHECK(decoded.format == DeviceTextureFormat::A8R8G8B8 && decoded.levels == 11, "decoded to A8R8G8B8");
	// An A8R8G8B8 DDS (the preview cube's form) at 32 bits a texel.
	DdsSource argb;
	argb.width = argb.height = 64;
	argb.levels = 1;
	argb.format = DeviceTextureFormat::A8R8G8B8;
	const DeviceTexture argb_texture = dds_device_texture(argb, 0);
	CHECK(argb_texture.levels == 7 && argb_texture.bytes > 64ull * 64 * 4, "its complete chain");
}

void test_level_bytes() {
	CHECK(device_level_bytes(1, 1, DeviceTextureFormat::Dxt1) == 8, "one block at least");
	CHECK(device_level_bytes(5, 5, DeviceTextureFormat::Dxt5) == 4 * 16, "blocks round up");
	CHECK(device_level_bytes(4, 4, DeviceTextureFormat::Uncompressed, 16) == 32, "16 bits a texel");
	CHECK(full_chain_levels(1024, 256) == 11, "by the larger side");
	CHECK(full_chain_levels(1, 1) == 1, "a 1 x 1 is one level");
}

void test_dxt5_opaque() {
	// Two blocks of an 8 x 4 level: both alpha endpoints 255 in each.
	std::vector<uint8_t> blocks(32, 0);
	blocks[0] = blocks[1] = blocks[16] = blocks[17] = 0xFF;
	CHECK(dxt5_first_level_opaque(blocks.data(), blocks.size(), 8, 4), "every block's endpoints 255");
	blocks[17] = 0xFE;
	CHECK(!dxt5_first_level_opaque(blocks.data(), blocks.size(), 8, 4), "one endpoint below");
	// The indices are never read: endpoints of 255 with every index set still pass.
	std::vector<uint8_t> indexed(16, 0xFF);
	CHECK(dxt5_first_level_opaque(indexed.data(), indexed.size(), 4, 4), "the indices are not read");
	// A side below 4 counts one block; a side of 6 counts one (6 / 4).
	std::vector<uint8_t> one(16, 0);
	one[0] = one[1] = 0xFF;
	CHECK(dxt5_first_level_opaque(one.data(), one.size(), 2, 2), "a small level: one block");
	CHECK(dxt5_first_level_opaque(one.data(), one.size(), 6, 6), "a side of 6: one block, as the game counts");
	CHECK(!dxt5_first_level_opaque(one.data(), 8, 4, 4), "bytes too few");
}

// The levels of a texture built from pixels as their texels read back: A8R8G8B8
// keeps level 0 and box-filters the stored bytes; a DXT format's levels are its
// blocks decoded; the halvings come first [orig: GTexture_CreateFromPixelData_0
// @ 0x6876C0].
void test_pixel_device_texture_levels() {
	std::vector<uint8_t> rgba(16 * 16 * 4, 0);
	for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<uint8_t>((i * 37) % 256);
	const auto plain = pixel_device_texture_levels(rgba.data(), 16, 16, 0x100001u);
	CHECK(plain.size() == 3 && plain[0].width == 16 && plain[2].width == 4, "16 to 4 uncompressed");
	CHECK(plain[0].rgba == rgba, "level 0 is the pixels");
	CHECK(plain[1].rgba == encode_rgba8(box_filter_half(decode_rgba8(rgba.data(), 16, 16), 16, 16)),
			"level 1 is D3DX's box filter of the stored bytes");
	const auto dxt1 = pixel_device_texture_levels(rgba.data(), 16, 16, 0x500201u);
	const auto blocks = build_dxt_texture_levels(rgba.data(), 16, 16, TextureDxtFormat::Dxt1, 3);
	CHECK(dxt1.size() == 3 && dxt1[0].rgba == encode_rgba8(decode_dxt_surface(blocks[0])) &&
			dxt1[2].rgba == encode_rgba8(decode_dxt_surface(blocks[2])),
			"0x500201 is DXT1 on the reference card, its blocks decoded");
	const auto limited = pixel_device_texture_levels(rgba.data(), 16, 16, 0x500201u,
			kReferenceTextureDxtCaps, 1);
	CHECK(limited.size() == 1 && limited[0].rgba == dxt1[0].rgba, "a level limit keeps level 0");
	const auto halved = pixel_device_texture_levels(rgba.data(), 16, 16, kTextureFlagHalveOnce);
	CHECK(!halved.empty() && halved[0].width == 8 && halved.size() == 2, "0x10000 halves first");
	CHECK(pixel_device_texture_levels(nullptr, 16, 16, 0).empty(), "no pixels, no levels");
}

} // namespace

int main() {
	test_pixel_device_texture_levels();
	test_object_texdetail_flags();
	test_pixel_halvings();
	test_pixel_device_texture();
	test_dds_device_texture();
	test_level_bytes();
	test_dxt5_opaque();
	if (failures != 0) {
		std::fprintf(stderr, "device_texture: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("device_texture: ok\n");
	return 0;
}
