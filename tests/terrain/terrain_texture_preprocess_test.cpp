#include <runtime/terrain/texture_preprocess.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using opennova::terrain::Rgba8Image;

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool expect_pixels(const Rgba8Image &actual,
		uint32_t width,
		uint32_t height,
		const std::vector<uint8_t> &expected,
		const char *message) {
	if (actual.width == width && actual.height == height && actual.pixels == expected) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s (got %ux%u, %zu bytes)\n",
			message, actual.width, actual.height, actual.pixels.size());
	return false;
}

bool expect_pixel(const Rgba8Image &actual,
		uint32_t x,
		uint32_t y,
		const std::array<uint8_t, 4> &expected,
		const char *message) {
	if (!actual.is_valid() || x >= actual.width || y >= actual.height) {
		std::fprintf(stderr, "FAIL: %s (invalid image/coordinate)\n", message);
		return false;
	}
	const size_t offset = (static_cast<size_t>(y) * actual.width + x) * 4;
	for (size_t channel = 0; channel < expected.size(); ++channel) {
		if (actual.pixels[offset + channel] != expected[channel]) {
			std::fprintf(stderr,
					"FAIL: %s (pixel %u,%u channel %zu: got %u expected %u)\n",
					message, x, y, channel, actual.pixels[offset + channel],
					expected[channel]);
			return false;
		}
	}
	return true;
}

Rgba8Image make_directional_detailmap() {
	const uint8_t blue[16] = {
		10, 20, 30, 40,
		50, 60, 70, 80,
		90, 100, 110, 120,
		130, 140, 150, 160,
	};
	Rgba8Image image{4, 4, {}};
	image.pixels.reserve(64);
	for (uint8_t i = 0; i < 16; ++i) {
		image.pixels.push_back(200);
		image.pixels.push_back(10);
		image.pixels.push_back(blue[i]);
		image.pixels.push_back(static_cast<uint8_t>(17 + i));
	}
	return image;
}

void make_mip_pair(Rgba8Image &base, Rgba8Image &far_detail) {
	base = Rgba8Image{8, 8, {}};
	far_detail = Rgba8Image{8, 8, {}};
	base.pixels.reserve(8 * 8 * 4);
	far_detail.pixels.reserve(8 * 8 * 4);
	for (uint8_t y = 0; y < 8; ++y) {
		for (uint8_t x = 0; x < 8; ++x) {
			base.pixels.insert(base.pixels.end(), {
				static_cast<uint8_t>(x * 10),
				static_cast<uint8_t>(y * 10),
				static_cast<uint8_t>(x + y),
				static_cast<uint8_t>(100 + x + 8 * y),
			});
			far_detail.pixels.insert(far_detail.pixels.end(), {
				static_cast<uint8_t>(200 - x * 5),
				static_cast<uint8_t>(150 - y * 5),
				static_cast<uint8_t>(240 - x - y),
				static_cast<uint8_t>(7 + x + y),
			});
		}
	}
}

} // namespace

int main() {
	using opennova::terrain::build_detail_coefficient_map;
	using opennova::terrain::build_heightfield_normal_map;
	using opennova::terrain::build_paired_detail_mip_chain;
	using opennova::terrain::normalize_detail_blend_map;

	bool ok = true;

	// The generator reads authored BLUE, wraps both axes with a power-of-two
	// mask, uses scale 1/32 and the sum of two unit-Z vectors, truncates (n+1)*127.5,
	// and preserves alpha. [orig: Texture_GenerateNormalMap @ 0x58c1fa]
	const std::vector<uint8_t> expected_coefficient = {
		151,225,205,17, 103,225,205,18, 103,225,205,19, 151,225,205,20,
		151,29,205,21, 103,29,205,22, 103,29,205,23, 151,29,205,24,
		151,29,205,25, 103,29,205,26, 103,29,205,27, 151,29,205,28,
		151,225,205,29, 103,225,205,30, 103,225,205,31, 151,225,205,32,
	};
	ok &= expect_pixels(build_detail_coefficient_map(make_directional_detailmap()),
			4, 4, expected_coefficient,
			"detail coefficient map must match the recovered wrapped blue-channel vector");

	std::vector<uint16_t> heightfield(16);
	for (uint32_t y = 0; y < 4; ++y) {
		for (uint32_t x = 0; x < 4; ++x) {
			heightfield[y * 4 + x] = static_cast<uint16_t>(x * 256);
		}
	}
	// The centred difference over an up of 2: a unit ramp is a 45-degree
	// normal. [orig: Terrain_GenerateNormalMap @ 0x603210, the two unit ups
	// summed @ 0x603425] (D-TERRAIN-19)
	const std::vector<uint8_t> expected_height_normals = {
		217,127,217,128, 37,127,217,128, 37,127,217,128, 217,127,217,128,
		217,127,217,128, 37,127,217,128, 37,127,217,128, 217,127,217,128,
		217,127,217,128, 37,127,217,128, 37,127,217,128, 217,127,217,128,
		217,127,217,128, 37,127,217,128, 37,127,217,128, 217,127,217,128,
	};
	ok &= expect_pixels(build_heightfield_normal_map(heightfield, 4, 4),
			4, 4, expected_height_normals,
			"heightfield normal atlas must wrap and truncate texture-basis RGB/A128");

	// Each lock component changes only its own quadrant/axis: non-zero wraps
	// neighbour taps within the half-atlas, while zero crosses the internal
	// seam and wraps across the full atlas.
	std::vector<uint16_t> x_ramp(8 * 8);
	std::vector<uint16_t> y_ramp(8 * 8);
	for (uint32_t y = 0; y < 8; ++y) {
		for (uint32_t x = 0; x < 8; ++x) {
			x_ramp[y * 8 + x] = static_cast<uint16_t>(x * 256);
			y_ramp[y * 8 + x] = static_cast<uint16_t>(y * 256);
		}
	}
	opennova::TerrainQuadrantLocks x_locks{};
	x_locks[0].x = 1;
	const Rgba8Image locked_x =
			build_heightfield_normal_map(x_ramp, 8, 8, x_locks);
	ok &= expect_pixel(locked_x, 3, 0, {217,127,217,128},
			"TL X lock must wrap the +X tap to the TL quadrant origin");
	ok &= expect_pixel(locked_x, 3, 4, {37,127,217,128},
			"unlocked BL X must continue across the full atlas seam");

	opennova::TerrainQuadrantLocks y_locks{};
	y_locks[3].y = 1;
	const Rgba8Image locked_y =
			build_heightfield_normal_map(y_ramp, 8, 8, y_locks);
	ok &= expect_pixel(locked_y, 7, 7, {127,217,217,128},
			"BR Y lock must wrap the +Y tap to the BR quadrant origin");
	ok &= expect_pixel(locked_y, 3, 7, {127,248,167,128},
			"unlocked BL Y must wrap across the full atlas");

	ok &= expect(!build_heightfield_normal_map({0}, 1, 1).is_valid(),
			"heightfield normal atlas requires four non-empty quadrants");

	const Rgba8Image blendmap{5, 1, {
		1,2,3,17, 5,0,0,23, 0,0,0,34, 100,100,100,45, 255,255,255,56,
	}};
	const std::vector<uint8_t> expected_blend = {
		42,85,127,17, 255,0,0,23, 255,0,0,34, 85,85,85,45, 84,84,84,56,
	};
	ok &= expect_pixels(normalize_detail_blend_map(blendmap),
			5, 1, expected_blend,
			"DBlend normalization must match 0xffff/sum integer scaling and zero-to-red");

	Rgba8Image base;
	Rgba8Image far_detail;
	make_mip_pair(base, far_detail);
	const auto mips = build_paired_detail_mip_chain(base, far_detail);
	ok &= expect(mips.size() == 2,
			"8x8 paired detail textures must produce retail levels 8x8 and 4x4");
	if (mips.size() == 2) {
		ok &= expect_pixels(mips[0], 8, 8, base.pixels,
				"paired detail mip 0 must remain byte-identical to base");
		const std::vector<uint8_t> expected_mip1 = {
			125,93,149,104, 126,93,149,106, 127,93,148,108, 128,93,148,110,
			125,95,149,120, 126,95,148,122, 127,95,148,124, 128,95,147,126,
			125,96,148,136, 126,96,148,138, 127,96,147,140, 128,96,147,142,
			125,97,148,152, 126,97,147,154, 127,97,147,156, 128,97,146,158,
		};
		ok &= expect_pixels(mips[1], 4, 4, expected_mip1,
				"paired detail mip 1 must blend independently downsampled RGB at 160/96 and preserve base alpha");
	}

	// The detail layers are DXT1 on the reference adapter: without a far
	// texture the layer is D3DX's filtered level chain, with one each paired
	// level is encoded as it is. [orig: PolyTrn_InitTextures @ 0x60ABBC,
	// @ 0x60ABA0; GTexture_CreateFromPixelDataWithAlphaBlend @ 0x6875C5]
	{
		using opennova::renderer::TextureDxtFormat;
		const auto plain = opennova::terrain::build_detail_layer_levels(base, nullptr);
		const auto plain_chain = opennova::renderer::build_dxt_texture_levels(
				base.pixels.data(), base.width, base.height, TextureDxtFormat::Dxt1, 2);
		ok &= expect(plain.size() == 2 && plain[0].format == TextureDxtFormat::Dxt1 &&
				plain[0].blocks == plain_chain[0].blocks &&
				plain[1].blocks == plain_chain[1].blocks,
				"an unpaired detail layer is the DXT1 filtered chain down to 4x4");
		const auto paired = opennova::terrain::build_detail_layer_levels(base, &far_detail);
		ok &= expect(paired.size() == mips.size(), "a paired layer keeps the paired level count");
		for (size_t level = 0; level < paired.size() && level < mips.size(); ++level) {
			const auto encoded = opennova::renderer::encode_dxt_surface(
					opennova::renderer::decode_rgba8(mips[level].pixels.data(),
							mips[level].width, mips[level].height),
					mips[level].width, mips[level].height, TextureDxtFormat::Dxt1);
			ok &= expect(paired[level].blocks == encoded.blocks,
					"each paired level is encoded straight from its blended texels");
		}
	}

	if (!ok) {
		return 1;
	}
	std::puts("OK: terrain texture preprocessing matches recovered retail byte vectors");
	return 0;
}
