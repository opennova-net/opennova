#include <runtime/terrain_query/terrain_field_build.h>
#include <formats/env/env.h>
#include <formats/trn/trn_io.h>
#include <formats/pcx/pcx_io.h>
#include <formats/cpt/cpt_io.h>
#include <formats/til/til_io.h>
#include <base/io/log.h>
#include <runtime/terrain_query/surface_tiles.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <array>
#include <cmath>

namespace opennova::terrain {

void TerrainFieldStore::clear() {
	heightmap_.clear();
	sector_grid_.clear();
	surface_indices_.clear();
	field_ = TerrainHeightField{};
	surface_map_ = SurfaceTypeMap{};
	trn_water_height_ = 0.0f;
	tilestrip_.clear();
	attach_placed_tiles();
}

void TerrainFieldStore::set_placed_tiles(std::vector<SurfaceTileEntry> tiles) {
	placed_tiles_ = std::move(tiles);
	attach_placed_tiles();
}

void TerrainFieldStore::set_tile_surface_table(const std::array<uint8_t, 256> &table) {
	tile_surface_ = table;
	attach_placed_tiles();
}

// The overlay onto the view: no tiles skips the pass; the table is always
// present (zeroed = retail's memset default). With no charmap the sampler's
// early return-1 skips the walk exactly like retail, so attaching the tiles
// unconditionally is faithful.
void TerrainFieldStore::attach_placed_tiles() {
	surface_map_.tiles = placed_tiles_.empty() ? nullptr : placed_tiles_.data();
	surface_map_.tile_count = static_cast<int32_t>(placed_tiles_.size());
	surface_map_.tile_surface = tile_surface_.data();
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
		// The cell masks the sampler clamps or wraps by (D-TERRAIN-16) [orig:
		// PolyTrn_LoadTerrainConfig @ 0x60E4B1..0x60E4CB].
		surface_map_.wrap_x = wrap_x;
		surface_map_.wrap_z = wrap_z;
	}
	attach_placed_tiles();
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
	if (store.valid())
		store.set_trn_facts(trn.water_height != 0 ? static_cast<float>(trn.water_height) * 0.5f : 0.0f,
				trn.tilestrip);
}

std::string terrain_tileinfo_name(const std::string &tileinfo) {
	if (tileinfo.empty()) return std::string();
	return tileinfo.substr(0, tileinfo.find('.')) + ".til";
}

std::string read_placed_tile_bytes(const TerrainFileReader &read_loose_first,
		const TerrainFileReader &read_file, const std::string &mission_til,
		const std::string &terrain_name, const std::string &environment,
		std::vector<uint8_t> &out) {
	out.clear();
	const auto load = [&read_loose_first, &out](const std::string &name) {
		if (name.empty() || !read_loose_first || !read_loose_first(name, out) ||
				!til_load_accepts(out.data(), out.size())) {
			out.clear();
			return false;
		}
		return true;
	};
	if (load(mission_til)) return mission_til;
	// The terrain configuration as the terrain's load parses it; a .trn that
	// is not there names no tile info, and one the admission gate refuses
	// still carries the value its parser stored.
	if (!read_file || terrain_name.empty()) return std::string();
	const TrnTextReader read_text = [&read_file](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!read_file(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
	TrnConfig trn;
	std::string error;
	(void)read_mission_trn(read_text, terrain_name + ".trn",
			environment.empty() ? std::string() : environment + ".env", trn, error);
	const std::string own = terrain_tileinfo_name(trn.tileinfo);
	return load(own) ? own : std::string();
}

void terrain_field_store_set_placed_tiles(TerrainFieldStore &store,
		const std::vector<uint8_t> &til_bytes) {
	store.set_placed_tiles(surface_tiles_from_til_bytes(til_bytes));
}

void terrain_field_store_resolve_tile_surfaces(TerrainFieldStore &store,
		const std::function<bool(const std::string &name)> &has_file,
		const std::function<bool(const std::string &name, std::vector<uint8_t> &out)> &read_file) {
	std::array<uint8_t, 256> table{};
	if (store.valid() && has_file && read_file) {
		SurfaceTileFileSource files;
		files.has_file = has_file;
		files.read_file = read_file;
		resolve_tileset_surface_table(files, store.tilestrip(), table.data());
	}
	store.set_tile_surface_table(table);
}

namespace {

// The one load body: `read` answers a file by its flat name (the resource index's, a file source's).
template <typename Read>
bool load_store(TerrainFieldStore &store, const Read &read, const std::string &terrain_name,
		const std::string &tile_set, const std::string &environment, std::string &error, TrnConfig *trn_out) {
	if (terrain_name.empty()) {
		error = "the mission names no terrain";
		return false;
	}
	std::vector<uint8_t> cpt_bytes;
	const auto read_text = [&read](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!read(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
	std::string trn_text;
	if (!read_text(terrain_name + ".trn", trn_text)) {
		error = terrain_name + ".trn is not under the mount";
		return false;
	}
	// The .trn, then overcast.def and the mission's .env through the terrain's parser (D-TERRAIN-18) [orig:
	// Terrain_LoadEnvironmentConfig @ 0x6109ad -> Environment_LoadTimeOfDayConfig @ 0x57db44].
	std::string overcast_text, environment_text;
	TrnLaterTexts later;
	if (read_text(env::kOvercastFile, overcast_text)) later.overcast = &overcast_text;
	if (!environment.empty() && read_text(environment + ".env", environment_text)) later.environment = &environment_text;
	std::string doc_error;
	TrnConfig trn;
	if (!load_mission_trn(trn_text, later, trn, doc_error)) {
		error = terrain_name + ".trn: " + doc_error;
		return false;
	}
	// The height data by the name the .trn writes (D-TERRAIN-17), never the terrain's own: terrains share one (15 of
	// JO:CA's 38 name another's, dvxg8a.trn dvxg8.cpt) [orig: PolyTrn_LoadTerrainConfig @ 0x60E62F -> sub_603CC0 @
	// 0x603D0A].
	if (!read(trn.polydata, cpt_bytes)) {
		error = trn.polydata + " (" + terrain_name + ".trn's height data) is not under the mount";
		return false;
	}
	CptFile cpt;
	if (!load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, doc_error) || cpt.depth_buffer.empty()) {
		error = trn.polydata + ": " + (doc_error.empty() ? std::string("no heights") : doc_error);
		return false;
	}
	// One name drives the atlas and the .TSD: the mission's tile set over the
	// .trn value [orig: Terrain_LoadEnvironmentConfig @ 0x6109ce].
	trn.tilestrip = trn_mission_tilestrip(trn, tile_set);
	IndexedImage8 charmap;
	std::vector<uint8_t> charmap_bytes;
	if (!trn.charmap.empty()) {
		if (!read(trn.charmap, charmap_bytes)) {
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
	if (trn_out != nullptr) *trn_out = std::move(trn);
	return store.valid();
}

} // namespace

bool terrain_field_store_load(TerrainFieldStore &store, const ResourceIndex &index,
		const std::string &terrain_name, const std::string &tile_set, const std::string &environment,
		std::string &error) {
	const auto read = [&index](const std::string &name, std::vector<uint8_t> &out) {
		return index.read_file(name, out);
	};
	return load_store(store, read, terrain_name, tile_set, environment, error, nullptr);
}

bool terrain_field_store_load(TerrainFieldStore &store, const FileSource &files,
		const std::string &terrain_name, const std::string &tile_set, const std::string &environment,
		std::string &error, TrnConfig *trn) {
	const auto read = [&files](const std::string &name, std::vector<uint8_t> &out) {
		return files.read(name, out);
	};
	return load_store(store, read, terrain_name, tile_set, environment, error, trn);
}

} // namespace opennova::terrain
