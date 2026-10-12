#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace opennova::editor {

// The texel layouts a raster's plane holds (ADR 0046 S25, the raster core): a byte (a char map's class, a
// foliage code, a palette index, a grey), a 16-bit value (a terrain's raw16 heights), and colour in three or
// four bytes, red first.
enum class RasterFormat : uint8_t { U8, U16, Rgb8, Rgba8 };
int raster_texel_bytes(RasterFormat format);

// A plane's tiles are this many texels a side.
inline constexpr int kRasterTileSide = 64;

// One tile's texels, kRasterTileSide rows of kRasterTileSide, row-major; an edge tile's texels past the plane
// are zero and read by nothing.
struct RasterTile {
	std::vector<uint8_t> bytes;
};

// A plane of texels held in tiles shared copy-on-write: a copy of the plane (a snapshot, a history step's
// state) shares every tile, and a write (RasterWriter) copies only the tiles it touches. Tiles are numbered
// row by row, `tiles_across()` a row.
class RasterPlane {
public:
	RasterPlane() = default;
	// A plane of zeros (every tile the one zero tile).
	RasterPlane(RasterFormat format, int width, int height);
	// A plane of `texels`: width x height texels row-major, the top row first, raster_texel_bytes each.
	static RasterPlane from_bytes(RasterFormat format, int width, int height, const uint8_t *texels);

	RasterFormat format() const { return format_; }
	int width() const { return width_; }
	int height() const { return height_; }
	bool empty() const { return width_ <= 0 || height_ <= 0; }
	int texel_bytes() const { return raster_texel_bytes(format_); }
	int tiles_across() const { return (width_ + kRasterTileSide - 1) / kRasterTileSide; }
	int tiles_down() const { return (height_ + kRasterTileSide - 1) / kRasterTileSide; }
	int tile_count() const { return tiles_across() * tiles_down(); }
	// The tile holding texel (x, y), and a tile's first texel.
	int tile_of(int x, int y) const { return (y / kRasterTileSide) * tiles_across() + x / kRasterTileSide; }
	// A tile's bytes (kRasterTileSide squared texels).
	size_t tile_bytes() const;

	// Texel (x, y), inside the plane: its bytes, and a one-byte or 16-bit plane's value.
	const uint8_t *texel(int x, int y) const;
	uint8_t u8(int x, int y) const { return *texel(x, y); }
	uint16_t u16(int x, int y) const;
	// The plane's texels row-major, the top row first (what from_bytes takes).
	std::vector<uint8_t> bytes() const;
	// A rectangle's texels row-major into `out` (w x h x texel_bytes bytes), inside the plane.
	void copy_rect(int x, int y, int w, int h, uint8_t *out) const;

	const std::shared_ptr<const RasterTile> &tile(int index) const { return tiles_[static_cast<size_t>(index)]; }
	// A tile put in place whole (the history's undo and redo).
	void set_tile(int index, std::shared_ptr<const RasterTile> tile) { tiles_[static_cast<size_t>(index)] = std::move(tile); }
	// Whether `other` holds the same texels in the same tiles (shared, not compared).
	bool shares_tiles_with(const RasterPlane &other) const;
	// Whether `other` holds the same texels (compared).
	bool same_texels(const RasterPlane &other) const;

private:
	RasterFormat format_ = RasterFormat::U8;
	int width_ = 0, height_ = 0;
	std::vector<std::shared_ptr<const RasterTile>> tiles_;
};

// One tile of one plane swapped: what the history keeps and undoes (`before`) or redoes (`after`).
struct TileSwap {
	int plane = 0;
	int tile = 0;
	std::shared_ptr<const RasterTile> before, after;
};

// Writes into one plane through copy-on-write: the first write into a tile gives the plane a copy of it, and
// finish() names every tile so written with what it held before and holds after. Texels are inside the plane.
class RasterWriter {
public:
	RasterWriter(RasterPlane &plane, int plane_index) : plane_(plane), index_(plane_index) {}
	RasterWriter(const RasterWriter &) = delete;
	RasterWriter &operator=(const RasterWriter &) = delete;

	// Texel (x, y)'s bytes to write (valid while the writer lives).
	uint8_t *texel(int x, int y);
	void set_u8(int x, int y, uint8_t value) { *texel(x, y) = value; }
	void set_u16(int x, int y, uint16_t value);
	const RasterPlane &plane() const { return plane_; }
	// The tiles written, each once, in the order first written; the writer then holds none.
	std::vector<TileSwap> finish();

private:
	RasterPlane &plane_;
	int index_ = 0;
	std::map<int, std::shared_ptr<RasterTile>> copies_; // tile -> its writable copy
	std::vector<TileSwap> swaps_;
};

} // namespace opennova::editor
