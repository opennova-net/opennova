#include <editor/preview/mission_map_terrain_view.h>

#include <cstddef>

namespace opennova::editor {

namespace {

constexpr uint32_t kOpaque = 0xFF000000u;

// The atlas copied into its four quadrants one after another, each `width / 2` rows of `width / 2` words: the first
// the top-left, the second the rows past the half, the third the columns past it, the fourth both [orig: gobj.dll
// sub_100307BA @ 0x100307ba, over a 32-bit copy of the atlas @ 0x10010762..0x10010793].
std::vector<uint32_t> quadrants(const uint16_t *atlas, int width, int height) {
	const int half = width >> 1;
	std::vector<uint32_t> out;
	out.reserve(size_t(width) * size_t(height));
	for (int i = 0; i < 4; ++i) {
		const int row0 = (i & 1) ? half : 0, column0 = (i & 2) ? half : 0;
		for (int r = 0; r < half; ++r)
			for (int c = 0; c < half; ++c) out.push_back(atlas[size_t(row0 + r) * size_t(width) + size_t(column0 + c)]);
	}
	out.resize(size_t(width) * size_t(height), 0);
	return out;
}

// Where the quadrant-major word `k` stands in the atlas (the inverse of quadrants()).
size_t atlas_index(size_t k, int width) {
	const size_t half = size_t(width >> 1), side = half * half;
	if (side == 0) return k;
	const size_t q = k / side, within = k % side;
	const size_t row = within / half + ((q & 1) ? half : 0), column = within % half + ((q & 2) ? half : 0);
	return row * size_t(width) + column;
}

// The range the brightness spreads over: the highest seeded at 0, the lowest at 0x7FFF [@ 0x100107ae..0x100107e3];
// 255 over the range, 1 where it is flat, held as a float [@ 0x100107e8..0x10010810].
struct Range {
	uint32_t lowest = 0x7FFF;
	float scale = 1.0f;
};
Range range_of(const std::vector<uint32_t> &words) {
	uint32_t highest = 0, lowest = 0x7FFF;
	for (const uint32_t word : words) {
		if (highest <= word) highest = word;
		if (lowest >= word) lowest = word;
	}
	Range out;
	out.lowest = lowest;
	out.scale = highest == lowest ? 1.0f : float(1.0 / (double(highest - lowest) * 0.0039215689));
	return out;
}

// A word's brightness: its height above the lowest (read unsigned) times the scale, truncated [@ 0x10010853..0x10010869].
uint32_t brightness(uint32_t word, const Range &range) {
	const uint32_t above = word - range.lowest;
	return uint32_t(uint64_t(double(above) * double(range.scale)));
}

} // namespace

void nile_height_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out) {
	out.assign(size_t(width) * size_t(height), kOpaque);
	if (!atlas || width <= 0 || height <= 0) return;
	const std::vector<uint32_t> words = quadrants(atlas, width, height);
	const Range range = range_of(words);
	for (size_t k = 0; k < words.size(); ++k) out[atlas_index(k, width)] = (brightness(words[k], range) << 8) | kOpaque;
}

void nile_delta_map(const uint16_t *atlas, int width, int height, std::vector<uint32_t> &out) {
	out.assign(size_t(width) * size_t(height), kOpaque);
	if (!atlas || width <= 0 || height <= 0) return;
	const std::vector<uint32_t> words = quadrants(atlas, width, height);
	const Range range = range_of(words);
	// The rows 1024 words apart, wrapped by (height << 10) - 1024; the columns by width - 1 [@ 0x10010935..0x100109be].
	const uint32_t rows = (uint32_t(height) << 10) - 1024u, columns = uint32_t(width) - 1u;
	const auto at = [&words](uint32_t index) { return index < words.size() ? words[index] : 0u; };
	for (uint32_t y = 0; y < uint32_t(height); ++y) {
		const uint32_t here = rows & (y << 10), south = rows & ((y + 1u) << 10), north = rows & ((y - 1u) << 10);
		for (uint32_t x = 0; x < uint32_t(width); ++x) {
			const uint32_t column = x & columns;
			const uint32_t word = at(column + here);
			const bool flat = word == at(column + south) && word == at(column + north) &&
			                  word == at(here + (columns & (x + 1u))) && word == at(here + (columns & (x - 1u)));
			const uint32_t lit = brightness(word, range);
			out[atlas_index(size_t(y) * size_t(width) + x, width)] = (flat ? lit << 8 : lit << 16) | kOpaque;
		}
	}
}

void med_flatness_map(const uint16_t *atlas, std::vector<uint32_t> &out) {
	constexpr uint32_t kSide = 1024;
	out.assign(size_t(kSide) * kSide, kOpaque);
	if (!atlas) return;
	// The grid as the original holds it (twice each depth word: equal where the words are), wrapped at 0x3FF; the
	// alpha byte its blit ignores set opaque.
	for (uint32_t y = 0; y < kSide; ++y)
		for (uint32_t x = 0; x < kSide; ++x) {
			const uint16_t word = atlas[y * kSide + x];
			const bool flat = word == atlas[((y + 1u) & 0x3FFu) * kSide + x] && word == atlas[((y - 1u) & 0x3FFu) * kSide + x] &&
			                  word == atlas[y * kSide + ((x + 1u) & 0x3FFu)] && word == atlas[y * kSide + ((x - 1u) & 0x3FFu)];
			out[size_t(y) * kSide + x] = (flat ? 0u : 0x404040u) | kOpaque;
		}
}

void med_water_overlay(const uint16_t *atlas, int32_t water_1616, std::vector<uint32_t> &pixels) {
	if (!atlas || pixels.size() < size_t(1024) * 1024) return;
	const uint32_t water = uint32_t(water_1616 >> 15);
	for (size_t i = 0; i < size_t(1024) * 1024; ++i) {
		const uint32_t byte = ((uint32_t(atlas[i]) * 2u) >> 8) & 0xFFu;
		if (byte >= water) continue;
		pixels[i] = ((((pixels[i] >> 1) & 0x7F7F7F7Fu) + 128u) & 0x00FFFFFFu) | kOpaque;
	}
}

void MissionMapTerrainView::follow(MissionMapBackground background, bool water, const uint16_t *atlas, uint64_t revision,
                                   int32_t water_1616) {
	const bool moved = !followed_ || background != background_ || water != water_ || atlas != atlas_ ||
	                   revision != revision_ || water_1616 != water_1616_;
	if (!moved) return;
	followed_ = true;
	background_ = background;
	water_ = water;
	atlas_ = atlas;
	revision_ = revision;
	water_1616_ = water_1616;
	picture_.clear();
	if (atlas) {
		switch (background) {
		case MissionMapBackground::Height: nile_height_map(atlas, 1024, 1024, picture_); break;
		case MissionMapBackground::Delta: nile_delta_map(atlas, 1024, 1024, picture_); break;
		case MissionMapBackground::Flatness: med_flatness_map(atlas, picture_); break;
		case MissionMapBackground::Colour:
		case MissionMapBackground::None:
		case MissionMapBackground::kCount: break;
		}
		if (water && water_1616 != 0 && !picture_.empty()) med_water_overlay(atlas, water_1616, picture_);
	}
	++serial_;
	++made_;
}

} // namespace opennova::editor
