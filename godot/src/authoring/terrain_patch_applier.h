#pragma once

#include <cstdint>
#include <vector>

#include <editor/preview/terrain_heights_patch.h>

namespace godot {

class Terrain;
class TerrainData;

// The device half of a sculpted terrain's live picture (ADR 0046 S25, the heights lane), shared by the mission's and
// the terrain's 3D views: the heights patches their viewport made since this device last took them, applied to the
// terrain it built (TerrainData::patch_heights over each region, TerrainData::replace_tiles for a landing's tiles,
// then one Terrain::refresh_region over the union of the frame's regions with the replaced tiles: the terrain shown
// as a fresh build of the patched data, never built again). In a stroke the regions draw under the old tiles (what
// the game draws for that atlas); the landing's tiles take their place.
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

private:
	uint64_t taken_ = 0;
	int regions_ = 0;
	int tiles_ = 0;
};

} // namespace godot
