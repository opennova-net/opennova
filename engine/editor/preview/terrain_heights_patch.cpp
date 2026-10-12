#include <editor/preview/terrain_heights_patch.h>

#include <algorithm>
#include <variant>

#include <editor/documents/terrain_heights_document.h>
#include <editor/model/change_set.h>
#include <editor/preview/terrain_patch.h>

namespace opennova::editor {

bool HeightsTilesFollow::take(const TerrainPatch &patch, const DocumentBase &document, HeightsDevicePatch &out) {
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(&document);
	if (!heights || heights->plane_count() < 2 || heights->blocked()) return false;
	if (document.identity() != identity_ || document.load_generation() != load_) {
		// A picture made from the file, its document opened since (a stroke's file opened quietly): its tiles as it
		// loaded them stand in the picture where no step since replaced one.
		ChangeSet changes;
		const RasterChanges *raster = nullptr;
		if (identity_ != 0 || load_ != 0 || !document.changes_since(document.load_generation(), 0, changes) ||
				!(raster = std::get_if<RasterChanges>(&changes)) || (raster->parts & kHeightsTilesPart) != 0)
			return false;
		identity_ = document.identity();
		load_ = document.load_generation();
		tiles_ = heights->heights().tiles;
	}
	HeightsDevicePatch made;
	made.path = patch.path;
	std::vector<RasterRegion> regions = patch.regions;
	if (patch.whole) regions.assign(1, RasterRegion{0, 0, kHeightsSide, kHeightsSide, 0});
	for (const RasterRegion &region : regions) {
		const int x0 = std::max(region.x, 0), y0 = std::max(region.y, 0);
		const int x1 = std::min(region.x + region.width, kHeightsSide);
		const int y1 = std::min(region.y + region.height, kHeightsSide);
		if (x0 >= x1 || y0 >= y1) continue;
		HeightsDevicePatch::Region taken;
		taken.x = x0;
		taken.y = y0;
		taken.w = x1 - x0;
		taken.h = y1 - y0;
		taken.heights = heights->live_heights(HeightsRect{taken.x, taken.y, taken.w, taken.h});
		made.regions.push_back(std::move(taken));
	}
	// The tiles a landing (or its undo, or its redo) replaced: those whose shared tile is another one now.
	const std::vector<std::shared_ptr<const CptTile>> &now = heights->heights().tiles;
	if (now.size() != tiles_.size()) return false;
	for (size_t i = 0; i < now.size(); ++i) {
		if (now[i] == tiles_[i] || !now[i]) continue;
		made.tile_indices.push_back(int(i));
		made.tiles.push_back(*now[i]);
	}
	tiles_ = now;
	out = std::move(made);
	return true;
}

void HeightsTilesFollow::shown(const DocumentBase *document) {
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(document);
	identity_ = heights ? heights->identity() : 0;
	load_ = heights ? heights->load_generation() : 0;
	tiles_ = heights ? heights->heights().tiles : std::vector<std::shared_ptr<const CptTile>>();
}

} // namespace opennova::editor
