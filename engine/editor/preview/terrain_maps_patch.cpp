#include <editor/preview/terrain_maps_patch.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/texture_raster.h>
#include <editor/model/document_base.h>
#include <editor/model/raster_document.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/terrain_map_target.h>
#include <editor/preview/terrain_patch.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_family.h>
#include <editor/session/view/session_view.h>
#include <formats/til/til_io.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace opennova::editor {

namespace {

const DocumentBase *open_document(const SessionView &view, const std::string &path) {
	for (const auto &each : view.documents.open)
		if (each && each->path() == path) return each.get();
	return nullptr;
}

// A map patch's rectangle: the union of its regions on the plane (the whole plane for a whole change), cut at its edges.
bool map_rect(const TerrainPatch &patch, const RasterPlane &plane, int &x, int &y, int &w, int &h) {
	int x0 = plane.width(), y0 = plane.height(), x1 = 0, y1 = 0;
	if (patch.whole) {
		x0 = y0 = 0;
		x1 = plane.width();
		y1 = plane.height();
	}
	for (const RasterRegion &region : patch.regions) {
		x0 = std::min(x0, std::max(region.x, 0));
		y0 = std::min(y0, std::max(region.y, 0));
		x1 = std::max(x1, std::min(region.x + region.width, plane.width()));
		y1 = std::max(y1, std::min(region.y + region.height, plane.height()));
	}
	if (x1 <= x0 || y1 <= y0) return false;
	x = x0;
	y = y0;
	w = x1 - x0;
	h = y1 - y0;
	return true;
}

} // namespace

void TerrainMapsFollow::read_tiles_(const std::shared_ptr<const ProjectAssetSource> &assets, const TilesRead &read,
                                    std::string &took, std::vector<uint8_t> &bytes, FileStamps &stamps) {
	took.clear();
	bytes.clear();
	stamps.clear();
	if (!assets || read.terrain.empty()) return;
	// The game's rule (terrain::read_placed_tile_bytes): <mission>.til loose first, else the terrain's own
	// polytrn_tileinfo, each taken only where its load takes it [orig: PolyTrn_LoadTerrainConfig @ 0x60e3d0, the
	// authority's reads @ 0x60e6d2 and 0x60e6e5], every name it looked at noted with its stamp.
	const auto stamped = std::make_shared<StampedFiles>(assets);
	const terrain::TerrainFileReader reader = [stamped](const std::string &name, std::vector<uint8_t> &out) {
		return stamped->read(name, out);
	};
	const std::string mission_til = read.mission.empty() ? std::string() : read.mission + ".til";
	took = terrain::read_placed_tile_bytes(reader, reader, mission_til, read.terrain, read.environment, bytes);
	stamps = stamped->stamps();
}

bool TerrainMapsFollow::take(const SessionView &view, const DocumentBase &document, const TerrainPatches &patches,
                             const TilesRead &read, MissionGround &ground, std::vector<std::string> &names) {
	const std::shared_ptr<const ProjectAssetSource> &assets = view.findings.assets;
	if (!assets) return false;
	bool queued = false;
	// The maps the stroke follow patched in its ground: the colour and the detail blend maps' texels, the foliage map's
	// codes, the surface map's stamp (the device draws nothing of it). The heights are the heights lane's
	// (HeightsTilesFollow).
	for (const TerrainPatch &patch : patches.patches) {
		MapsDevicePatch made;
		switch (patch.plane) {
		case TerrainPlane::Colour: made.kind = MapsDevicePatch::Kind::Colour; break;
		case TerrainPlane::Blend: made.kind = MapsDevicePatch::Kind::Blend; break;
		case TerrainPlane::Surface: made.kind = MapsDevicePatch::Kind::Surface; break;
		case TerrainPlane::Foliage: made.kind = MapsDevicePatch::Kind::Foliage; break;
		default: continue;
		}
		const std::string name = basename_of(patch.path);
		if (made.kind == MapsDevicePatch::Kind::Foliage) {
			// The codes of the foliage map the terrain names (not an import's source), as an 8-bit plane, and where its
			// foliage reads them: the foliage map's lookup routes a position as the char map's, the texel alone [orig:
			// Foliage_SampleFoliageMapMask @ 0x606620], over every sector drawing the quadrant.
			const DocumentBase *open = open_document(view, patch.path);
			const RasterDocument *raster = open ? open->as_raster() : nullptr;
			TerrainFamily family;
			const bool resolved = resolve_terrain_family(view, document, family) && family.trn;
			const TerrainFamilyFile &file = family.plane(TerrainPlane::Foliage);
			// The foliage map itself, or an imported terrain's source in its output's place (its indices or grey levels
			// the codes, the side kept: decode_foliage_map_source).
			const bool own = file.import_output.empty() && strutil::iequals(name, basename_of(file.name));
			const bool source = !file.import_output.empty() && file.path == patch.path;
			const RasterFormat format = raster && raster->plane_count() == 1 ? raster->plane(0).format() : RasterFormat::U8;
			const bool grey = source && (format == RasterFormat::Rgb8 || format == RasterFormat::Rgba8);
			if (!raster || raster->plane_count() != 1 || (format != RasterFormat::U8 && !grey) || !resolved ||
			    !(own || source) || !map_rect(patch, raster->plane(0), made.x, made.y, made.w, made.h)) {
				stale_ = true;
				names.push_back(name);
				continue;
			}
			const RasterPlane &plane = raster->plane(0);
			made.width = plane.width();
			made.height = plane.height();
			made.indices.resize(size_t(made.w) * size_t(made.h));
			if (grey) {
				// A grey source's levels (its red) are the codes.
				for (int y = 0; y < made.h; ++y)
					for (int x = 0; x < made.w; ++x)
						made.indices[size_t(y) * size_t(made.w) + size_t(x)] = plane.texel(made.x + x, made.y + y)[0];
			} else {
				plane.copy_rect(made.x, made.y, made.w, made.h, made.indices.data());
			}
			const TerrainMapTarget target = terrain_map_target(*family.trn, TerrainPlane::Foliage, made.width, made.height);
			const terrain::TerrainAtlasRouting routing{target.grid.data(), target.origin_x, target.origin_y, target.wrap_x,
			                                           target.wrap_z};
			made.world = terrain::terrain_atlas_world_rects(
			        routing, terrain::terrain_map_atlas_rect(terrain::CoordsRect{made.x, made.y, made.w, made.h}, made.width));
		} else if (made.kind != MapsDevicePatch::Kind::Surface) {
			const DocumentBase *open = open_document(view, patch.path);
			const RasterDocument *raster = open ? open->as_raster() : nullptr;
			if (!raster || !texture_paintable(raster->state()) ||
			    !map_rect(patch, raster->plane(0), made.x, made.y, made.w, made.h) ||
			    !texture_rgba_rect(raster->state(), made.x, made.y, made.w, made.h, made.rgba))
				continue;
			made.width = raster->plane(0).width();
			made.height = raster->plane(0).height();
		}
		made.files.push_back({name, assets->stamp(name)});
		made.serial = ++serial_;
		names.push_back(name);
		patches_.push_back(std::move(made));
		queued = true;
	}
	// The placed tiles, compared when the files' generation moved: a move of a file their read looked at (a tile tool's
	// edit of the open .til, the mission's .til made, opened, closed or read again) reads them again as the game
	// would, the whole placement the device's.
	if (assets->generation() == generation_) return queued;
	generation_ = assets->generation();
	if (!tiles_.moved(*assets)) return queued;
	std::string took;
	std::vector<uint8_t> bytes;
	FileStamps stamps;
	read_tiles_(assets, read, took, bytes, stamps);
	MapsDevicePatch made;
	made.kind = MapsDevicePatch::Kind::Tiles;
	std::string error;
	if (!took.empty() && !load_til(bytes.data(), bytes.size(), made.tiles, error)) made.tiles = TilFile();
	// The tile files alone take their stamps (the mission's .til and the one taken); a .trn or an .env the read looked
	// at that moved is the picture's to make again.
	const std::string mission_til = read.mission.empty() ? std::string() : read.mission + ".til";
	std::vector<std::string> tiled;
	for (const FileStamp &file : stamps.files())
		if ((!mission_til.empty() && strutil::iequals(file.name, mission_til)) ||
		    (!took.empty() && strutil::iequals(file.name, took))) {
			made.files.push_back(file);
			tiled.push_back(file.name);
		}
	ground.retile(*assets, took, bytes, tiled);
	names.insert(names.end(), tiled.begin(), tiled.end());
	made.serial = ++serial_;
	patches_.push_back(std::move(made));
	tiles_ = std::move(stamps);
	return true;
}

void TerrainMapsFollow::built(const SessionView &view, const TilesRead &read) {
	patches_.clear();
	built_serial_ = serial_;
	stale_ = false;
	generation_ = view.findings.assets ? view.findings.assets->generation() : UINT64_MAX;
	std::string took;
	std::vector<uint8_t> bytes;
	read_tiles_(view.findings.assets, read, took, bytes, tiles_);
}

void TerrainMapsFollow::taken(uint64_t serial, bool refused) {
	patches_.erase(std::remove_if(patches_.begin(), patches_.end(),
	                              [&](const MapsDevicePatch &patch) { return patch.serial <= serial; }),
	               patches_.end());
	stale_ = stale_ || refused;
}

} // namespace opennova::editor
