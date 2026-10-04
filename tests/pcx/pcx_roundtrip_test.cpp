#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	opennova::IndexedImage8 source;
	source.width = 4;
	source.height = 2;
	source.indices = {
		0, 1, 2, 3,
		3, 2, 1, 0,
	};
	source.palette[0][0] = 0;
	source.palette[0][1] = 0;
	source.palette[0][2] = 0;
	source.palette[1][0] = 255;
	source.palette[1][1] = 0;
	source.palette[1][2] = 0;
	source.palette[2][0] = 0;
	source.palette[2][1] = 255;
	source.palette[2][2] = 0;
	source.palette[3][0] = 0;
	source.palette[3][1] = 0;
	source.palette[3][2] = 255;

	std::vector<uint8_t> encoded;
	std::string error;
	if (!opennova::encode_pcx_indexed(source, encoded, error)) {
		std::fprintf(stderr, "FAIL: encode_pcx_indexed failed: %s\n", error.c_str());
		return 1;
	}

	opennova::IndexedImage8 indexed;
	error.clear();
	if (!opennova::decode_pcx_indexed(encoded.data(), encoded.size(), indexed, error)) {
		std::fprintf(stderr, "FAIL: decode_pcx_indexed failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(indexed.width == source.width, "indexed width should round-trip")) return 1;
	if (!expect(indexed.height == source.height, "indexed height should round-trip")) return 1;
	if (!expect(indexed.indices == source.indices, "indexed pixels should round-trip")) return 1;
	if (!expect(indexed.palette[1][0] == 255 && indexed.palette[2][1] == 255 && indexed.palette[3][2] == 255,
	            "palette entries should round-trip")) return 1;

	opennova::RgbImage rgb;
	error.clear();
	if (!opennova::decode_pcx_rgb(encoded.data(), encoded.size(), rgb, error)) {
		std::fprintf(stderr, "FAIL: decode_pcx_rgb failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(rgb.width == source.width, "rgb width should match source")) return 1;
	if (!expect(rgb.height == source.height, "rgb height should match source")) return 1;
	if (!expect(rgb.pixels.size() == static_cast<size_t>(source.width * source.height * 3),
	            "rgb pixel payload should match source dimensions")) return 1;
	if (!expect(rgb.pixels[3] == 255 && rgb.pixels[4] == 0 && rgb.pixels[5] == 0,
	            "palette index 1 should decode to red")) return 1;

	// A 24-bit PCX (three planes of colour) of an odd width round-trips its colours, and reads as no
	// indexed image.
	opennova::RgbImage colours;
	colours.width = 3;
	colours.height = 2;
	for (int i = 0; i < colours.width * colours.height; ++i)
		colours.pixels.insert(colours.pixels.end(), {uint8_t(i * 40), uint8_t(200 - i * 30), uint8_t(i * 7 + 1)});
	std::vector<uint8_t> planes;
	error.clear();
	if (!expect(opennova::encode_pcx_rgb(colours, planes, error), "encode_pcx_rgb should write three planes")) return 1;
	if (!expect(planes.size() > 128 && planes[65] == 3, "the header should say three planes")) return 1;
	opennova::RgbImage back;
	if (!expect(opennova::decode_pcx_rgb(planes.data(), planes.size(), back, error), "the 24-bit PCX should decode")) return 1;
	if (!expect(back.width == 3 && back.height == 2 && back.pixels == colours.pixels, "24-bit colours should round-trip"))
		return 1;
	opennova::IndexedImage8 none;
	if (!expect(!opennova::decode_pcx_indexed(planes.data(), planes.size(), none, error),
	            "a 24-bit PCX is no indexed image"))
		return 1;

	std::printf("OK: pcx indexed/rgb round-trip preserved payload and palette; 24-bit planes round-trip\n");
	return 0;
}
