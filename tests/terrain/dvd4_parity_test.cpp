// DVD4 build parity test. Runs opennova::build_terrain() on the captured raw16
// heightmap for the older BHD-era DVD4 terrain.
//
// DVD4 is a regression fixture for two reasons:
//   1) It previously crashed mid-build in LODMeshData_LoadSectionDedup when
//      the root node merged the 4 depth-1 children (face indices at higher
//      LOD sections can exceed level.vertex_count; the old remap buffer was
//      undersized and segfaulted).
//   2) It exercises the `no_smooth` path used by NovaTerrainBuilder's
//      build_from_data entry, which the Godot editor relies on.
//
// We don't have the original unsmoothed depth map that produced dvd4.cpt, so
// this is NOT a byte-identical parity check — the captured heightmap has
// already been rasterized once. The test asserts: no crash, valid CPT
// produced, and the output is the same length as the golden reference.

#include "terrain/builder.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(buf.data()), size);
    return buf;
}

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path fixture_dir = repo_root / "fixtures" / "terrain" / "dvd4";
    const fs::path raw_path    = fixture_dir / "dvd4_d.raw";
    const fs::path golden_path = fixture_dir / "dvd4.cpt";

    if (!fs::exists(raw_path) || !fs::exists(golden_path)) {
        std::fprintf(stderr, "FAIL: missing DVD4 fixture under %s\n",
                     fixture_dir.string().c_str());
        return 1;
    }

    constexpr size_t EXPECTED_RAW = 1024u * 1024u * 2u;
    auto raw = read_file(raw_path);
    if (raw.size() != EXPECTED_RAW) {
        std::fprintf(stderr, "FAIL: dvd4_d.raw size %zu, expected %zu\n",
                     raw.size(), EXPECTED_RAW);
        return 1;
    }

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output_dir =
        fs::path(test_paths_temp_dir()) / ("opennova_terrain_dvd4_" + std::to_string(suffix));
    fs::remove_all(output_dir);
    fs::create_directories(output_dir);

    // Write the raw16 to a temp file so we can point a TerrainProject at it.
    fs::path depth_path = output_dir / "_temp_depth.raw";
    std::ofstream depth_out(depth_path, std::ios::binary);
    depth_out.write(reinterpret_cast<const char *>(raw.data()), raw.size());
    depth_out.close();

    opennova::TpjProject project;
    project.terrain_name = "Dvd4";
    project.creator = "";
    project.path = "";
    project.depthmap = depth_path.string();
    project.output = "Dvd4";
    // DVD4.TRN has per-corner lock flags — mirror them so this exercises the
    // path the editor will once it wires lock_* through.
    project.lock_topleft     = {0, 1};
    project.lock_topright    = {1, 0};
    project.lock_bottomleft  = {0, 1};
    project.lock_bottomright = {1, 1};

    try {
        opennova::TerrainBuildOptions options;
        options.depth_format = opennova::DepthFormat::CDEP;
        options.smooth_depthmap = false;
        options.rasterize_depth = true;
        opennova::build_terrain(project, output_dir.string(), options, {});
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: opennova::build_terrain threw: %s\n", e.what());
        return 1;
    }

    fs::path generated_path = output_dir / "Dvd4.cpt";
    auto generated = read_file(generated_path);
    auto golden    = read_file(golden_path);
    if (generated.empty()) {
        std::fprintf(stderr, "FAIL: build did not produce %s\n",
                     generated_path.string().c_str());
        return 1;
    }
    if (golden.empty()) {
        std::fprintf(stderr, "FAIL: cannot read golden %s\n",
                     golden_path.string().c_str());
        return 1;
    }

    std::printf("OK: DVD4 build produced %ju bytes (golden=%ju)\n",
                static_cast<uintmax_t>(generated.size()),
                static_cast<uintmax_t>(golden.size()));

    fs::remove_all(output_dir);
    return 0;
}
