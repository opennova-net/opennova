#include "raster_plane.h"

#include <algorithm>
#include <cstring>

namespace opennova::editor {

namespace {

// The one zero tile of each texel size, which a plane of zeros shares.
std::shared_ptr<const RasterTile> zero_tile(size_t bytes) {
	auto tile = std::make_shared<RasterTile>();
	tile->bytes.assign(bytes, 0);
	return tile;
}

} // namespace

int raster_texel_bytes(RasterFormat format) {
	switch (format) {
	case RasterFormat::U8: return 1;
	case RasterFormat::U16: return 2;
	case RasterFormat::Rgb8: return 3;
	case RasterFormat::Rgba8: return 4;
	}
	return 1;
}

RasterPlane::RasterPlane(RasterFormat format, int width, int height)
		: format_(format), width_(std::max(0, width)), height_(std::max(0, height)) {
	const std::shared_ptr<const RasterTile> zero = zero_tile(tile_bytes());
	tiles_.assign(static_cast<size_t>(tile_count()), zero);
}

size_t RasterPlane::tile_bytes() const {
	return static_cast<size_t>(kRasterTileSide) * kRasterTileSide * static_cast<size_t>(texel_bytes());
}

RasterPlane RasterPlane::from_bytes(RasterFormat format, int width, int height, const uint8_t *texels) {
	RasterPlane plane(format, width, height);
	const size_t texel = static_cast<size_t>(plane.texel_bytes());
	const size_t row_bytes = static_cast<size_t>(kRasterTileSide) * texel;
	for (int ty = 0; ty < plane.tiles_down(); ++ty)
		for (int tx = 0; tx < plane.tiles_across(); ++tx) {
			auto tile = std::make_shared<RasterTile>();
			tile->bytes.assign(plane.tile_bytes(), 0);
			const int x0 = tx * kRasterTileSide, y0 = ty * kRasterTileSide;
			const int w = std::min(kRasterTileSide, plane.width_ - x0), h = std::min(kRasterTileSide, plane.height_ - y0);
			for (int row = 0; row < h; ++row)
				std::memcpy(tile->bytes.data() + static_cast<size_t>(row) * row_bytes,
				            texels + (static_cast<size_t>(y0 + row) * static_cast<size_t>(plane.width_) + x0) * texel,
				            static_cast<size_t>(w) * texel);
			plane.tiles_[static_cast<size_t>(ty * plane.tiles_across() + tx)] = std::move(tile);
		}
	return plane;
}

const uint8_t *RasterPlane::texel(int x, int y) const {
	const RasterTile &tile = *tiles_[static_cast<size_t>(tile_of(x, y))];
	const size_t at = static_cast<size_t>((y % kRasterTileSide) * kRasterTileSide + x % kRasterTileSide);
	return tile.bytes.data() + at * static_cast<size_t>(texel_bytes());
}

uint16_t RasterPlane::u16(int x, int y) const {
	uint16_t value = 0;
	std::memcpy(&value, texel(x, y), sizeof(value));
	return value;
}

void RasterPlane::copy_rect(int x, int y, int w, int h, uint8_t *out) const {
	const size_t texel = static_cast<size_t>(texel_bytes());
	for (int row = 0; row < h; ++row)
		for (int col = 0; col < w;) {
			// A run inside one tile at a time.
			const int px = x + col, py = y + row;
			const int run = std::min(w - col, kRasterTileSide - px % kRasterTileSide);
			std::memcpy(out + (static_cast<size_t>(row) * static_cast<size_t>(w) + static_cast<size_t>(col)) * texel,
			            this->texel(px, py), static_cast<size_t>(run) * texel);
			col += run;
		}
}

std::vector<uint8_t> RasterPlane::bytes() const {
	std::vector<uint8_t> out(static_cast<size_t>(width_) * static_cast<size_t>(height_) *
	                         static_cast<size_t>(texel_bytes()));
	if (!empty()) copy_rect(0, 0, width_, height_, out.data());
	return out;
}

bool RasterPlane::shares_tiles_with(const RasterPlane &other) const {
	return format_ == other.format_ && width_ == other.width_ && height_ == other.height_ && tiles_ == other.tiles_;
}

bool RasterPlane::same_texels(const RasterPlane &other) const {
	if (format_ != other.format_ || width_ != other.width_ || height_ != other.height_) return false;
	for (size_t i = 0; i < tiles_.size(); ++i)
		if (tiles_[i] != other.tiles_[i] && tiles_[i]->bytes != other.tiles_[i]->bytes) return false;
	return true;
}

uint8_t *RasterWriter::texel(int x, int y) {
	const int index = plane_.tile_of(x, y);
	auto found = copies_.find(index);
	if (found == copies_.end()) {
		const std::shared_ptr<const RasterTile> &before = plane_.tile(index);
		auto copy = std::make_shared<RasterTile>(*before);
		swaps_.push_back({index_, index, before, nullptr});
		plane_.set_tile(index, copy);
		found = copies_.emplace(index, std::move(copy)).first;
	}
	const size_t at = static_cast<size_t>((y % kRasterTileSide) * kRasterTileSide + x % kRasterTileSide);
	return found->second->bytes.data() + at * static_cast<size_t>(plane_.texel_bytes());
}

void RasterWriter::set_u16(int x, int y, uint16_t value) { std::memcpy(texel(x, y), &value, sizeof(value)); }

void RasterWriter::set_rect_u16(int x, int y, int w, int h, const uint16_t *src, size_t stride) {
	for (int row = 0; row < h; ++row)
		for (int col = 0; col < w;) {
			// The run of the row inside one tile: its texels lie side by side.
			const int run = std::min(w - col, kRasterTileSide - (x + col) % kRasterTileSide);
			std::memcpy(texel(x + col, y + row), src + size_t(row) * stride + size_t(col), size_t(run) * sizeof(uint16_t));
			col += run;
		}
}

std::vector<TileSwap> RasterWriter::finish() {
	for (TileSwap &swap : swaps_) swap.after = copies_.at(swap.tile);
	copies_.clear();
	std::vector<TileSwap> out = std::move(swaps_);
	swaps_.clear();
	return out;
}

} // namespace opennova::editor
