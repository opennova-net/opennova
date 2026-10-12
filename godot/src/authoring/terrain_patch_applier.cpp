#include "authoring/terrain_patch_applier.h"

#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

#include <algorithm>

namespace godot {

bool TerrainPatchApplier::apply(const std::vector<opennova::editor::HeightsDevicePatch> &patches, TerrainData &data,
		Terrain &terrain) {
	regions_ = 0;
	tiles_ = 0;
	std::vector<opennova::terrain::CoordsRect> rects;
	std::vector<int> replaced;
	uint64_t last = taken_;
	for (const opennova::editor::HeightsDevicePatch &patch : patches) {
		if (patch.serial <= taken_) continue;
		last = std::max(last, patch.serial);
		for (const opennova::editor::HeightsDevicePatch::Region &region : patch.regions) {
			const opennova::terrain::CoordsRect rect{region.x, region.y, region.w, region.h};
			if (region.heights.size() != size_t(region.w) * size_t(region.h) ||
					!data.patch_heights(rect, region.heights.data(), size_t(region.w))) {
				taken_ = last;
				return false;
			}
			// Each region its own rect (a change across the atlas's wrap arrives as two): one refresh shows them all.
			rects.push_back(rect);
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
	if (rects.empty() && replaced.empty()) return true;
	std::sort(replaced.begin(), replaced.end());
	replaced.erase(std::unique(replaced.begin(), replaced.end()), replaced.end());
	// The foliage grows again with a landing (its tiles: the stroke's end, an undo, a redo), not every sample.
	return terrain.refresh_region(rects, replaced, !replaced.empty());
}

} // namespace godot
