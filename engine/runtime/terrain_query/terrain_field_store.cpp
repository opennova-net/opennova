#include <runtime/terrain_query/terrain_field_build.h>
#include <sstream>
#include <formats/trn/trn_io.h>
#include <formats/pcx/pcx_io.h>
#include <formats/cpt/cpt_io.h>
#include <base/io/log.h>
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
		int32_t sector_count, int32_t sector_rows, bool wrap_x, bool wrap_z,
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
	field_.layout.sector_count = sector_count;
	field_.layout.sector_rows = sector_rows;
	field_.locks = locks;
	field_.wrap_x = wrap_x;
	field_.wrap_z = wrap_z;
	// The water plane arrives separately (set_water_plane, fed from the
	// world's environment): a rebuilt field starts without one.
	field_.water_y = 0;
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
	field.layout.sector_count = trn.sector_count;
	field.layout.sector_rows = trn.sector_rows;
	field.locks = coords_locks_from(trn);
	field.wrap_x = trn.wrap_x != 0;
	field.wrap_z = trn.wrap_y != 0;
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
			trn.sector_count, trn.sector_rows, trn.wrap_x != 0, trn.wrap_y != 0,
			coords_locks_from(trn), charmap, charmap_width, charmap_height);
}

bool terrain_field_store_load(TerrainFieldStore &store, const ResourceIndex &index,
		const std::string &terrain_name, std::string &error) {
	if (terrain_name.empty()) {
		error = "the mission names no terrain";
		return false;
	}
	std::vector<uint8_t> cpt_bytes, trn_bytes;
	if (!index.read_file(terrain_name + ".cpt", cpt_bytes) ||
			!index.read_file(terrain_name + ".trn", trn_bytes)) {
		error = terrain_name + ".cpt/.trn are not under the mount";
		return false;
	}
	std::string doc_error;
	CptFile cpt;
	if (!load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, doc_error)) {
		error = terrain_name + ".cpt: " + doc_error;
		return false;
	}
	TrnConfig trn;
	std::string raw(reinterpret_cast<const char *>(trn_bytes.data()), trn_bytes.size());
	std::istringstream ts(raw);
	if (!load_trn(ts, trn, doc_error) || cpt.depth_buffer.empty()) {
		error = terrain_name + ".trn: " + doc_error;
		return false;
	}
	IndexedImage8 charmap;
	std::vector<uint8_t> charmap_bytes;
	if (!trn.charmap.empty()) {
		if (!index.read_file(trn.charmap, charmap_bytes)) {
			io::logf(io::LogLevel::kWarn, "terrain: charmap %s is not under the mount - no surface map",
					trn.charmap.c_str());
		} else {
			std::string charmap_error;
			if (!decode_pcx_indexed(charmap_bytes.data(), charmap_bytes.size(), charmap,
						charmap_error)) {
				charmap = IndexedImage8{};
				io::logf(io::LogLevel::kWarn, "terrain: charmap %s did not decode (%s) - no surface map",
						trn.charmap.c_str(), charmap_error.c_str());
			}
		}
	}
	terrain_field_store_build(store, cpt, trn,
			charmap.empty() ? nullptr : charmap.indices.data(),
			charmap.width, charmap.height);
	return store.valid();
}

} // namespace opennova::terrain
