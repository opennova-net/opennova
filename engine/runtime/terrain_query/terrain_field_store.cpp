#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <cmath>

namespace opennova::terrain {

void TerrainFieldStore::clear() {
	heightmap_.clear();
	sector_grid_.clear();
	surface_indices_.clear();
	field_ = TerrainHeightField{};
	surface_map_ = SurfaceTypeMap{};
}

void TerrainFieldStore::build(const uint16_t *heightmap, size_t heightmap_count,
		const int *sector_grid, int32_t origin_x, int32_t origin_y,
		const CoordsQuadrantLocks &locks,
		const uint8_t *charmap, int32_t charmap_width, int32_t charmap_height) {
	// Clear first so a null/empty heightmap disables grounding.
	clear();
	if (heightmap == nullptr || heightmap_count == 0 || sector_grid == nullptr) return;

	// Own copies: the field's raw pointers must outlive the source document /
	// resource and survive a reload.
	heightmap_.assign(heightmap, heightmap + heightmap_count);
	sector_grid_.assign(sector_grid,
			sector_grid + COORDS_SECTOR_GRID_DIM * COORDS_SECTOR_GRID_DIM);

	field_.heightmap = heightmap_.data();
	field_.dim = static_cast<int>(std::sqrt(static_cast<double>(heightmap_.size())));
	field_.layout.sector_grid = sector_grid_.data();
	// Sector origins + the per-quadrant neighbour-tap locks. Without the locks
	// every 512-unit sector boundary reads the neighbouring quadrant and
	// grounding drops into a one-unit trench the terrain mesh does not draw —
	// you fall through ground that looks solid.
	field_.layout.origin_x = origin_x;
	field_.layout.origin_y = origin_y;
	field_.locks = locks;
	// Water clamp deferred: water_height units (vs the 16.16 worldY @0x26C6454
	// the original compares) are not yet verified, so leave has_water off
	// rather than float entities onto a wrong plane. The ground-following path
	// does not need it.
	field_.has_water = false;

	// The charmap surface raster for the footstep surface pick (owned copy like
	// the depth buffer; shares the sector grid + origins)
	// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
	if (charmap != nullptr && charmap_width > 0 && charmap_height > 0) {
		surface_indices_.assign(charmap,
				charmap + static_cast<size_t>(charmap_width) * static_cast<size_t>(charmap_height));
		surface_map_.data = surface_indices_.data();
		surface_map_.width = charmap_width;
		surface_map_.height = charmap_height;
		surface_map_.sector_grid = sector_grid_.data();
		surface_map_.origin_x = origin_x;
		surface_map_.origin_y = origin_y;
	}
}

CoordsQuadrantLocks coords_locks_from(const TrnConfig &trn) {
	const TerrainQuadrantLocks source = trn.get_quadrant_locks();
	CoordsQuadrantLocks locks{};
	for (int quadrant = 0; quadrant < static_cast<int>(source.size()); ++quadrant) {
		locks.set(quadrant, source[quadrant].x != 0, source[quadrant].y != 0);
	}
	return locks;
}

void height_field_apply_trn(TerrainHeightField &field, const TrnConfig &trn) {
	field.layout.origin_x = trn.origin_x;
	field.layout.origin_y = trn.origin_y;
	field.locks = coords_locks_from(trn);
}

TerrainHeightField height_field_from(const CptFile &cpt, const TrnConfig &trn) {
	TerrainHeightField field;
	if (cpt.depth_buffer.empty()) {
		return field;
	}
	field.heightmap = cpt.depth_buffer.data();
	field.dim = static_cast<int>(std::sqrt(static_cast<double>(cpt.depth_buffer.size())));
	field.layout.sector_grid = &trn.sector_grid[0][0];
	height_field_apply_trn(field, trn);
	return field;
}

void terrain_field_store_build(TerrainFieldStore &store, const CptFile &cpt,
		const TrnConfig &trn, const uint8_t *charmap, int32_t charmap_width,
		int32_t charmap_height) {
	store.build(cpt.depth_buffer.data(), cpt.depth_buffer.size(),
			&trn.sector_grid[0][0], trn.origin_x, trn.origin_y,
			coords_locks_from(trn), charmap, charmap_width, charmap_height);
}

} // namespace opennova::terrain
