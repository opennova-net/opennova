#include <tpj/tpj.h>
#include <tpj/tpj_io.h>

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
	opennova::TpjProject saved;
	saved.terrain_name = "RoundtripMap";
	saved.creator = "tester";
	saved.path = "C:\\RoundtripMap\\";
	saved.depthmap = "RoundtripMap_depth.raw";
	saved.output = "RoundtripMap";
	saved.lock_topleft = {1, 2};
	saved.lock_topright = {3, 4};
	saved.lock_bottomleft = {5, 6};
	saved.lock_bottomright = {7, 8};
	saved.charmap = "roundtrip_char.pcx";
	saved.foliagemap = "roundtrip_foliage.pcx";
	saved.tilestrip = "roundtrip_tilestrip.tga";
	saved.tileinfo = "roundtrip_tileinfo";
	opennova::FoliageDef foliage_a;
	foliage_a.graphic = "tree_a.3di";
	foliage_a.color_lower = static_cast<int>(opennova::FoliageColorMode::Blend50);
	foliage_a.color_upper = static_cast<int>(opennova::FoliageColorMode::RetainFullColor);
	foliage_a.match = 0;
	foliage_a.attrib_flags = opennova::FOLIAGE_ATTRIB_SHADOW;
	saved.foliage_defs.push_back(foliage_a);

	opennova::FoliageDef foliage_b;
	foliage_b.graphic = "tree_b.3di";
	foliage_b.color_lower = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	foliage_b.color_upper = static_cast<int>(opennova::FoliageColorMode::Blend50);
	foliage_b.match = 1;
	foliage_b.attrib_flags = opennova::FOLIAGE_ATTRIB_FORCE_ON;
	saved.foliage_defs.push_back(foliage_b);
	saved.has_metadata = true;

	std::string error;
	std::ostringstream output;
	if (!opennova::save_tpj(output, saved, error)) {
		std::fprintf(stderr, "FAIL: save_tpj failed: %s\n", error.c_str());
		return 1;
	}

	opennova::TpjProject loaded;
	std::istringstream input(output.str());
	error.clear();
	if (!opennova::load_tpj(input, loaded, error)) {
		std::fprintf(stderr, "FAIL: load_tpj failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(loaded.terrain_name == saved.terrain_name, "terrain_name should round-trip")) return 1;
	if (!expect(loaded.creator == saved.creator, "creator should round-trip")) return 1;
	if (!expect(loaded.path == saved.path, "path should round-trip")) return 1;
	if (!expect(loaded.depthmap == saved.depthmap, "depthmap should round-trip")) return 1;
	if (!expect(loaded.output == saved.output, "output should round-trip")) return 1;
	if (!expect(loaded.lock_topleft.x == saved.lock_topleft.x && loaded.lock_topleft.y == saved.lock_topleft.y,
	            "lock_topleft should round-trip")) return 1;
	if (!expect(loaded.lock_bottomright.x == saved.lock_bottomright.x &&
	                loaded.lock_bottomright.y == saved.lock_bottomright.y,
	            "lock_bottomright should round-trip")) return 1;
	if (!expect(loaded.charmap == saved.charmap, "charmap should round-trip")) return 1;
	if (!expect(loaded.foliagemap == saved.foliagemap, "foliagemap should round-trip")) return 1;
	if (!expect(loaded.tilestrip == saved.tilestrip, "tilestrip should round-trip")) return 1;
	if (!expect(loaded.tileinfo == saved.tileinfo, "tileinfo should round-trip")) return 1;
	if (!expect(loaded.has_metadata, "has_metadata should remain true when metadata exists")) return 1;
	if (!expect(loaded.foliage_defs.size() == 2, "foliage_defs count should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].graphic == "tree_a.3di", "first foliage graphic should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[0].attrib_flags == opennova::FOLIAGE_ATTRIB_SHADOW, "first foliage attrib_flags should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[1].match == 1, "second foliage match should round-trip")) return 1;
	if (!expect(loaded.foliage_defs[1].attrib_flags == opennova::FOLIAGE_ATTRIB_FORCE_ON, "second foliage attrib_flags should round-trip")) return 1;

	std::printf("OK: tpj round-trip preserved authoring metadata\n");
	return 0;
}
