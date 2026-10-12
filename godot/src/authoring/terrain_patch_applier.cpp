#include "authoring/terrain_patch_applier.h"

#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

#include <algorithm>

namespace godot {

bool TerrainPatchApplier::apply(const std::vector<opennova::editor::HeightsDevicePatch> &patches, TerrainData &data,
		Terrain &terrain) {
	regions_ = 0;
	tiles_ = 0;
	bool any = false;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::vector<int> replaced;
	uint64_t last = taken_;
	for (const opennova::editor::HeightsDevicePatch &patch : patches) {
		if (patch.serial <= taken_) continue;
		last = std::max(last, patch.serial);
		for (const opennova::editor::HeightsDevicePatch::Region &region : patch.regions) {
			if (region.heights.size() != size_t(region.w) * size_t(region.h) ||
					!data.patch_heights(opennova::terrain::CoordsRect{region.x, region.y, region.w, region.h},
							region.heights.data(), size_t(region.w))) {
				taken_ = last;
				return false;
			}
			// The frame's regions as one rect: the terrain shows it again once.
			if (!any) {
				x0 = region.x;
				y0 = region.y;
				x1 = region.x + region.w;
				y1 = region.y + region.h;
			} else {
				x0 = std::min(x0, region.x);
				y0 = std::min(y0, region.y);
				x1 = std::max(x1, region.x + region.w);
				y1 = std::max(y1, region.y + region.h);
			}
			any = true;
			++regions_;
		}
		if (!patch.tile_indices.empty()) {
			if (!data.replace_tiles(patch.tile_indices, patch.tiles)) {
				taken_ = last;
				return false;
			}
			replaced.insert(replaced.end(), patch.tile_indices.begin(), patch.tile_indices.end());
			tiles_ += int(patch.tile_indices.size());
		}
	}
	taken_ = last;
	if (!any && replaced.empty()) return true;
	std::sort(replaced.begin(), replaced.end());
	replaced.erase(std::unique(replaced.begin(), replaced.end()), replaced.end());
	const opennova::terrain::CoordsRect rect =
			any ? opennova::terrain::CoordsRect{x0, y0, x1 - x0, y1 - y0} : opennova::terrain::CoordsRect{};
	return terrain.refresh_region(rect, replaced);
}

} // namespace godot
