// A sculpted heightmap written again (formats/cpt/trngen heightmap_depth.h, encode_heightmap_depth16; ADR 0046
// S25): under a white of `top` units every height from 0 to the white has a 16-bit sample that the heightmap's
// read (decode_heightmap_depth) takes back to it exactly, through a 16-bit grey PNG the encoder writes; a height
// past the white, and heights between two samples under a white past 256 units, are refused in words.
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/png/png_encode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

namespace {

using namespace opennova;
using namespace opennova::trngen;

constexpr size_t kTexels = size_t(kDepthSide) * kDepthSide;

// Every height from 0 to the white's, over and over, written through a PNG and read back.
int check_top(double top) {
	const int white = static_cast<int>(std::min(65535.0, std::floor(top * 256.0)));
	std::vector<uint16_t> raw16(kTexels);
	for (size_t i = 0; i < kTexels; ++i) raw16[i] = static_cast<uint16_t>(i % size_t(white + 1));
	std::vector<uint16_t> samples;
	std::string why;
	TEST_EXPECT(encode_heightmap_depth16(raw16, top, samples, why));
	const std::vector<uint8_t> png = png::encode_png_gray16(samples.data(), kDepthSide, kDepthSide);
	HeightmapDepth back;
	TEST_EXPECT(decode_heightmap_depth("island_h.png", png, top, back, why));
	TEST_EXPECT(back.depth8.empty() && back.depth16 == raw16);
	std::printf("top %.3f: every height 0..%d reads back as written\n", top, white);
	return 0;
}

int test_refusals() {
	std::vector<uint16_t> raw16(kTexels, 100);
	std::vector<uint16_t> samples;
	std::string why;
	raw16[5] = 64 * 256 + 300; // past a white of 64 units
	TEST_EXPECT(!encode_heightmap_depth16(raw16, 64.0, samples, why) && why.find("raise the heightmap's top") != std::string::npos);
	// A white of 300 units: a sample is 1.17 raw, so some raw steps fall between two samples.
	for (size_t i = 0; i < kTexels; ++i) raw16[i] = static_cast<uint16_t>(i % 2000);
	TEST_EXPECT(!encode_heightmap_depth16(raw16, 300.0, samples, why) && why.find("have no 16-bit sample") != std::string::npos);
	std::printf("refusals: a height past the white, heights between samples, said in words\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	for (double top : {32.0, 64.0, 100.0, kDepth8Top, 128.0, 255.99}) failures += check_top(top);
	failures += test_refusals();
	if (failures == 0) std::printf("heightmap_depth_encode16: all passed\n");
	return failures == 0 ? 0 : 1;
}
