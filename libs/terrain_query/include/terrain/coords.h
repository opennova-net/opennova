#pragma once

// Terrain world -> source/atlas coordinate transforms.
//
// One implementation, shared by the runtime height/foliage samplers and the
// editor brush/eyedropper paths. The two callers differ only in three lookup
// guards (not in the data or the core math), expressed here as explicit options
// so the divergence is auditable rather than forked:
//
//   * resolve_world_sample (runtime, jodemo.exe Terrain_SampleHeightBilinear
//     @0x5C6770 / Terrain_GetFoliageMapValue @0x5C65E0): wraps the grid index
//     with & 0xF, uses the raw sector id, and does not clamp the local offset.
//   * EditorTerrainMesh.world_to_source_coords (editor): rejects cells outside
//     the authored rows/cols, clamps the sector id to [0,4], and clamps the
//     local offset to [0, 512 - 0.001].
//
// GDScript computes the editor path in 64-bit double then stores a float32
// Vector2; the runtime path is float throughout. The kernel is therefore
// templated on the precision so each caller keeps its native rounding:
// instantiate <double> for the editor, <float> for the runtime.
//
// Godot-agnostic: raw int grid pointer + scalars only. The GDExtension wrapper
// (NovaTerrainData) converts to/from Vector2 / Rect2i at the boundary.

#include <cmath>

namespace opennova::terrain {

constexpr int COORDS_SECTOR_SIZE = 512;
constexpr int COORDS_SECTOR_GRID_DIM = 16;
// The 1024 source atlas: a 2x2 quadrant grid of 512 sectors (each authored cell
// samples a 512x512 window at its quadrant offset; see coords_cell_atlas_rect).
constexpr int COORDS_ATLAS_SIZE = 2 * COORDS_SECTOR_SIZE;
// Sector ids are 0 (empty) .. 4 (the four quadrants).
constexpr int COORDS_SECTOR_ID_MAX = 4;

// The 16x16 sector grid plus its placement. `sector_grid` points at
// COORDS_SECTOR_GRID_DIM^2 ints, row-major (index = row * 16 + col), holding the
// per-cell sector id (0 = empty, 1..4 = quadrant). `sector_count`/`sector_rows`
// are the authored extent used only by the editor bounds-reject guard.
struct SectorLayout {
	const int *sector_grid = nullptr;
	int origin_x = 0;
	int origin_y = 0;
	int sector_count = 0;
	int sector_rows = 0;
};

// The three editor-vs-runtime guard differences. coords_runtime_options() and
// coords_editor_options() are the only two combinations used in production.
struct CoordsOptions {
	bool bounds_reject = false;  // editor: reject out-of-extent; runtime: & 0xF wrap
	bool clamp_sector_id = false; // editor: clampi(id, 0, 4); runtime: raw
	bool clamp_local = false;    // editor: clampf(local, 0, 512 - 0.001); runtime: raw
};

inline CoordsOptions coords_runtime_options() {
	return CoordsOptions{false, false, false};
}

inline CoordsOptions coords_editor_options() {
	return CoordsOptions{true, true, true};
}

template <typename Real>
struct CoordsResult {
	bool valid = false;
	int sector_sx = 0;
	int sector_sz = 0;
	int sector_id = 0;
	Real source_x = Real(0);
	Real source_z = Real(0);
};

inline int coords_clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Quadrant offsets: ids 3,4 sit in the right half (+512 X); ids 2,4 in the
// bottom half (+512 Z). Mirrors EditorTerrainMesh._sector_source_offset and the
// quadrant_x/quadrant_z branches of resolve_world_sample.
inline int coords_quadrant_offset_x(int sector_id) {
	return (sector_id == 3 || sector_id == 4) ? COORDS_SECTOR_SIZE : 0;
}

inline int coords_quadrant_offset_z(int sector_id) {
	return (sector_id == 2 || sector_id == 4) ? COORDS_SECTOR_SIZE : 0;
}

// Sector id at an explicit grid cell using the editor's guards: reject (return
// 0) outside the authored rows/cols, otherwise clamp to [0,4].
// Mirrors EditorTerrainMesh.get_sector_cell_value.
inline int coords_sector_id_at_cell(const SectorLayout &layout, int row, int col) {
	if (!layout.sector_grid || row < 0 || row >= layout.sector_rows ||
	    col < 0 || col >= layout.sector_count) {
		return 0;
	}
	return coords_clampi(layout.sector_grid[row * COORDS_SECTOR_GRID_DIM + col], 0, COORDS_SECTOR_ID_MAX);
}

// One sector's resolved atlas mapping — the per-sample-invariant half of
// coords_world_to_source. Exposed so hot callers that walk many samples
// through the same sector (the LOS raycast crosses a 512 u sector in ~128
// four-unit steps) can memoize it; the template below delegates here so the
// two can never drift.
struct CoordsSectorResolve {
	bool valid = false;
	int sector_id = 0;
	int quadrant_x = 0;
	int quadrant_z = 0;
};

inline CoordsSectorResolve coords_resolve_sector(const SectorLayout &layout, int sector_sx,
                                                 int sector_sz, const CoordsOptions &opts) {
	CoordsSectorResolve out;
	if (!layout.sector_grid) {
		return out;
	}
	const int grid_x = sector_sx - layout.origin_x;
	const int grid_z = sector_sz - layout.origin_y;

	int cell_x;
	int cell_z;
	if (opts.bounds_reject) {
		if (grid_z < 0 || grid_z >= layout.sector_rows || grid_x < 0 || grid_x >= layout.sector_count) {
			return out;
		}
		cell_x = grid_x;
		cell_z = grid_z;
	} else {
		cell_x = grid_x & (COORDS_SECTOR_GRID_DIM - 1);
		cell_z = grid_z & (COORDS_SECTOR_GRID_DIM - 1);
	}

	int sector_id = layout.sector_grid[cell_z * COORDS_SECTOR_GRID_DIM + cell_x];
	if (opts.clamp_sector_id) {
		sector_id = coords_clampi(sector_id, 0, COORDS_SECTOR_ID_MAX);
	}
	if (sector_id <= 0) {
		return out;
	}
	out.valid = true;
	out.sector_id = sector_id;
	out.quadrant_x = coords_quadrant_offset_x(sector_id);
	out.quadrant_z = coords_quadrant_offset_z(sector_id);
	return out;
}

// World -> source/atlas coordinate transform.
// Returns an invalid result (valid == false) for an out-of-extent cell (editor
// mode) or a sector id <= 0, matching the negative-sentinel contract the
// GDScript callers branch on.
template <typename Real>
CoordsResult<Real> coords_world_to_source(const SectorLayout &layout, Real world_x, Real world_z,
                                          const CoordsOptions &opts) {
	CoordsResult<Real> out;
	if (!layout.sector_grid) {
		return out;
	}
	const int sector_sx = static_cast<int>(std::floor(world_x / static_cast<Real>(COORDS_SECTOR_SIZE)));
	const int sector_sz = static_cast<int>(std::floor(world_z / static_cast<Real>(COORDS_SECTOR_SIZE)));
	out.sector_sx = sector_sx;
	out.sector_sz = sector_sz;

	const CoordsSectorResolve sector = coords_resolve_sector(layout, sector_sx, sector_sz, opts);
	if (!sector.valid) {
		return out;
	}
	out.sector_id = sector.sector_id;

	// sector_sx * 512 is an exact integer well within Real's exact range for the
	// bounded sector extent, so the multiply order matches both the editor's
	// float(origin_x + col) * 512 and the runtime's float(sector_sx * 512).
	Real local_x = world_x - static_cast<Real>(sector_sx) * static_cast<Real>(COORDS_SECTOR_SIZE);
	Real local_z = world_z - static_cast<Real>(sector_sz) * static_cast<Real>(COORDS_SECTOR_SIZE);
	if (opts.clamp_local) {
		const Real lo = Real(0);
		const Real hi = static_cast<Real>(COORDS_SECTOR_SIZE) - static_cast<Real>(0.001);
		local_x = local_x < lo ? lo : (local_x > hi ? hi : local_x);
		local_z = local_z < lo ? lo : (local_z > hi ? hi : local_z);
	}

	out.source_x = static_cast<Real>(sector.quadrant_x) + local_x;
	out.source_z = static_cast<Real>(sector.quadrant_z) + local_z;
	out.valid = true;
	return out;
}

template <typename Real>
struct CellCoordsResult {
	bool valid = false;
	int sector_id = 0;
	Real source_x = Real(0);
	Real source_z = Real(0);
};

// Source coords for an explicitly chosen cell, with the local offset left
// UNCLAMPED (the caller owns the cell choice and may sample outside it).
// Mirrors EditorTerrainMesh.world_to_cell_source_coords.
template <typename Real>
CellCoordsResult<Real> coords_world_to_cell_source(const SectorLayout &layout, Real world_x, Real world_z,
                                                   int row, int col) {
	CellCoordsResult<Real> out;
	const int sector_id = coords_sector_id_at_cell(layout, row, col);
	if (sector_id <= 0) {
		return out;
	}
	out.sector_id = sector_id;
	const Real cell_origin_x = static_cast<Real>(layout.origin_x + col) * static_cast<Real>(COORDS_SECTOR_SIZE);
	const Real cell_origin_z = static_cast<Real>(layout.origin_y + row) * static_cast<Real>(COORDS_SECTOR_SIZE);
	out.source_x = static_cast<Real>(coords_quadrant_offset_x(sector_id)) + (world_x - cell_origin_x);
	out.source_z = static_cast<Real>(coords_quadrant_offset_z(sector_id)) + (world_z - cell_origin_z);
	out.valid = true;
	return out;
}

struct CoordsRect {
	int x = 0;
	int z = 0;
	int w = 0;
	int h = 0;
};

// Atlas (quadrant) rect for a cell: a 512x512 window into the 1024 atlas at the
// cell's quadrant offset, or a zero rect for an empty/out-of-extent cell.
// Mirrors EditorTerrainMesh.get_cell_atlas_rect.
inline CoordsRect coords_cell_atlas_rect(const SectorLayout &layout, int row, int col) {
	const int sector_id = coords_sector_id_at_cell(layout, row, col);
	if (sector_id <= 0) {
		return CoordsRect{};
	}
	return CoordsRect{coords_quadrant_offset_x(sector_id), coords_quadrant_offset_z(sector_id),
	                  COORDS_SECTOR_SIZE, COORDS_SECTOR_SIZE};
}

} // namespace opennova::terrain
