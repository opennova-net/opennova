// Generator + guard for the minimal map's .trn terrain config (mnml.trn). The
// config ties the terrain set together: it names the committed .cpt polydata
// and terrain art bundled by the minimal package (colormap/detailmap/tilestrip
// TGAs and charmap/foliagemap PCXs). Generated from scratch with
// engine/formats/trn save_trn (no retail asset), it models a small flat map
// after the retail Dvxi5.trn layout and is guarded by a round-trip check.
//
// Emit with `--write`; else guard the config loads and
// carries the expected references.
#include <formats/trn/trn_io.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>

using namespace opennova;

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

std::string path(const char *name) { return std::string(GAME_ASSETS_DIR) + "/" + name; }

// The minimal flat map's terrain config. Its mnml prefix matches the committed
// polydata and terrain art bundled by minimal_pff_package.
TrnConfig build_config() {
	TrnConfig c;
	c.name = "mnml";
	c.polydata = "mnml.cpt";       // committed baked polydata
	c.colormap = "mnml_c.tga";     // committed terrain art
	c.detailmap = "mnml_dm.tga";
	c.detailmap_c1 = "mnml_dc1.tga";
	c.detailmap_c2 = "mnml_dc2.tga";
	c.detailmap_c3 = "mnml_dc3.tga";
	c.detailmapdist = "mnml_dmd.tga";
	c.detailblendmap = "mnml_d1.tga";
	c.charmap = "mnml_m.pcx";      // surface map (all one surface for the flat map)
	c.foliagemap = "mnml_f.pcx";   // foliage placement (empty = no foliage)
	c.tilestrip = "mnml_t.tga";
	c.detail_density = 128;
	c.detail_density2 = 8;
	// sector_count is the grid WIDTH, not the number of active sectors: save_trn
	// emits `sector_count` columns per polytrn_sectors row and load_trn reads that
	// many, edge-replicating the rest. A value of 1 therefore emits column 0 only
	// and silently drops every sector placed further right.
	c.sector_count = 8;
	c.origin_x = -4;
	c.origin_y = -4;
	c.water_height = 0;
	// The 2x2 quadrant block at rows/cols 3-4, addressing the four quadrants of the
	// 1024x1024 heightmap atlas (COORDS_ATLAS_SIZE). Byte-identical to the grid in
	// the retail Dvxi5.trn, preserving its retail-shaped layout (the synthetic
	// fixtures/terrain/tmap/Tmap.trn carries the same block).
	c.sector_grid[3][3] = 1;
	c.sector_grid[3][4] = 3;
	c.sector_grid[4][3] = 2;
	c.sector_grid[4][4] = 4;
	c.sector_rows = 8;
	return c;
}

} // namespace

int main(int argc, char **argv) {
	// `--write` regenerates the committed file; the ctest registration passes nothing.
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const TrnConfig cfg = build_config();

	std::ostringstream os;
	std::string err;
	CHECK(save_trn(os, cfg, err), err.c_str());
	const std::string text = os.str();

	// Round-trip: the emitted config loads back with its references intact.
	TrnConfig reloaded;
	std::istringstream is(text);
	CHECK(load_trn(is, reloaded, err), err.c_str());
	CHECK(reloaded.name == "mnml", "trn name round-trips");
	CHECK(reloaded.polydata == "mnml.cpt", "trn names the .cpt polydata");
	CHECK(reloaded.charmap == "mnml_m.pcx", "trn names the charmap");
	CHECK(reloaded.foliagemap == "mnml_f.pcx", "trn names the foliagemap");
	CHECK(reloaded.colormap == "mnml_c.tga", "trn names the colormap");
	// The sector grid is what gives the map ground. A grid that reloads all-zero
	// means save_trn dropped columns (sector_count is the grid WIDTH) and retail
	// gets a terrain with no active sectors — boot still succeeds, nothing renders.
	CHECK(reloaded.sector_count == 8, "trn keeps the 8-wide sector grid");
	CHECK(reloaded.sector_grid[3][3] == 1 && reloaded.sector_grid[3][4] == 3 &&
	              reloaded.sector_grid[4][3] == 2 && reloaded.sector_grid[4][4] == 4,
	      "trn round-trips the active quadrant block");

	const std::string trn_path = path("mnml.trn");
	if (write_mode) {
		std::ofstream o(trn_path, std::ios::binary);
		o << text;
		std::printf("wrote %s (%zu bytes)\n", trn_path.c_str(), text.size());
	} else {
		std::ifstream f(trn_path, std::ios::binary);
		CHECK(f.good(), "committed mnml.trn missing — run with --write");
		if (f.good()) {
			std::stringstream buf;
			buf << f.rdbuf();
			std::string committed = buf.str();
			static const char kLfs[] = "version https://git-lfs";
			if (committed.rfind(kLfs, 0) == 0) {
				std::printf("[skip] mnml.trn is an unpulled LFS pointer\n");
			} else {
				// .trn is text (EOL may differ) — assert it LOADS, the engine's contract.
				TrnConfig c;
				std::string e2;
				std::istringstream cis(committed);
				CHECK(load_trn(cis, c, e2), e2.c_str());
				CHECK(c.polydata == "mnml.cpt", "committed .trn names the polydata");
				// Every image the committed config names must be one the packager ships
				// (minimal_pff_package kResource) and the art validator checks; a name added
				// here without those two is a texture retail asks for and never finds.
				CHECK(c.colormap == "mnml_c.tga" && c.detailmap == "mnml_dm.tga" &&
				              c.detailmap_c1 == "mnml_dc1.tga" && c.detailmap_c2 == "mnml_dc2.tga" &&
				              c.detailmap_c3 == "mnml_dc3.tga" && c.detailmapdist == "mnml_dmd.tga" &&
				              c.detailblendmap == "mnml_d1.tga" && c.tilestrip == "mnml_t.tga",
				      "committed .trn names exactly the shipped mnml_* source art");
				CHECK(c.sector_grid[3][3] == 1 && c.sector_grid[3][4] == 3 &&
				              c.sector_grid[4][3] == 2 && c.sector_grid[4][4] == 4,
				      "committed .trn carries the active quadrant block");
			}
		}
	}

	if (fail == 0) std::printf("OK: minimal mnml.trn config authored + round-trips\n");
	return fail == 0 ? 0 : 1;
}
