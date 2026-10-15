#include <editor/preview/mission_map_terrain_view.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>

#include <formats/trn/charmap_legend.h>
#include <runtime/terrain_query/surface_type_map.h>

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

// A word's brightness: its height above the lowest (read unsigned) times the scale, truncated [@ 0x10010853..0x10010869];
// a word past a range kept over a stroke (S25) shown at its end.
uint32_t brightness(uint32_t word, const Range &range) {
	if (word < range.lowest) return 0;
	const uint32_t above = word - range.lowest;
	return std::min<uint32_t>(255u, uint32_t(uint64_t(double(above) * double(range.scale))));
}

// The same range over the atlas itself (the words alike, in another order).
Range range_of_atlas(const uint16_t *atlas, size_t count) {
	uint32_t highest = 0, lowest = 0x7FFF;
	for (size_t i = 0; i < count; ++i) {
		const uint32_t word = atlas[i];
		if (highest <= word) highest = word;
		if (lowest >= word) lowest = word;
	}
	Range out;
	out.lowest = lowest;
	out.scale = highest == lowest ? 1.0f : float(1.0 / (double(highest - lowest) * 0.0039215689));
	return out;
}

// Where the atlas word `index` stands in the quadrant-major copy (the inverse of atlas_index).
size_t quadrant_index(size_t index, int width) {
	const size_t half = size_t(width >> 1), side = half * half;
	if (side == 0) return index;
	const size_t row = index / size_t(width), column = index % size_t(width);
	const size_t q = (row >= half ? 1u : 0u) | (column >= half ? 2u : 0u);
	return q * side + (row % half) * half + column % half;
}

// One Delta pixel: the quadrant-major word `k` against Nile's four neighbours of it (nile_delta_map's walk).
uint32_t delta_pixel(const uint16_t *atlas, int width, int height, size_t k, const Range &range) {
	const uint32_t rows = (uint32_t(height) << 10) - 1024u, columns = uint32_t(width) - 1u;
	const size_t count = size_t(width) * size_t(height);
	const auto word_at = [&](uint32_t index) { return index < count ? uint32_t(atlas[atlas_index(index, width)]) : 0u; };
	const uint32_t y = uint32_t(k) >> 10, x = uint32_t(k) & 1023u;
	const uint32_t here = rows & (y << 10), south = rows & ((y + 1u) << 10), north = rows & ((y - 1u) << 10);
	const uint32_t column = x & columns;
	const uint32_t word = word_at(column + here);
	const bool flat = word == word_at(column + south) && word == word_at(column + north) &&
	                  word == word_at(here + (columns & (x + 1u))) && word == word_at(here + (columns & (x - 1u)));
	const uint32_t lit = brightness(word, range);
	return (flat ? lit << 8 : lit << 16) | kOpaque;
}

// One flatness pixel at atlas (x, y) (med_flatness_map's).
uint32_t flatness_pixel(const uint16_t *atlas, uint32_t x, uint32_t y) {
	constexpr uint32_t kSide = 1024;
	const uint16_t word = atlas[y * kSide + x];
	const bool flat = word == atlas[((y + 1u) & 0x3FFu) * kSide + x] && word == atlas[((y - 1u) & 0x3FFu) * kSide + x] &&
	                  word == atlas[y * kSide + ((x + 1u) & 0x3FFu)] && word == atlas[y * kSide + ((x - 1u) & 0x3FFu)];
	return (flat ? 0u : 0x404040u) | kOpaque;
}

// The water's tint over one pixel (med_water_overlay's).
uint32_t watered(uint16_t word, uint32_t water, uint32_t pixel) {
	const uint32_t byte = ((uint32_t(word) * 2u) >> 8) & 0xFFu;
	return byte >= water ? pixel : ((((pixel >> 1) & 0x7F7F7F7Fu) + 128u) & 0x00FFFFFFu) | kOpaque;
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

namespace {

// A class's legend colour as a picture word.
uint32_t legend_pixel(int surface) {
	const CharmapLegendColour &c = surface >= 0 && surface < kCharmapLegendCount ? kCharmapLegend[surface] : kCharmapLegendRest;
	return kOpaque | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | uint32_t(c.b);
}

// The class the char map sampler reads at atlas texel (x, z) [terrain::surface_type_at_fixed: the texel shifted down
// by the sample shift, class 1 past the map].
int surface_class_at(const uint8_t *classes, int width, int height, int shift, int x, int z) {
	if (!classes || width <= 0 || height <= 0) return 1;
	const int sx = x >> shift, sz = z >> shift;
	if (sx < 0 || sx >= width || sz < 0 || sz >= height) return 1;
	return classes[size_t(width) * size_t(sz) + size_t(sx)];
}

} // namespace

void surface_class_map(const uint8_t *classes, int width, int height, std::vector<uint32_t> &out) {
	constexpr int kSide = 1024;
	out.assign(size_t(kSide) * kSide, 0);
	const int shift = terrain::detail::surface_sample_shift(width);
	for (int z = 0; z < kSide; ++z)
		for (int x = 0; x < kSide; ++x)
			out[size_t(z) * kSide + size_t(x)] = legend_pixel(surface_class_at(classes, width, height, shift, x, z));
}

MissionMapAtlasRect surface_atlas_rect(int width, int x, int y, int region_width, int region_height) {
	constexpr int kSide = 1024;
	if (width <= 0 || width > kSide) return {0, 0, kSide, kSide};
	const int shift = terrain::detail::surface_sample_shift(width);
	return {std::clamp(x << shift, 0, kSide), std::clamp(y << shift, 0, kSide), std::clamp((x + region_width) << shift, 0, kSide),
	        std::clamp((y + region_height) << shift, 0, kSide)};
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
                                   int32_t water_1616, const MissionMapSurface &surface, bool terrain) {
	const bool moved = !followed_ || background != background_ || water != water_ || atlas != atlas_ ||
	                   revision != revision_ || water_1616 != water_1616_ || surface != surface_ || terrain != terrain_;
	if (!moved) return;
	followed_ = true;
	background_ = background;
	water_ = water;
	atlas_ = atlas;
	revision_ = revision;
	water_1616_ = water_1616;
	surface_ = surface;
	terrain_ = terrain;
	make_();
}

void MissionMapTerrainView::patch_surface(const MissionMapSurface &surface, const std::vector<MissionMapAtlasRect> &regions) {
	if (!followed_ || background_ != MissionMapBackground::Surface || picture_.size() != size_t(1024) * 1024 ||
	    surface.classes != surface_.classes || surface.width != surface_.width || surface.height != surface_.height)
		return;
	surface_ = surface;
	if (regions.empty()) return;
	constexpr int kSide = 1024;
	const int shift = terrain::detail::surface_sample_shift(surface.width);
	for (const MissionMapAtlasRect &rect : regions) {
		const int x0 = std::max(rect.x0, 0), y0 = std::max(rect.y0, 0), x1 = std::min(rect.x1, kSide), y1 = std::min(rect.y1, kSide);
		if (x0 >= x1 || y0 >= y1) continue;
		for (int z = y0; z < y1; ++z)
			for (int x = x0; x < x1; ++x)
				picture_[size_t(z) * kSide + size_t(x)] =
						legend_pixel(surface_class_at(surface.classes, surface.width, surface.height, shift, x, z));
		dirty_.push_back({x0, y0, x1, y1});
	}
	++serial_;
	++patched_;
}

void MissionMapTerrainView::make_() {
	picture_.clear();
	// The surface classes: the char map's, over a terrain read whatever its heights.
	if (background_ == MissionMapBackground::Surface && terrain_)
		surface_class_map(surface_.classes, surface_.width, surface_.height, picture_);
	if (atlas_) {
		switch (background_) {
		case MissionMapBackground::Height: nile_height_map(atlas_, 1024, 1024, picture_); break;
		case MissionMapBackground::Delta: nile_delta_map(atlas_, 1024, 1024, picture_); break;
		case MissionMapBackground::Flatness: med_flatness_map(atlas_, picture_); break;
		case MissionMapBackground::Colour:
		case MissionMapBackground::Surface:
		case MissionMapBackground::None:
		case MissionMapBackground::kCount: break;
		}
		if (water_ && water_1616_ != 0 && !picture_.empty()) med_water_overlay(atlas_, water_1616_, picture_);
		if (background_ == MissionMapBackground::Height || background_ == MissionMapBackground::Delta) {
			const Range range = range_of_atlas(atlas_, size_t(1024) * 1024);
			lowest_ = range.lowest;
			scale_ = range.scale;
		}
	}
	stale_ = false;
	dirty_.clear();
	++serial_;
	whole_serial_ = serial_;
	++made_;
}

void MissionMapTerrainView::settle(const uint16_t *atlas) {
	if (!followed_ || atlas != atlas_ || !stale_) return;
	stale_ = false;
	if (!atlas || picture_.size() != size_t(1024) * 1024) return;
	if (background_ != MissionMapBackground::Height && background_ != MissionMapBackground::Delta) return;
	const Range range = range_of_atlas(atlas, size_t(1024) * 1024);
	if (range.lowest != lowest_ || range.scale != scale_) make_();
}

void MissionMapTerrainView::patch(const uint16_t *atlas, const std::vector<MissionMapAtlasRect> &regions, uint64_t revision) {
	if (!followed_ || atlas != atlas_) return;
	revision_ = revision;
	if (!atlas || picture_.size() != size_t(1024) * 1024 || regions.empty()) return;
	constexpr int kSide = 1024;
	// Height and Delta scale by the heights' lowest and highest: kept as the picture was made over them while a stroke
	// patches it (settle takes them again at its end).
	Range range;
	range.lowest = lowest_;
	range.scale = scale_;
	if (background_ == MissionMapBackground::Height || background_ == MissionMapBackground::Delta) stale_ = true;
	const uint32_t water = water_ && water_1616_ != 0 ? uint32_t(water_1616_ >> 15) : 0u;
	const auto set = [&](size_t index, uint32_t pixel) {
		picture_[index] = water ? watered(atlas[index], water, pixel) : pixel;
	};
	for (const MissionMapAtlasRect &rect : regions) {
		const int x0 = std::max(rect.x0, 0), y0 = std::max(rect.y0, 0), x1 = std::min(rect.x1, kSide), y1 = std::min(rect.y1, kSide);
		if (x0 >= x1 || y0 >= y1) continue;
		switch (background_) {
		case MissionMapBackground::Height:
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x) {
					const size_t index = size_t(y) * kSide + size_t(x);
					set(index, (brightness(atlas[index], range) << 8) | kOpaque);
				}
			break;
		case MissionMapBackground::Delta:
			// Each changed texel's word and the four whose neighbour it is, over the quadrant-major copy.
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x) {
					const size_t k = quadrant_index(size_t(y) * kSide + size_t(x), kSide);
					const uint32_t ky = uint32_t(k) >> 10, kx = uint32_t(k) & 1023u;
					const uint32_t around[5][2] = {{kx, ky}, {(kx + 1u) & 1023u, ky}, {(kx - 1u) & 1023u, ky},
					                               {kx, (ky + 1u) & 1023u}, {kx, (ky - 1u) & 1023u}};
					for (const auto &at : around) {
						const size_t each = size_t(at[1]) * kSide + at[0];
						set(atlas_index(each, kSide), delta_pixel(atlas, kSide, kSide, each, range));
					}
				}
			break;
		case MissionMapBackground::Flatness:
			// The region and a texel around it, wrapped (each texel reads its four neighbours).
			for (int y = y0 - 1; y <= y1; ++y)
				for (int x = x0 - 1; x <= x1; ++x) {
					const uint32_t wx = uint32_t(x) & 0x3FFu, wy = uint32_t(y) & 0x3FFu;
					set(size_t(wy) * kSide + wx, flatness_pixel(atlas, wx, wy));
				}
			break;
		case MissionMapBackground::Colour:
		case MissionMapBackground::Surface:
		case MissionMapBackground::None:
		case MissionMapBackground::kCount: return;
		}
		// The pixels it set: the rect grown by a texel (flatness's ring, Delta's neighbours), clamped; where those wrap
		// (flatness at the atlas's edge, Delta at a quadrant's: its neighbours lie over the quadrant-major copy), the
		// whole picture.
		const auto touches = [](int from, int to, std::initializer_list<int> edges) {
			for (const int edge : edges)
				if (edge >= from && edge < to) return true;
			return false;
		};
		const bool wraps = background_ == MissionMapBackground::Flatness
		                           ? touches(x0, x1, {0, kSide - 1}) || touches(y0, y1, {0, kSide - 1})
		                           : background_ == MissionMapBackground::Delta &&
		                                     (touches(x0, x1, {0, 511, 512, kSide - 1}) || touches(y0, y1, {0, 511, 512, kSide - 1}));
		if (wraps) dirty_.push_back({0, 0, kSide, kSide});
		else dirty_.push_back({std::max(x0 - 1, 0), std::max(y0 - 1, 0), std::min(x1 + 1, kSide), std::min(y1 + 1, kSide)});
	}
	++serial_;
	++patched_;
}

} // namespace opennova::editor
