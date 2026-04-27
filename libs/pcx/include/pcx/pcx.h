#pragma once

#include <cstdint>
#include <vector>

namespace opennova {

struct IndexedImage8 {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> indices;
	uint8_t palette[256][3] = {};

	bool empty() const { return width <= 0 || height <= 0 || indices.empty(); }
};

struct RgbImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;

	bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

} // namespace opennova
