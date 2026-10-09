// A heightmap made into the depth the TrnGen bake takes (formats/cpt/trngen heightmap_depth.h): an 8-bit PNG at
// TrnGen's own scale as it is, at another smoothed then scaled; a 16-bit PNG over 0 to `top`; TrnGen's own .raw and
// the game's raw16 as they are; any other size refused in words.
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/cpt/trngen/terrain_bake.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/png_test_support.h"
#include "common/test_expect.h"

namespace {

using opennova::trngen::HeightmapDepth;
using opennova::trngen::decode_heightmap_depth;
using opennova::trngen::kDepthSide;
using test_png::PngSpec;
using test_png::make_png;

constexpr int kSide = kDepthSide;

// A 16-bit grey PNG of an island: a cone from the centre, its top `peak` of 65535.
std::vector<uint8_t> island_png16(double peak) {
	PngSpec spec;
	spec.width = kSide;
	spec.height = kSide;
	spec.depth = 16;
	spec.color_type = 0;
	for (int y = 0; y < kSide; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < kSide; ++x) {
			const double r = std::hypot(x - 512.0, y - 512.0) / 400.0;
			const uint16_t v = static_cast<uint16_t>(65535.0 * peak * std::max(0.0, 1.0 - r));
			spec.rows.push_back(uint8_t(v >> 8));
			spec.rows.push_back(uint8_t(v & 0xFF));
		}
	}
	return make_png(spec);
}

// An 8-bit grey PNG of `side` texels, a ramp.
std::vector<uint8_t> ramp_png8(int side) {
	PngSpec spec;
	spec.width = side;
	spec.height = side;
	spec.depth = 8;
	spec.color_type = 0;
	for (int y = 0; y < side; ++y) {
		spec.rows.push_back(0);
		for (int x = 0; x < side; ++x) spec.rows.push_back(uint8_t(x * 255 / std::max(1, side - 1)));
	}
	return make_png(spec);
}

int test_depths() {
	TEST_EXPECT(opennova::trngen::kDepth8Top == 127.5);
	HeightmapDepth heights;
	std::string why;
	// 16 bits: 0..65535 over 0..top, 256 raw a world unit.
	TEST_EXPECT(decode_heightmap_depth("h.png", island_png16(1.0), 127.5, heights, why));
	TEST_EXPECT(heights.depth8.empty() && heights.depth16.size() == size_t(kSide) * kSide);
	TEST_EXPECT(heights.depth16[512 * kSide + 512] == 32640 && heights.depth16[0] == 0);
	TEST_EXPECT(decode_heightmap_depth("h.png", island_png16(1.0), 200.0, heights, why) &&
	            heights.depth16[512 * kSide + 512] == 51200);
	// 8 bits at TrnGen's own scale go to the bake as they are; at another, TrnGen's smoothing then the scale.
	TEST_EXPECT(decode_heightmap_depth("h.png", ramp_png8(kSide), 127.5, heights, why) && heights.depth16.empty() &&
	            heights.depth8.size() == size_t(kSide) * kSide && heights.depth8[kSide - 1] == 255);
	TEST_EXPECT(decode_heightmap_depth("h.png", ramp_png8(kSide), 63.75, heights, why) && heights.depth8.empty() &&
	            heights.depth16.size() == size_t(kSide) * kSide);
	// TrnGen's .raw (1 MiB, 8 bits) and the game's raw16 (2 MiB), taken as they are.
	TEST_EXPECT(decode_heightmap_depth("h.raw", std::vector<uint8_t>(size_t(kSide) * kSide, 7), 127.5, heights, why) &&
	            heights.depth8[0] == 7);
	std::vector<uint8_t> raw16(size_t(kSide) * kSide * 2, 0);
	raw16[0] = 0x34;
	raw16[1] = 0x12;
	TEST_EXPECT(decode_heightmap_depth("h.raw", raw16, 10.0, heights, why) && heights.depth16[0] == 0x1234);
	// Any other size refused in words.
	TEST_EXPECT(!decode_heightmap_depth("h.png", ramp_png8(512), 127.5, heights, why) &&
	            why.find("1024 x 1024") != std::string::npos && why.find("h.png") == 0);
	TEST_EXPECT(!decode_heightmap_depth("h.raw", std::vector<uint8_t>(100), 127.5, heights, why) &&
	            why.find("100 bytes") != std::string::npos);
	std::printf("depths: 16-bit over top, 8-bit as TrnGen's or smoothed and scaled, the .raw forms, sizes refused\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_depths();
	if (failures == 0) std::printf("cpt_trngen_heightmap_depth: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
