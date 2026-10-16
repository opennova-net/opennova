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
	out.remap = foliage_pixel_masks(defs);
	out.masks.resize(out.codes.indices.size());
	for (size_t i = 0; i < out.masks.size(); ++i) out.masks[i] = out.remap[out.codes.indices[i]];
	return true;
}

bool patch_foliage_mask_raster(FoliageMaskRaster &raster, const CoordsRect &rect, const uint8_t *src, size_t stride) {
	const int32_t width = raster.codes.width, height = raster.codes.height;
	if (raster.empty() || src == nullptr || rect.w <= 0 || rect.h <= 0 || rect.x < 0 || rect.z < 0 ||
			rect.x > width - rect.w || rect.z > height - rect.h || stride < static_cast<size_t>(rect.w))
		return false;
	// The load's remap over the patched texels alone [orig: Foliage_LoadFoliageMapPCX @ 0x605B73..0x605B8A].
	const std::array<uint8_t, 256> &remap = raster.remap;
	for (int32_t row = 0; row < rect.h; ++row) {
		const uint8_t *from = src + static_cast<size_t>(row) * stride;
		const size_t at = static_cast<size_t>(rect.z + row) * static_cast<size_t>(width) + static_cast<size_t>(rect.x);
		for (int32_t col = 0; col < rect.w; ++col) {
			raster.codes.indices[at + col] = from[col];
			raster.masks[at + col] = remap[from[col]];
		}
	}
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

FoliageMaskMap foliage_code_map(const FoliageMap &codes, const TrnConfig &trn) {
	FoliageMaskMap map;
	if (codes.width <= 0 || codes.height <= 0 ||
			codes.indices.size() <
					static_cast<size_t>(codes.width) * static_cast<size_t>(codes.height))
		return map;
	map.data = codes.indices.data();
	map.width = codes.width;
	map.height = codes.height;
	map.sector_grid = &trn.sector_grid[0][0];
	map.origin_x = trn.origin_x;
	map.origin_y = trn.origin_y;
	map.wrap_x = trn.wrap_x != 0;
	map.wrap_z = trn.wrap_y != 0;
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
