// CDEP overflow handling regression. The encoder's bits_per_delta field is
// 4 bits, so any 256-pixel block whose max - min exceeds 32 767 used to
// silently truncate the field to 0 and desync the rest of the depth
// section.
//
// Current behavior: the encoder clamps over-range blocks to the 15-bit
// limit and prints a warning on stderr. The bake completes and the
// resulting CPT round-trips cleanly. This test drives a synthetic
// heightmap with a 40 000-raw-unit cliff, bakes it, and asserts:
//   (1) bake succeeds (no exception),
//   (2) the written .cpt decodes via CptFile::read without error,
//   (3) no decoded block has range > 32 767.

#include "terrain/builder.h"
#include "common/test_paths.h"

#include <cpt/cpt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    constexpr int HM_SIZE = 1024;
    std::vector<uint16_t> heightmap(HM_SIZE * HM_SIZE, 0);

    // Force a 40 000-unit gradient into block 0 of row 100 — well past
    // CDEP's 32 767 ceiling. The encoder must clamp rather than desync.
    heightmap[100 * HM_SIZE + 100] = 40000;

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output_dir =
        fs::path(test_paths_temp_dir()) / ("opennova_terrain_cdep_overflow_" + std::to_string(suffix));
    fs::remove_all(output_dir);
    fs::create_directories(output_dir);

    fs::path depth_path = output_dir / "_temp_depth.raw";
    {
        std::ofstream depth_out(depth_path, std::ios::binary);
        depth_out.write(reinterpret_cast<const char *>(heightmap.data()),
                        heightmap.size() * sizeof(uint16_t));
    }

    opennova::TpjProject project;
    project.terrain_name = "CdepOverflow";
    project.creator = "";
    project.path = "";
    project.depthmap = depth_path.string();
    project.output = "CdepOverflow";

    try {
        opennova::TerrainBuildOptions options;
        options.depth_format = opennova::DepthFormat::CDEP;
        options.smooth_depthmap = false;
        options.rasterize_depth = true;
        opennova::build_terrain(project, output_dir.string(), options, {});
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: bake threw unexpectedly: %s\n", e.what());
        fs::remove_all(output_dir);
        return 1;
    }

    fs::path cpt_path = output_dir / "CdepOverflow.cpt";
    opennova::CptFile cpt;
    try {
        cpt = opennova::CptFile::read(cpt_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: CPT read threw: %s\n", e.what());
        fs::remove_all(output_dir);
        return 1;
    }

    if (cpt.depth_buffer.size() != static_cast<size_t>(HM_SIZE * HM_SIZE)) {
        std::fprintf(stderr, "FAIL: depth buffer size %zu, expected %d\n",
                     cpt.depth_buffer.size(), HM_SIZE * HM_SIZE);
        fs::remove_all(output_dir);
        return 1;
    }

    for (int b = 0; b < 4096; ++b) {
        uint16_t lo = 65535, hi = 0;
        for (int i = 0; i < 256; ++i) {
            uint16_t v = cpt.depth_buffer[b * 256 + i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        int rng = hi - lo;
        if (rng > 32767) {
            std::fprintf(stderr,
                         "FAIL: decoded block %d still over limit (range=%d)\n",
                         b, rng);
            fs::remove_all(output_dir);
            return 1;
        }
    }

    std::printf("OK: bake clamped over-range block, CPT round-trips within limit\n");
    fs::remove_all(output_dir);
    return 0;
}
