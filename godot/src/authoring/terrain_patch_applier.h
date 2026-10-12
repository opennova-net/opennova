#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/preview/terrain_heights_patch.h>
#include <editor/preview/terrain_maps_patch.h>

namespace godot {

class FoliageDispatcher;
class Terrain;
class TerrainData;

// The device half of a sculpted terrain's live picture (ADR 0046 S25, the heights lane), shared by the mission's and
// the terrain's 3D views: the heights patches their viewport made since this device last took them, applied to the
// terrain it built (TerrainData::patch_heights over each region, TerrainData::replace_tiles for a landing's tiles,
// then one Terrain::refresh_region over the frame's regions, each its own rect, with the replaced tiles: the terrain
// shown as a fresh build of the patched data, never built again). In a stroke the regions draw under the old tiles
// (what the game draws for that atlas), the foliage over them kept back; the landing's tiles take their place and
// the foliage over every region since grows again then (Terrain::refresh_region's foliage).
class TerrainPatchApplier {
public:
	// The patches `patches` holds past the last serial this device took, applied to `terrain` over `data` (the one it
	// built); true where each applied (none: true). False where one could not (a region or a tile the data refuses, a
	// terrain not built): the caller builds its terrain again.
	bool apply(const std::vector<opennova::editor::HeightsDevicePatch> &patches, TerrainData &data, Terrain &terrain);
	// The terrain built again (or none): the patches up to `serial` stand in it.
	void built(uint64_t serial) { taken_ = serial; }
	uint64_t taken() const { return taken_; }
	// What the last apply did (a test's count): the regions written and the tiles replaced.
	int regions_applied() const { return regions_; }
	int tiles_applied() const { return tiles_; }

	// The maps' and the placed tiles' patches (S25, the maps lane: MapsDevicePatch) past the last serial taken, applied
	// in place to `terrain`: a colour map's region (Terrain::update_colormap_region: its tile pages that sample it
	// recompose), a detail blend map's (update_blendmap_region), a foliage map's codes (TerrainData::
	// write_foliagemap_rect, then `foliage`'s cells over the ground that reads them dropped and grown again when next
	// visible: FoliageDispatcher::invalidate_cells), the placed tiles (the override's entries replaced,
	// TerrainTileInfo::copy_from_native: the tile pages over the changed squares recompose and the foliage that reads the
	// override places again; a new override where the terrain held none), and the surface map's move (nothing drawn).
	// `took` hears each file a patch accounts for, at its stamp: the caller's read of it holds that stamp now, so the
	// file's move does not build the terrain again. False where one could not be taken (a map of another size, a
	// region outside it, a terrain not built): maps_refused says so once, and the caller's picture is made again.
	bool apply_maps(const std::vector<opennova::editor::MapsDevicePatch> &patches, Terrain &terrain,
			FoliageDispatcher *foliage, const std::function<void(const std::string &name, uint64_t stamp)> &took);
	// The terrain built again (or none): the maps' patches up to `serial` stand in it.
	void maps_built(uint64_t serial) { maps_taken_ = serial; }
	uint64_t maps_taken() const { return maps_taken_; }
	// Whether a patch could not be taken since this was last asked (the report's maps_refused).
	bool take_maps_refused() {
		const bool refused = maps_refused_;
		maps_refused_ = false;
		return refused;
	}
	// What the last apply_maps did (a test's count): the patches applied.
	int maps_applied() const { return maps_applied_; }

private:
	uint64_t taken_ = 0;
	int regions_ = 0;
	int tiles_ = 0;
	uint64_t maps_taken_ = 0;
	bool maps_refused_ = false;
	int maps_applied_ = 0;
};

} // namespace godot
