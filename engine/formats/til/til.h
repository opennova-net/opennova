#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace opennova {

// [orig: jodemo Terrain_DrawTileOverlays2D @0x5C79C0, sub_5C42B0 @0x5C42B0, Terrain_RenderSectorTile @0x5CDAA0]
// [orig: PolyTrn_RenderTile @0x60df0d -> PolyTrn_DrawTileOverlayQuad @0x604700; docs/tiles/til-re.md]

constexpr uint32_t TIL_MAGIC = 0x74696C30u;

constexpr uint8_t TIL_FLAG_FLIP_X = 0x01u;
constexpr uint8_t TIL_FLAG_FLIP_Y = 0x02u;
constexpr uint8_t TIL_FLAG_ROTATE_90 = 0x04u;
// Bit 0x08 - the original editors' selection mark. Nile toggles it on the tile a click hits and sets
// or clears it on every tile a drag box crosses [orig: gobj.dll sub_10013364 @ 0x1001340C (xor 8),
// sub_10013416 @ 0x100134F7 / 0x100134FC], and jodemo.exe draws a LINELIST outline around a marked
// tile (Terrain_DrawTileOverlays2D@0x5C79C0). RETAIL JO does NOT: its tile overlay render
// (PolyTrn_DrawTileOverlayQuad@0x604700, via PolyTrn_RenderTile@0x60df0d) tests only bits 0/1/2 and
// draws a single TRIANGLESTRIP - no outline. No shipped .til marks a tile. The flags byte is carried
// whole (every bit a file holds is written back); like retail JO the port renders no outline. See
// docs/tiles/til-re.md (D-TIL-1, D-TIL-6).
constexpr uint8_t TIL_FLAG_OUTLINE = 0x08u;

constexpr int TIL_ATLAS_TILE_PIXELS = 64;
constexpr int TIL_CELL_WORLD_UNITS = 16;
constexpr int TIL_FIXED_SHIFT = 16;
constexpr int32_t TIL_FIXED_UNITS = 1 << TIL_FIXED_SHIFT;
constexpr int32_t TIL_FIXED_CELL_UNITS = TIL_CELL_WORLD_UNITS << TIL_FIXED_SHIFT;

struct TilCellCoord {
	int x = 0;
	int z = 0;
};

struct TilUv {
	float u = 0.0f;
	float v = 0.0f;
};

struct TilUvQuad {
	std::array<TilUv, 4> corners{};
	bool valid = false;
};

struct TilAtlasLayout {
	int tiles_x = 0;
	int tiles_y = 0;
	float step_u = 0.0f;
	float step_v = 0.0f;
};

// One 12-byte tile record. The game reads +0 x, +4 z (stored negated), +8 the atlas cell and +9 the
// flags [orig: PolyTrn_RenderTile @ 0x60DE20..0x60DF09; Terrain_GetSurfaceTypeAtPosition @ 0x6065CC..
// 0x60660C]; +10 is MED's u16 layer (its sort order), written by the editor and never read by the
// game [orig: JOTACmed MapTileStore_Append @ 0x433BBF; MapTileStore_Serialize @ 0x433F88 writes the
// records grouped by ascending layer]. Every byte is a field, so a record writes back as it was read.
struct TilOverlayEntry {
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
	uint8_t tile_index = 0;
	uint8_t flags = 0;
	uint16_t layer = 0;
};

// A .til: the records in file order, which is the game's draw order. The header is computed on save
// (til_io.h), never carried.
struct TilFile {
	std::vector<TilOverlayEntry> entries;

	bool empty() const { return entries.empty(); }
};

inline int til_cell_from_world(double world_units) {
	return static_cast<int>(std::floor(world_units / static_cast<double>(TIL_CELL_WORLD_UNITS)));
}

inline int til_cell_x_from_world(double world_x) {
	return til_cell_from_world(world_x);
}

inline int til_cell_z_from_world(double world_z) {
	return til_cell_from_world(world_z);
}

inline TilAtlasLayout til_make_atlas_layout(int atlas_width, int atlas_height) {
	TilAtlasLayout layout;
	if (atlas_width < TIL_ATLAS_TILE_PIXELS || atlas_height < TIL_ATLAS_TILE_PIXELS) {
		return layout;
	}
	layout.tiles_x = atlas_width / TIL_ATLAS_TILE_PIXELS;
	layout.tiles_y = atlas_height / TIL_ATLAS_TILE_PIXELS;
	if (layout.tiles_x <= 0 || layout.tiles_y <= 0) {
		layout = TilAtlasLayout{};
		return layout;
	}
	layout.step_u = 1.0f / static_cast<float>(layout.tiles_x);
	layout.step_v = 1.0f / static_cast<float>(layout.tiles_y);
	return layout;
}

inline TilUv til_transform_local_uv(TilUv uv, uint8_t flags) {
	// [orig: PolyTrn_DrawTileOverlayQuad @ 0x604700 — flag blocks @ 0x604782 (U swap),
	// 0x6047a9 (V swap), 0x6047d4 (rotate)]. Retail's rotate permutes the four
	// corner UVs as NW<-NE, NE<-SE, SE<-SW, SW<-NW (corner cycle
	// A<-B, B<-D, D<-C, C<-A @ 0x6047d4..0x604806): per corner (u,v) that is
	// (1-v, u), a 90-degree CCW image rotation. The prior (v, 1-u) reading was
	// the CW transpose — every ROTATE_90 tile rendered 180 degrees off
	// (docs/tiles/til-re.md D-TIL-2, FIXED).
	//
	// Composition order (docs/tiles/til-re.md D-TIL-4): retail mirrors the
	// CORNER UV variables
	// first (bit 0 @ 0x604782, bit 1 @ 0x6047a9) and only then applies the
	// rotate as a corner-ASSIGNMENT cycle over those already-mirrored values
	// (@ 0x6047d4). Permuting corner assignments is the inverse mapping of
	// transforming the sampling function, so in sampling-function form the
	// faithful order is ROTATE FIRST, THEN FLIPS. The two orders agree for
	// pure rotate (0x04), pure flips, and rotate+both flips (0x07), but for
	// rotate plus exactly one flip (0x05/0x06) flip-then-rotate renders the
	// art 180 degrees off — witnessed against the 00TRa driving-course fork
	// (entries 731/746/761/781) that retail draws through the fixture camera.
	if (flags & TIL_FLAG_ROTATE_90) {
		uv = TilUv{1.0f - uv.v, uv.u};
	}
	if (flags & TIL_FLAG_FLIP_X) {
		uv.u = 1.0f - uv.u;
	}
	if (flags & TIL_FLAG_FLIP_Y) {
		uv.v = 1.0f - uv.v;
	}
	return uv;
}

inline TilUvQuad til_build_entry_uv_quad(uint8_t tile_index,
                                         uint8_t flags,
                                         int atlas_width,
                                         int atlas_height) {
	TilUvQuad quad;
	const TilAtlasLayout layout = til_make_atlas_layout(atlas_width, atlas_height);
	if (layout.tiles_x <= 0 || layout.tiles_y <= 0) {
		return quad;
	}

	// The cell is the index's column and row with no bound on the row: an index past the atlas's
	// cells lies below it, and the atlas's CLAMP addressing draws that quad from the atlas's last
	// texel row [orig: PolyTrn_RenderTile @ 0x60DEBF..0x60DF05 (idiv by g_TerrainTileSetTilesX, the
	// row times flt_319F7C4); Terrain_LoadTileSetAtlas @ 0x604B4E (flags 0x100203: CLAMP) and
	// @ 0x604B8A..0x604BC2 (tiles = width / 64, height / 64)]. docs/tiles/til-re.md D-TIL-7.
	const int tile_col = tile_index % layout.tiles_x;
	const int tile_row = tile_index / layout.tiles_x;

	const float origin_u = static_cast<float>(tile_col) * layout.step_u;
	const float origin_v = static_cast<float>(tile_row) * layout.step_v;
	const std::array<TilUv, 4> local = {
	    til_transform_local_uv(TilUv{0.0f, 0.0f}, flags),
	    til_transform_local_uv(TilUv{1.0f, 0.0f}, flags),
	    til_transform_local_uv(TilUv{0.0f, 1.0f}, flags),
	    til_transform_local_uv(TilUv{1.0f, 1.0f}, flags),
	};
	for (size_t i = 0; i < local.size(); ++i) {
		quad.corners[i] = TilUv{
		    origin_u + local[i].u * layout.step_u,
		    origin_v + local[i].v * layout.step_v,
		};
	}
	quad.valid = true;
	return quad;
}

inline TilUvQuad til_build_entry_render_uv_quad(uint8_t tile_index,
                                                uint8_t flags,
                                                int atlas_width,
                                                int atlas_height) {
	TilUvQuad quad = til_build_entry_uv_quad(tile_index, flags, atlas_width, atlas_height);
	if (!quad.valid || atlas_width <= 0 || atlas_height <= 0) {
		return quad;
	}

	// [orig: PolyTrn_DrawTileOverlayQuad @0x604700 — u += +-0.5*flt_319F7C8, v += +-0.5*flt_319F7CC]
	// (jodemo sub_5C42B0 @0x5C42B0.)
	// The in-world sector pass shifts the already-transformed UV quad by a
	// D3D half-texel in the active texture direction before drawing the
	// TRIANGLESTRIP. This is separate from the authored atlas UVs above.
	const float half_u = 0.5f / static_cast<float>(atlas_width);
	const float half_v = 0.5f / static_cast<float>(atlas_height);
	const TilUv &tl = quad.corners[0];
	const float sign_u = (tl.u > quad.corners[1].u || tl.u > quad.corners[2].u) ? -1.0f : 1.0f;
	const float sign_v = (tl.v > quad.corners[2].v || tl.v > quad.corners[1].v) ? -1.0f : 1.0f;
	for (TilUv &uv : quad.corners) {
		uv.u += sign_u * half_u;
		uv.v += sign_v * half_v;
	}
	return quad;
}

inline float til_world_x_from_fixed(int32_t x_fixed) {
	return static_cast<float>(x_fixed) / static_cast<float>(TIL_FIXED_UNITS);
}

inline float til_world_z_from_fixed(int32_t z_fixed) {
	return -static_cast<float>(z_fixed) / static_cast<float>(TIL_FIXED_UNITS);
}

inline int32_t til_x_fixed_from_world(double world_x) {
	return static_cast<int32_t>(std::llround(world_x * static_cast<double>(TIL_FIXED_UNITS)));
}

inline int32_t til_z_fixed_from_world(double world_z) {
	return -static_cast<int32_t>(std::llround(world_z * static_cast<double>(TIL_FIXED_UNITS)));
}

inline int til_floor_div_fixed(int32_t value, int32_t divisor) {
	if (divisor <= 0) {
		return 0;
	}

	int32_t quotient = value / divisor;
	const int32_t remainder = value % divisor;
	if (remainder != 0 && value < 0) {
		--quotient;
	}
	return static_cast<int>(quotient);
}

inline int til_cell_x_from_fixed(int32_t x_fixed) {
	return til_floor_div_fixed(x_fixed, TIL_FIXED_CELL_UNITS);
}

inline int til_cell_z_from_fixed(int32_t z_fixed) {
	return til_floor_div_fixed(-z_fixed, TIL_FIXED_CELL_UNITS);
}

inline int32_t til_x_fixed_from_cell(int cell_x) {
	return static_cast<int32_t>(static_cast<int64_t>(cell_x) * static_cast<int64_t>(TIL_FIXED_CELL_UNITS));
}

inline int32_t til_z_fixed_from_cell(int cell_z) {
	return -static_cast<int32_t>(static_cast<int64_t>(cell_z) * static_cast<int64_t>(TIL_FIXED_CELL_UNITS));
}

inline TilCellCoord til_cell_from_fixed(int32_t x_fixed, int32_t z_fixed) {
	return {til_cell_x_from_fixed(x_fixed), til_cell_z_from_fixed(z_fixed)};
}

inline TilCellCoord til_entry_cell(const TilOverlayEntry &entry) {
	return til_cell_from_fixed(entry.x_fixed, entry.z_fixed);
}

inline bool til_entry_matches_cell(const TilOverlayEntry &entry, int cell_x, int cell_z) {
	const TilCellCoord cell = til_entry_cell(entry);
	return cell.x == cell_x && cell.z == cell_z;
}

// Tests the candidate's square footprint against the mission tile array's
// inclusive 16x16 world AABBs. Runtime foliage supplies radius 2.0.
// [orig: Foliage_PathBlockedByPlacedTile @ 0x606490;
// Terrain_LoadTileInfoFile @ 0x60a740]
bool til_blocks_foliage(const TilFile &file, float world_x, float world_z, float radius);

// A record at a 16.16 position, whole units or not (no snap: a tile goes where it is put, MED's and
// Nile's tools moving it by whole units), with its flags byte and layer as given.
inline TilOverlayEntry make_til_entry(int32_t x_fixed,
                                      int32_t z_fixed,
                                      uint8_t tile_index,
                                      uint8_t flags,
                                      uint16_t layer) {
	TilOverlayEntry entry;
	entry.x_fixed = x_fixed;
	entry.z_fixed = z_fixed;
	entry.tile_index = tile_index;
	entry.flags = flags;
	entry.layer = layer;
	return entry;
}

// A record on the 16-unit grid (the cell's corner), layer 0.
inline TilOverlayEntry make_til_overlay_entry(int cell_x,
	                                          int cell_z,
	                                          uint8_t tile_index,
	                                          uint8_t flags) {
	return make_til_entry(til_x_fixed_from_cell(cell_x), til_z_fixed_from_cell(cell_z), tile_index, flags, 0);
}

} // namespace opennova
