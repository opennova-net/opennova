// The 256-colour quantizer an 8-bit PCX is written through (formats/pcx/pcx_quantize.h; moved from the
// editor's import test): an exact small palette, sorted, through a PCX round trip; a median cut past 256
// colours, every index valid and nearby colours mapped close; the same source the same indices.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <formats/pcx/pcx_io.h>
#include <formats/pcx/pcx_quantize.h>

#include "common/test_expect.h"

using opennova::IndexedImage8;
using opennova::RgbaImage;
using opennova::decode_pcx_indexed;
using opennova::encode_pcx_indexed;
using opennova::quantize_to_256;

namespace {

int test_quantize() {
	// Three colors: the palette is exact and sorted, and the PCX round trip keeps every pixel.
	RgbaImage small;
	small.width = 3;
	small.height = 1;
	small.pixels = {200, 0, 0, 255, 0, 200, 0, 255, 0, 0, 200, 255};
	IndexedImage8 indexed = quantize_to_256(small);
	TEST_EXPECT(indexed.width == 3 && indexed.indices.size() == 3);
	TEST_EXPECT(indexed.palette[indexed.indices[0]][0] == 200 && indexed.palette[indexed.indices[1]][1] == 200 &&
	            indexed.palette[indexed.indices[2]][2] == 200);
	std::vector<uint8_t> pcx;
	std::string error;
	TEST_EXPECT(encode_pcx_indexed(indexed, pcx, error));
	IndexedImage8 back;
	TEST_EXPECT(decode_pcx_indexed(pcx.data(), pcx.size(), back, error) && back.width == 3 && back.indices == indexed.indices);
	// Past 256 colors: at most 256 entries, every index valid, nearby colors map close.
	RgbaImage many;
	many.width = 64;
	many.height = 64;
	for (int y = 0; y < 64; ++y)
		for (int x = 0; x < 64; ++x) {
			many.pixels.push_back(uint8_t(x * 4));
			many.pixels.push_back(uint8_t(y * 4));
			many.pixels.push_back(uint8_t((x + y) * 2));
			many.pixels.push_back(255);
		}
	IndexedImage8 reduced = quantize_to_256(many);
	TEST_EXPECT(reduced.indices.size() == 64 * 64);
	int used = 0;
	bool seen[256] = {};
	for (const uint8_t index : reduced.indices) if (!seen[index]) { seen[index] = true; ++used; }
	TEST_EXPECT(used > 64 && used <= 256);
	long worst = 0;
	for (size_t i = 0; i < reduced.indices.size(); ++i)
		for (int c = 0; c < 3; ++c)
			worst = std::max(worst, std::labs(long(many.pixels[i * 4 + size_t(c)]) - long(reduced.palette[reduced.indices[i]][c])));
	TEST_EXPECT(worst < 40);
	// Deterministic.
	const IndexedImage8 again = quantize_to_256(many);
	TEST_EXPECT(again.indices == reduced.indices);
	return 0;
}

} // namespace

int main() {
	if (test_quantize() != 0) return 1;
	std::printf("pcx_quantize: an exact palette, a median cut, deterministic\n");
	return 0;
}
