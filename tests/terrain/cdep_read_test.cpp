// CDEP header parity over the synthetic map fixtures/terrain/tmap/Tmap.cpt
// (tests/fixtures/minimal_terrain_gen.cpp): the header decode, the depth
// buffer shape and the full 1024-to-64 tile quadtree the generator mints.
// The retail CDEP files are swept by cpt_jo_assets_sweep_test behind
// OPENNOVA_JO_ASSETS.

#include <formats/cpt/cpt.h>
#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path cpt_path =
        repo_root / "fixtures" / "terrain" / "tmap" / "Tmap.cpt";
    if (!fs::exists(cpt_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n",
                     cpt_path.string().c_str());
        return 1;
    }

    opennova::CptFile cpt;
    try {
        cpt = opennova::CptFile::read(cpt_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: CptFile::read threw: %s\n", e.what());
        return 1;
    }

    std::string name = cpt.header.terrain_name;
    std::string creator = cpt.header.creator;

    if (name != "Tmap") {
        std::fprintf(stderr, "FAIL: terrain_name = \"%s\", expected \"Tmap\"\n",
                     name.c_str());
        return 1;
    }
    if (creator != "OpenNova") {
        std::fprintf(stderr, "FAIL: creator = \"%s\", expected \"OpenNova\"\n",
                     creator.c_str());
        return 1;
    }
    if (cpt.depth_format != opennova::DepthFormat::CDEP) {
        std::fprintf(stderr, "FAIL: depth_format is not CDEP\n");
        return 1;
    }
    if (cpt.depth_buffer.size() != 1024u * 1024u) {
        std::fprintf(stderr,
                     "FAIL: depth_buffer size = %zu, expected %u\n",
                     cpt.depth_buffer.size(), 1024u * 1024u);
        return 1;
    }
    // The generator's full quadtree: 1 + 4 + 16 + 64 + 256 tiles of 1024..64.
    if (cpt.tiles.size() != 341u) {
        std::fprintf(stderr, "FAIL: tiles = %zu, expected 341\n", cpt.tiles.size());
        return 1;
    }
    if (cpt.tiles.front().tile_size != 1024u || cpt.tiles.back().tile_size != 64u) {
        std::fprintf(stderr, "FAIL: tile sizes run 1024 first to 64 last\n");
        return 1;
    }
    for (const opennova::CptTile &tile : cpt.tiles) {
        if (tile.vertex_count != 25u || tile.is_single) {
            std::fprintf(stderr, "FAIL: every tile is a 5x5 vertex grid\n");
            return 1;
        }
        for (const opennova::CptTileLOD &lod : tile.lods) {
            if (lod.indices.size() != 96u || lod.is_strip || lod.max_index != 24u) {
                std::fprintf(stderr, "FAIL: every LOD is the 32-triangle list over the grid\n");
                return 1;
            }
        }
    }

    std::printf("OK: CDEP header parity — name=%s creator=%s fmt=CDEP buffer=%zu tiles=%zu\n",
                name.c_str(), creator.c_str(), cpt.depth_buffer.size(), cpt.tiles.size());
    return 0;
}
