#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	opennova::TrnConfig saved;
	saved.name = "RoundtripTerrain";
	saved.colormap = "roundtrip_c.tga";
	saved.detailmap_c1 = "roundtrip_dc1.tga";
	saved.detailmap_c2 = "roundtrip_dc2.tga";
	saved.detailmap_c3 = "roundtrip_dc3.tga";
	saved.detailblendmap = "roundtrip_d1.tga";
	saved.polydata = "roundtrip.cpt";
	saved.detailmap = "roundtrip_dm.tga";
	saved.detailmap2 = "roundtrip_dm2.tga";
	saved.detailmapdist = "roundtrip_dmd.tga";
	saved.detailmapdist2 = "roundtrip_dmd2.tga";
	saved.detail_density = 192;
	saved.detail_density2 = 12;
	// The retail admission gate needs a power-of-two sector count.
	saved.sector_count = 4;
	saved.sector_rows = 2;
	saved.origin_x = -4;
	saved.origin_y = 7;
	saved.water_height = 42;
	saved.wrap_x = 1;
	saved.wrap_y = 1;
	saved.lock_topleft = {0, 1};
	saved.lock_topright = {1, 0};
	saved.lock_bottomleft = {2, 3};
	saved.lock_bottomright = {4, 5};
	saved.horizon = 1234.5;
	saved.charmap = "roundtrip_char.pcx";
	saved.foliagemap = "roundtrip_foliage.pcx";
	saved.tilestrip = "roundtrip_tilestrip.tga";
	saved.tileinfo = "roundtrip_tileinfo";
	saved.sector_grid[0][0] = 1;
	saved.sector_grid[0][1] = 2;
	saved.sector_grid[0][2] = 3;
	saved.sector_grid[0][3] = 4;
	saved.sector_grid[1][0] = 4;
	saved.sector_grid[1][1] = 0;
	saved.sector_grid[1][2] = 1;
	saved.sector_grid[1][3] = 2;
	opennova::FoliageDef foliage_a;
	foliage_a.graphic = "tree_a";
	foliage_a.color_lower = static_cast<int>(opennova::FoliageColorMode::Blend50);
	foliage_a.color_upper = static_cast<int>(opennova::FoliageColorMode::RetainFullColor);
	// All four consumed codes on one `match` line.
	foliage_a.match = {2, 5, 9, 250};
	foliage_a.attrib_flags = opennova::FOLIAGE_ATTRIB_SHADOW;
	saved.foliage_defs.push_back(foliage_a);

	opennova::FoliageDef foliage_b;
	foliage_b.graphic = "tree_b";
	foliage_b.color_lower = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	foliage_b.color_upper = static_cast<int>(opennova::FoliageColorMode::Blend50);
	foliage_b.match[0] = 12;
	foliage_b.attrib_flags = opennova::FOLIAGE_ATTRIB_FORCE_ON;
	saved.foliage_defs.push_back(foliage_b);

	std::string error;
	std::ostringstream output;
	if (!opennova::save_trn(output, saved, error)) {
		std::fprintf(stderr, "FAIL: save_trn failed: %s\n", error.c_str());
		return 1;
	}

	opennova::TrnConfig loaded;
	std::istringstream input(output.str());
	error.clear();
	if (!opennova::load_trn(input, loaded, error)) {
		std::fprintf(stderr, "FAIL: load_trn failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(loaded.name == saved.name, "terrain name should round-trip")) return 1;
	if (!expect(loaded.detail_density == saved.detail_density, "detail_density should round-trip")) return 1;
	if (!expect(loaded.detail_density2 == saved.detail_density2, "detail_density2 should round-trip")) return 1;
	if (!expect(loaded.wrap_x == saved.wrap_x, "wrap_x should round-trip")) return 1;
	if (!expect(loaded.wrap_y == saved.wrap_y, "wrap_y should round-trip")) return 1;
	if (!expect(loaded.lock_topleft.x == 0 && loaded.lock_topleft.y == 1,
			"lock_topleft should round-trip")) return 1;
	if (!expect(loaded.lock_topright.x == 1 && loaded.lock_topright.y == 0,
			"lock_topright should round-trip")) return 1;
	if (!expect(loaded.lock_bottomleft.x == 2 && loaded.lock_bottomleft.y == 3,
			"lock_bottomleft should round-trip")) return 1;
	if (!expect(loaded.lock_bottomright.x == 4 && loaded.lock_bottomright.y == 5,
			"lock_bottomright should round-trip")) return 1;
	if (!expect(std::abs(loaded.horizon - saved.horizon) < 0.0001, "horizon should round-trip")) return 1;
	if (!expect(loaded.charmap == saved.charmap, "charmap should round-trip")) return 1;
	if (!expect(loaded.foliagemap == saved.foliagemap, "foliagemap should round-trip")) return 1;
	if (!expect(loaded.tilestrip == saved.tilestrip, "tilestrip should round-trip")) return 1;
	if (!expect(loaded.tileinfo == saved.tileinfo, "tileinfo should round-trip")) return 1;
	if (!expect(loaded.detailmapdist2 == saved.detailmapdist2, "detailmapdist2 should round-trip")) return 1;
	if (!expect(loaded.foliage_defs.size() == saved.foliage_defs.size(), "foliage_defs count should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].graphic == saved.foliage_defs[0].graphic, "foliage_defs graphic should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].color_lower == saved.foliage_defs[0].color_lower, "foliage_defs color_lower should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].color_upper == saved.foliage_defs[0].color_upper, "foliage_defs color_upper should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].attrib_flags == saved.foliage_defs[0].attrib_flags, "foliage_defs attrib_flags should round-trip")) return 1;
	if (!expect(output.str().find("  match           2 5 9 250\r\n") != std::string::npos,
			"save_trn should write every authored match code on the one match line")) return 1;
	if (!expect(loaded.foliage_defs[0].match == saved.foliage_defs[0].match,
			"four foliage match codes should round-trip")) return 1;
	if (!expect(opennova::foliage_def_match_count(loaded.foliage_defs[0]) == 4,
			"four authored codes should all be counted")) return 1;
	if (!expect(loaded.foliage_defs[1].match == saved.foliage_defs[1].match,
			"a single foliage match code should round-trip with an unset tail")) return 1;
	if (!expect(loaded.foliage_defs[1].graphic == saved.foliage_defs[1].graphic, "multiple foliage_defs should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[1].attrib_flags == saved.foliage_defs[1].attrib_flags, "multiple foliage attrib_flags should round-trip")) return 1;
	if (!expect(loaded.sector_grid[0][0] == 1 && loaded.sector_grid[0][1] == 2 && loaded.sector_grid[0][2] == 3,
	            "explicit sector grid values should round-trip")) return 1;
	if (!expect(loaded.sector_grid[0][3] == 4, "the fourth explicit column should round-trip")) return 1;
	if (!expect(loaded.sector_grid[0][4] == 1, "wrap_x should pad sectors cyclically")) return 1;
	if (!expect(loaded.sector_grid[2][1] == loaded.sector_grid[0][1], "wrap_y should pad rows cyclically")) return 1;

	// Retail stores up to seven `match` args but consumes four: the parser
	// keeps the first four and drops the rest; a normalized def compacts
	// authored codes to the front.
	{
		opennova::TrnConfig seven;
		std::istringstream seven_input(
				"polytrn_colormap c.tga\npolytrn_detailmap d.tga\npolytrn_polydata p.cpt\n"
				"foliage\n  graphic bush.3di\n  match 1 2 3 4 5 6 7\nend\n");
		error.clear();
		if (!expect(opennova::load_trn(seven_input, seven, error),
				"a seven-arg match line should parse")) return 1;
		if (!expect(seven.foliage_defs.size() == 1 &&
						seven.foliage_defs[0].match == std::array<int, 4>{1, 2, 3, 4},
				"only the first four match args are consumed")) return 1;
		opennova::FoliageDef gapped;
		gapped.graphic = "gap";
		gapped.match = {-1, 7, 300, 3};
		const opennova::FoliageDef normalized = opennova::foliage_normalize_def(gapped);
		if (!expect(normalized.match == std::array<int, 4>{7, 255, 3, -1},
				"normalization clamps each code to a byte and compacts authored codes")) return 1;
	}

	// A config with only the admission-gate minimum (colormap, detailmap,
	// polydata) parses with default locks.
	opennova::TrnConfig defaults;
	std::istringstream defaults_input(
			"terrain_name default_locks\npolytrn_colormap c.tga\npolytrn_detailmap d.tga\n"
			"polytrn_polydata p.cpt\n");
	error.clear();
	if (!expect(opennova::load_trn(defaults_input, defaults, error),
			"TRN with omitted lock keys should parse")) return 1;
	for (const auto &lock : defaults.get_quadrant_locks()) {
		if (!expect(lock.x == 0 && lock.y == 0,
				"omitted lock keys should default to zero")) return 1;
	}
	std::ostringstream defaults_output;
	error.clear();
	if (!expect(opennova::save_trn(defaults_output, defaults, error),
			"TRN with default locks should save")) return 1;
	if (!expect(defaults_output.str().find("lock_") == std::string::npos,
			"default lock keys should remain omitted when saving")) return 1;

	std::printf("OK: trn round-trip preserved terrain metadata\n");
	return 0;
}
