#include <formats/pcx/pcx_quantize.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace opennova {

namespace {

struct ColorCount {
	uint32_t rgb = 0; // 0xRRGGBB
	uint32_t count = 0;
};

int channel(uint32_t rgb, int c) { return int((rgb >> (16 - c * 8)) & 0xFF); }

struct Box {
	size_t first = 0, last = 0; // a range of the color list
	int min[3] = {255, 255, 255}, max[3] = {0, 0, 0};
	uint64_t population = 0;
	void measure(const std::vector<ColorCount> &colors) {
		min[0] = min[1] = min[2] = 255;
		max[0] = max[1] = max[2] = 0;
		population = 0;
		for (size_t i = first; i < last; ++i) {
			for (int c = 0; c < 3; ++c) {
				min[c] = std::min(min[c], channel(colors[i].rgb, c));
				max[c] = std::max(max[c], channel(colors[i].rgb, c));
			}
			population += colors[i].count;
		}
	}
	int widest() const {
		int best = 0;
		for (int c = 1; c < 3; ++c)
			if (max[c] - min[c] > max[best] - min[best]) best = c;
		return best;
	}
	int spread() const { return max[widest()] - min[widest()]; }
};

std::array<uint8_t, 3> average(const std::vector<ColorCount> &colors, const Box &box) {
	uint64_t sum[3] = {0, 0, 0};
	for (size_t i = box.first; i < box.last; ++i)
		for (int c = 0; c < 3; ++c) sum[c] += uint64_t(channel(colors[i].rgb, c)) * colors[i].count;
	std::array<uint8_t, 3> out{};
	for (int c = 0; c < 3; ++c) out[size_t(c)] = uint8_t(box.population ? (sum[c] + box.population / 2) / box.population : 0);
	return out;
}

} // namespace

IndexedImage8 quantize_to_256(const RgbaImage &image) {
	IndexedImage8 out;
	if (image.empty()) return out;
	out.width = image.width;
	out.height = image.height;
	const size_t pixel_count = size_t(image.width) * size_t(image.height);
	std::map<uint32_t, uint32_t> histogram;
	for (size_t i = 0; i < pixel_count; ++i) {
		const uint8_t *p = &image.pixels[i * 4];
		++histogram[(uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2])];
	}
	std::vector<ColorCount> colors;
	colors.reserve(histogram.size());
	for (const auto &entry : histogram) colors.push_back({entry.first, entry.second});

	std::vector<std::array<uint8_t, 3>> palette;
	if (colors.size() <= 256) {
		for (const ColorCount &color : colors)
			palette.push_back({uint8_t(channel(color.rgb, 0)), uint8_t(channel(color.rgb, 1)), uint8_t(channel(color.rgb, 2))});
	} else {
		// Median cut: split the box with the widest channel spread at its population
		// median until 256 boxes exist or nothing is left to split.
		std::vector<Box> boxes(1);
		boxes[0].first = 0;
		boxes[0].last = colors.size();
		boxes[0].measure(colors);
		while (boxes.size() < 256) {
			size_t pick = boxes.size();
			int best_spread = 0;
			for (size_t i = 0; i < boxes.size(); ++i) {
				if (boxes[i].last - boxes[i].first < 2) continue;
				if (boxes[i].spread() > best_spread) { best_spread = boxes[i].spread(); pick = i; }
			}
			if (pick == boxes.size()) break;
			Box &box = boxes[pick];
			const int axis = box.widest();
			std::sort(colors.begin() + long(box.first), colors.begin() + long(box.last),
			          [axis](const ColorCount &a, const ColorCount &b) {
				          const int ca = channel(a.rgb, axis), cb = channel(b.rgb, axis);
				          return ca != cb ? ca < cb : a.rgb < b.rgb;
			          });
			uint64_t seen = 0;
			size_t split = box.first + 1;
			for (size_t i = box.first; i + 1 < box.last; ++i) {
				seen += colors[i].count;
				split = i + 1;
				if (seen * 2 >= box.population) break;
			}
			Box right;
			right.first = split;
			right.last = box.last;
			box.last = split;
			box.measure(colors);
			right.measure(colors);
			boxes.push_back(right);
		}
		for (const Box &box : boxes) palette.push_back(average(colors, box));
	}
	std::sort(palette.begin(), palette.end());
	palette.erase(std::unique(palette.begin(), palette.end()), palette.end());
	for (size_t i = 0; i < palette.size() && i < 256; ++i)
		for (int c = 0; c < 3; ++c) out.palette[i][c] = palette[i][size_t(c)];

	// Every distinct color maps to its nearest entry once; the pixels follow.
	std::map<uint32_t, uint8_t> nearest;
	auto index_of = [&](uint32_t rgb) {
		const auto cached = nearest.find(rgb);
		if (cached != nearest.end()) return cached->second;
		uint8_t best = 0;
		long best_distance = 1L << 30;
		for (size_t i = 0; i < palette.size() && i < 256; ++i) {
			long distance = 0;
			for (int c = 0; c < 3; ++c) {
				const long d = long(channel(rgb, c)) - long(palette[i][size_t(c)]);
				distance += d * d;
			}
			if (distance < best_distance) { best_distance = distance; best = uint8_t(i); }
		}
		nearest[rgb] = best;
		return best;
	};
	out.indices.resize(pixel_count);
	for (size_t i = 0; i < pixel_count; ++i) {
		const uint8_t *p = &image.pixels[i * 4];
		out.indices[i] = index_of((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]));
	}
	return out;
}

} // namespace opennova
