#include <formats/trn/trn_io.h>

#include <formats/foliage/foliage.h>
#include <formats/mission/bms.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "common/retail_paths.h"

namespace {

bool expect(bool condition, const char *message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

// A mission's BMS tile-set name replaces the .trn tilestrip: the extension
// from the first '.' becomes "TGA" (or "TGA" is appended); an empty name keeps
// the .trn value. [orig: Terrain_LoadEnvironmentConfig @ 0x6109C8..0x6109EE;
// Path_ReplaceOrAppendExtension @ 0x53C780]
bool test_mission_tilestrip() {
    opennova::TrnConfig trn;
    trn.tilestrip = "trntile10.tga";
    return expect(opennova::trn_mission_tilestrip(trn, "") == "trntile10.tga",
                   "an empty mission tile set keeps the .trn tilestrip") &&
           expect(opennova::trn_mission_tilestrip(trn, "trntilec1") == "trntilec1.TGA",
                   "a bare mission tile-set name gains .TGA") &&
           expect(opennova::trn_mission_tilestrip(trn, "TRNTILEA1.TGA") == "TRNTILEA1.TGA",
                   "an authored extension is replaced by TGA") &&
           expect(opennova::trn_mission_tilestrip(trn, "set.v2.bmp") == "set.TGA",
                   "everything from the first dot is replaced");
}

// Retail 06TR authors tile set "trntilec1" over G13.trn's trntile10: its
// .til roads draw from trntilec1.TGA. Gated on the extracted retail tree
// (docs/asset-gated-tests.md).
bool test_retail_06tr_draws_from_its_mission_tile_set() {
    const std::string bms_path = retail::asset_file("06TR.bms");
    const std::string trn_path = retail::asset_file("G13.trn");
    if (bms_path.empty() || trn_path.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS with 06TR.bms and G13.trn");
        return true;
    }
    std::ifstream bms_file(bms_path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(bms_file)),
                                     std::istreambuf_iterator<char>());
    opennova::bms::Header header;
    std::string error;
    if (!expect(bytes.size() >= opennova::bms::kHeaderSize &&
                        opennova::bms::parse_header_blob(bytes.data(), opennova::bms::kHeaderSize,
                                                         header, error),
                "06TR.bms header parses")) {
        return false;
    }
    std::ifstream trn_file(trn_path);
    opennova::TrnConfig trn;
    if (!expect(opennova::load_trn(trn_file, trn, error), "G13.trn parses")) {
        return false;
    }
    const std::string tile_set(header.terrain_tile,
                               ::strnlen(header.terrain_tile, sizeof(header.terrain_tile)));
    return expect(retail::lower_ascii(trn.tilestrip) == "trntile10.tga",
                   "G13.trn authors the trntile10 strip") &&
           expect(retail::lower_ascii(opennova::trn_mission_tilestrip(trn, tile_set)) ==
                          "trntilec1.tga",
                   "06TR's header tile set replaces it with trntilec1");
}

} // namespace

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    if (!test_mission_tilestrip() || !test_retail_06tr_draws_from_its_mission_tile_set()) {
        return 1;
    }
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
    saved.lock_bottomleft = {0, 1};
    saved.lock_bottomright = {1, 1};
    saved.horizon = 1234.5;
    saved.charmap = "roundtrip_char.tga";
    saved.foliagemap = "roundtrip_foliage.tga";
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
    foliage_a.match = {2, 250, -1, -1};
    saved.foliage_defs.push_back(foliage_a);

    opennova::FoliageDef foliage_b;
    foliage_b.graphic = "tree_b";
    foliage_b.color_lower = static_cast<int>(opennova::FoliageColorMode::MatchGround);
    foliage_b.color_upper = static_cast<int>(opennova::FoliageColorMode::Blend50);
    saved.foliage_defs.push_back(foliage_b);

    std::string error;
    std::ostringstream out;
    if (!opennova::save_trn(out, saved, error)) {
        std::fprintf(stderr, "FAIL: save_trn failed: %s\n", error.c_str());
        return 1;
    }

    opennova::TrnConfig loaded;
    std::istringstream in(out.str());
    error.clear();
    if (!opennova::load_trn(in, loaded, error)) {
        std::fprintf(stderr, "FAIL: load_trn failed: %s\n", error.c_str());
        return 1;
    }

    if (!expect(loaded.name == saved.name, "terrain name should round-trip")) return 1;
    if (!expect(loaded.detail_density == saved.detail_density, "detail_density should round-trip")) return 1;
    if (!expect(loaded.detail_density2 == saved.detail_density2, "detail_density2 should round-trip")) return 1;
    if (!expect(loaded.wrap_x == saved.wrap_x, "wrap_x should round-trip")) return 1;
    if (!expect(loaded.wrap_y == saved.wrap_y, "wrap_y should round-trip")) return 1;
    if (!expect(loaded.lock_topleft.x == saved.lock_topleft.x &&
               loaded.lock_topleft.y == saved.lock_topleft.y,
               "lock_topleft should round-trip")) return 1;
    if (!expect(loaded.lock_bottomright.x == saved.lock_bottomright.x &&
               loaded.lock_bottomright.y == saved.lock_bottomright.y,
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
    if (!expect(loaded.foliage_defs[0].match == saved.foliage_defs[0].match, "foliage_defs match should round-trip")) return 1;
    if (!expect(loaded.foliage_defs[1].graphic == saved.foliage_defs[1].graphic, "multiple foliage_defs should round-trip")) return 1;

    if (!expect(loaded.sector_grid[0][0] == 1 && loaded.sector_grid[0][1] == 2 && loaded.sector_grid[0][2] == 3,
                "explicit sector grid values should round-trip")) return 1;
    if (!expect(loaded.sector_grid[0][3] == 4, "the fourth explicit column should round-trip")) return 1;
    if (!expect(loaded.sector_grid[0][4] == 1, "wrap_x should pad sectors cyclically")) return 1;
    if (!expect(loaded.sector_grid[2][1] == loaded.sector_grid[0][1], "wrap_y should pad rows cyclically")) return 1;

    std::printf("OK: trn config round-trip preserved terrain metadata\n");
    return 0;
}
