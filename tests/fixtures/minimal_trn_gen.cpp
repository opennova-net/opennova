// Generator + guard for the minimal map's .trn terrain config (mnml.trn). The
// config ties the terrain set together: it names the .cpt polydata (produced by
// the packaging step's build_terrain run) plus the source art the packaging
// step authors (colormap/detailmaps/tilestrip TGAs, charmap/foliagemap PCX).
// Authored from scratch by libs/trn save_trn (no retail asset), modeled on
// fixtures/godot/dvxi5/Dvxi5.trn for a small flat map. The heavy .cpt/tiles are
// generate-at-package (not committed); this config IS committed + round-trip
// guarded. See fixtures/minimal/README.md.
//
// Emit with OPENNOVA_WRITE_MINIMAL_FIXTURES=1; else guard the config loads and
// carries the expected references.
#include <trn/trn_io.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

// The minimal flat map's terrain config. Names mirror the packaging step's
// output prefix (mnml) so the .cpt build output and the authored art line up.
TrnConfig build_config() {
	TrnConfig c;
	c.name = "mnml";
	c.polydata = "mnml.cpt";       // build_terrain output (generate-at-package)
	c.colormap = "mnml_c.tga";     // authored source art (packaging step)
	c.detailmap = "mnml_dm.tga";
	c.detailmap_c1 = "mnml_dc1.tga";
	c.charmap = "mnml_m.pcx";      // surface map (all one surface for the flat map)
	c.foliagemap = "mnml_f.pcx";   // foliage placement (empty = no foliage)
	c.tilestrip = "mnml_t.tga";
	c.detail_density = 128;
	c.detail_density2 = 8;
	c.sector_count = 1;            // one active sector for the smallest map
	c.origin_x = -4;
	c.origin_y = -4;
	c.water_height = 0;
	// A single active sector in the grid centre — mirrors Dvxi5's sparse grid.
	c.sector_grid[3][3] = 1;
	c.sector_rows = 8;
	return c;
}

} // namespace

int main() {
	const bool write_mode = std::getenv("OPENNOVA_WRITE_MINIMAL_FIXTURES") != nullptr;
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

	const std::string trn_path = path("mnml.trn");
	if (write_mode) {
		std::ofstream o(trn_path, std::ios::binary);
		o << text;
		std::printf("wrote %s (%zu bytes)\n", trn_path.c_str(), text.size());
	} else {
		std::ifstream f(trn_path, std::ios::binary);
		CHECK(f.good(), "committed mnml.trn missing — run with OPENNOVA_WRITE_MINIMAL_FIXTURES=1");
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
			}
		}
	}

	if (fail == 0) std::printf("OK: minimal mnml.trn config authored + round-trips\n");
	return fail == 0 ? 0 : 1;
}
