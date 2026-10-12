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

bool patch_foliage_mask_raster(FoliageMaskRaster &raster, const CoordsRect &rect, const uint8_t *src, size_t stride,
		const std::vector<FoliageDef> &defs) {
	const int32_t width = raster.codes.width, height = raster.codes.height;
	if (raster.empty() || src == nullptr || rect.w <= 0 || rect.h <= 0 || rect.x < 0 || rect.z < 0 ||
			rect.x > width - rect.w || rect.z > height - rect.h || stride < static_cast<size_t>(rect.w))
		return false;
	// The load's remap over the patched texels alone [orig: Foliage_LoadFoliageMapPCX @ 0x605B73..0x605B8A].
	const std::array<uint8_t, 256> remap = foliage_pixel_masks(defs);
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

FoliageMaskMap foliage_mask_map(const FoliageMaskRaster &raster, const TerrainHeightField &field, bool codes) {
	if (raster.empty()) return FoliageMaskMap();
	FoliageMaskMap map = foliage_mask_grid(field);
	map.data = codes ? raster.codes.indices.data() : raster.masks.data();
	map.width = raster.codes.width;
	map.height = raster.codes.height;
	return map;
}

} // namespace opennova::terrain
