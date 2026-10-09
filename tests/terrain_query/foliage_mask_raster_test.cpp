// The foliage map as the game's load keeps it (runtime/terrain_query foliage_mask_raster.h): its PCX read through the
// 8-bit reader, the codes kept and each texel remapped to the definition slots it selects [orig:
// Foliage_LoadFoliageMapPCX @ 0x605AD0, the remap @ 0x605B73..0x605B8A]; bytes that do not read grow nothing; the
// sampler over a terrain's sector grid reads the masks or the codes where the game's would [orig:
// Foliage_SampleFoliageMapMask @ 0x606620].
#include <runtime/terrain_query/foliage_mask_raster.h>

#include <cstdio>
#include <string>
#include <vector>

#include <formats/pcx/pcx_io.h>

#include "common/test_expect.h"

namespace {

using opennova::FoliageDef;
using opennova::IndexedImage8;
using namespace opennova::terrain;

int test_raster() {
	// A 256-side map: 253 over the top-left quarter (the atlas's top-left 512 units), 254 over the top-right, 0
	// elsewhere.
	IndexedImage8 image;
	image.width = image.height = 256;
	image.indices.assign(size_t(256) * 256, 0);
	for (int row = 0; row < 128; ++row)
		for (int col = 0; col < 256; ++col) image.indices[size_t(row) * 256 + col] = col < 128 ? 253 : 254;
	image.palette[253][0] = 40;
	std::vector<uint8_t> pcx;
	std::string why;
	TEST_EXPECT(opennova::encode_pcx_indexed(image, pcx, why));
	std::vector<FoliageDef> defs(2);
	defs[0].graphic = "grass.3di";
	defs[0].match[0] = 253;
	defs[1].graphic = "shrub.3di";
	defs[1].match = {254, 253, opennova::FOLIAGE_MATCH_UNSET, opennova::FOLIAGE_MATCH_UNSET};
	FoliageMaskRaster raster;
	TEST_EXPECT(load_foliage_mask_raster(pcx, defs, raster) && !raster.empty());
	TEST_EXPECT(raster.codes.width == 256 && raster.codes.indices == image.indices && raster.codes.palette[253][0] == 40);
	TEST_EXPECT(raster.masks.size() == image.indices.size() && raster.masks[0] == 3 && raster.masks[200] == 2 &&
	            raster.masks[size_t(200) * 256] == 0);
	// Bytes that do not read: an empty raster.
	TEST_EXPECT(!load_foliage_mask_raster(std::vector<uint8_t>(10, 0), defs, raster) && raster.empty() &&
	            raster.masks.empty());

	// The sampler over a 2 x 2 grid from (-1, -1): the atlas's top-left quadrant (1) at the north-west sector.
	TEST_EXPECT(load_foliage_mask_raster(pcx, defs, raster));
	int grid[16][16] = {};
	grid[0][0] = 1, grid[0][1] = 3, grid[1][0] = 2, grid[1][1] = 4;
	TerrainHeightField field;
	field.layout.sector_grid = &grid[0][0];
	field.layout.origin_x = -1;
	field.layout.origin_y = -1;
	const FoliageMaskMap masks = foliage_mask_map(raster, field);
	const FoliageMaskMap codes = foliage_mask_map(raster, field, true);
	TEST_EXPECT(masks.width == 256 && masks.height == 256 && masks.sector_grid == &grid[0][0] && masks.origin_x == -1);
	// World (-256, 256): the north-west sector's middle, the atlas's (256, 256), the map's (64, 64): a 253.
	TEST_EXPECT(foliage_mask_at_fixed(masks, -256 * 65536, 256 * 65536) == 3);
	TEST_EXPECT(foliage_mask_at_fixed(codes, -256 * 65536, 256 * 65536) == 253);
	// World (256, 256): sector 3, the atlas's top-right quadrant, a 254 the shrub alone grows on.
	TEST_EXPECT(foliage_mask_at_fixed(masks, 256 * 65536, 256 * 65536) == 2);
	// World (-256, -256): sector 2, the bottom-left quadrant's 0s.
	TEST_EXPECT(foliage_mask_at_fixed(masks, -256 * 65536, -256 * 65536) == 0);
	// The grid alone and an empty raster's sampler: nothing grows.
	const FoliageMaskMap bare = foliage_mask_grid(field);
	TEST_EXPECT(bare.data == nullptr && bare.sector_grid == &grid[0][0] && foliage_mask_at_fixed(bare, 0, 0) == 0);
	TEST_EXPECT(foliage_mask_map(FoliageMaskRaster(), field).data == nullptr);
	std::printf("raster: the codes kept, the masks remapped, bytes that do not read empty; the sampler over a grid\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_raster();
	if (failures == 0) std::printf("foliage_mask_raster: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
