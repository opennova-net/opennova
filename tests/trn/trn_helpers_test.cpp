// The .trn writer's and the gate's shared pieces (formats/trn trn_io.h): the grid extension past the rows and the
// width read, the last again or, under the axis's wrap, the cells again from the first [orig:
// Terrain_ShiftHeightmapRows @0x60F2A5..0x60F317], as load_trn leaves the grid; and a foliage definition's block as
// save_trn writes it (trn_foliage_block_text), read back as the same definition.
#include <formats/trn/trn_io.h>

#include <cstdio>
#include <sstream>
#include <string>

#include "common/test_expect.h"

namespace {

using opennova::TrnConfig;

int test_grid_extension() {
	// The source cell of each place past the count: the last again, or the cells again from the first.
	TEST_EXPECT(opennova::trn_grid_extension_source(3, 0, 3) == 2 && opennova::trn_grid_extension_source(3, 0, 15) == 2);
	TEST_EXPECT(opennova::trn_grid_extension_source(3, 1, 3) == 0 && opennova::trn_grid_extension_source(3, 1, 4) == 1 &&
	            opennova::trn_grid_extension_source(5, 1, 7) == 2);
	// Two rows of two read, no wrap: each row's last cell out to 16, then the last row down to 16.
	TrnConfig flat;
	flat.sector_count = 2;
	flat.sector_rows = 2;
	flat.sector_grid[0][0] = 1, flat.sector_grid[0][1] = 3;
	flat.sector_grid[1][0] = 2, flat.sector_grid[1][1] = 4;
	opennova::trn_extend_sector_grid(flat);
	TEST_EXPECT(flat.sector_grid[0][15] == 3 && flat.sector_grid[1][2] == 4 && flat.sector_grid[15][0] == 2 &&
	            flat.sector_grid[15][15] == 4);
	// Wrapped both ways: the 2 x 2 pattern repeated.
	TrnConfig tiled = flat;
	tiled.wrap_x = tiled.wrap_y = 1;
	tiled.sector_grid[0][2] = tiled.sector_grid[1][2] = 0;
	opennova::trn_extend_sector_grid(tiled);
	TEST_EXPECT(tiled.sector_grid[0][2] == 1 && tiled.sector_grid[0][3] == 3 && tiled.sector_grid[2][0] == 1 &&
	            tiled.sector_grid[3][1] == 4 && tiled.sector_grid[15][15] == 4 && tiled.sector_grid[14][14] == 1);
	// load_trn leaves the grid so extended.
	std::istringstream in("polytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\npolytrn_polydata h.cpt\r\n"
	                      "polytrn_sectorcount 2\r\npolytrn_wrapy 1\r\npolytrn_sectors 1 3\r\npolytrn_sectors 2 4\r\n");
	TrnConfig loaded;
	std::string error;
	TEST_EXPECT(opennova::load_trn(in, loaded, error));
	TEST_EXPECT(loaded.sector_grid[0][9] == 3 && loaded.sector_grid[2][0] == 1 && loaded.sector_grid[3][5] == 4);
	std::printf("grid extension: the last again, the cells again under a wrap, as load_trn leaves the grid\n");
	return 0;
}

int test_foliage_block() {
	opennova::FoliageDef shrub;
	shrub.graphic = "shrub.3di";
	shrub.match = {254, 253, opennova::FOLIAGE_MATCH_UNSET, opennova::FOLIAGE_MATCH_UNSET};
	shrub.color_upper = 2;
	shrub.attrib_flags = opennova::FOLIAGE_ATTRIB_FORCE_ON | opennova::FOLIAGE_ATTRIB_SHADOW;
	const std::string block = opennova::trn_foliage_block_text(shrub);
	TEST_EXPECT(block == "foliage\r\n  graphic         shrub.3di\r\n  color_lower     0\r\n  color_upper     2\r\n"
	                     "  match           254 253\r\n  attrib          shadow forceon\r\nend\r\n");
	// No graphic, no codes, no attrib: those lines left out.
	TEST_EXPECT(opennova::trn_foliage_block_text(opennova::FoliageDef()) ==
	            "foliage\r\n  color_lower     0\r\n  color_upper     0\r\nend\r\n");
	// save_trn writes each definition's block after a blank line, read back as the definition.
	TrnConfig config;
	config.colormap = "c.tga";
	config.detailmap = "d.tga";
	config.polydata = "h.cpt";
	config.foliage_defs = {shrub};
	std::ostringstream out;
	std::string error;
	TEST_EXPECT(opennova::save_trn(out, config, error));
	TEST_EXPECT(out.str().find("\r\n\r\n" + block) != std::string::npos);
	std::istringstream in(out.str());
	TrnConfig back;
	TEST_EXPECT(opennova::load_trn(in, back, error) && back.foliage_defs.size() == 1);
	TEST_EXPECT(back.foliage_defs.size() == 1 && back.foliage_defs[0].graphic == "shrub.3di" &&
	            back.foliage_defs[0].match == shrub.match && back.foliage_defs[0].attrib_flags == shrub.attrib_flags);
	std::printf("foliage block: save_trn's block text, read back as the definition\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_grid_extension();
	failures += test_foliage_block();
	if (failures == 0) std::printf("trn_helpers: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
