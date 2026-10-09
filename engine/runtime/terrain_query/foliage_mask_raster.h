#pragma once

// The terrain's foliage map as the game's load keeps it, and the sampler (foliage_mask_map.h) over it and a
// terrain's sector grid.

#include <cstdint>
#include <vector>

#include <formats/foliage/foliage.h>
#include <formats/pcx/pcx.h>
#include <runtime/terrain_query/foliage_mask_map.h>
#include <runtime/terrain_query/height_field.h>

namespace opennova::terrain {

// The foliage map as the game's load keeps it: the codes as read through its 8-bit PCX reader (their side and
// palette kept for whoever shows them), and each texel remapped to the definition slots it selects
// (foliage_pixel_masks over the .trn's definitions) [orig: Foliage_LoadFoliageMapPCX @ 0x605AD0, the copy
// @ 0x605B3A, the remap @ 0x605B73..0x605B8A; Foliage_RemapPixelToDefMask @ 0x5FF4E0].
struct FoliageMaskRaster {
	IndexedImage8 codes;
	std::vector<uint8_t> masks;
	bool empty() const { return codes.empty(); }
};

// The raster of a foliage map's PCX bytes under the .trn's definitions (`defs` its slots); false, the raster left
// empty, for bytes that do not read or an empty image: a map that does not read grows nothing.
bool load_foliage_mask_raster(const std::vector<uint8_t> &pcx, const std::vector<FoliageDef> &defs,
		FoliageMaskRaster &out);

// The sampler's grid over a terrain's height field (its sector grid, origin and wraps), no raster: what a foliage
// map's sampler routes a position through.
FoliageMaskMap foliage_mask_grid(const TerrainHeightField &field);

// The sampler over a raster and a terrain's grid: its masks, or (`codes`) the codes as read; the empty sampler
// (nothing grows) for an empty raster.
FoliageMaskMap foliage_mask_map(const FoliageMaskRaster &raster, const TerrainHeightField &field, bool codes = false);

} // namespace opennova::terrain
