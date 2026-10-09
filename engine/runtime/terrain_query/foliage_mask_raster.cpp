// The foliage map as the game's load keeps it and its sampler's grid (foliage_mask_raster.h).
#include <runtime/terrain_query/foliage_mask_raster.h>

#include <array>
#include <string>

#include <formats/pcx/pcx_io.h>

namespace opennova::terrain {

bool load_foliage_mask_raster(const std::vector<uint8_t> &pcx, const std::vector<FoliageDef> &defs,
		FoliageMaskRaster &out) {
	out = FoliageMaskRaster();
	// The game's 8-bit PCX reader, its indices kept, each texel remapped to the definition slots it selects
	// [orig: Foliage_LoadFoliageMapPCX @ 0x605AD0, the remap @ 0x605B73..0x605B8A; Foliage_RemapPixelToDefMask
	// @ 0x5FF4E0].
	std::string error;
	if (!decode_pcx_indexed(pcx.data(), pcx.size(), out.codes, error) || out.codes.empty()) {
		out = FoliageMaskRaster();
		return false;
	}
	const std::array<uint8_t, 256> remap = foliage_pixel_masks(defs);
	out.masks.resize(out.codes.indices.size());
	for (size_t i = 0; i < out.masks.size(); ++i) out.masks[i] = remap[out.codes.indices[i]];
	return true;
}

FoliageMaskMap foliage_mask_grid(const TerrainHeightField &field) {
	FoliageMaskMap map;
	map.sector_grid = field.layout.sector_grid;
	map.origin_x = field.layout.origin_x;
	map.origin_y = field.layout.origin_y;
	map.wrap_x = field.wrap_x;
	map.wrap_z = field.wrap_z;
	return map;
}

FoliageMaskMap foliage_mask_map(const FoliageMaskRaster &raster, const TerrainHeightField &field, bool codes) {
	if (raster.empty()) return FoliageMaskMap();
	FoliageMaskMap map = foliage_mask_grid(field);
	map.data = codes ? raster.codes.indices.data() : raster.masks.data();
	map.width = raster.codes.width;
	map.height = raster.codes.height;
	return map;
}

} // namespace opennova::terrain
