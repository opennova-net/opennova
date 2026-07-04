// Dvxi5 bake repro. The Dvxi5 heightmap (committed as
// fixtures/terrain/dvxi5/Dvxi5_depth.raw) crashes the current bake with a
// non-std-exception after the editor's CDEP constraint passes it through.
// This test reproduces the crash in-tree so we can iterate on the fix
// without round-tripping through the Godot editor.
//
// Reports:
//   "OK: wrote N bytes"          — bake succeeded (test passes)
//   "FAIL: <std::exception>"     — bake threw, message captured (test fails)
//   "FAIL: SEH 0x... at addr"    — SEH translated to std::exception
//   "FAIL: unknown (caught ...)" — last-resort catch (should be rare)
//
// On MSVC, an SEH translator is installed at entry so access violations,
// divide-by-zero, stack overflows, etc. surface as std::runtime_error
// with code + address. The target TU is compiled with /EHa to allow this.

#include "terrain/builder.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <eh.h>
#include <windows.h>
static void seh_to_std(unsigned code, EXCEPTION_POINTERS *info) {
    const void *addr = info ? info->ExceptionRecord->ExceptionAddress : nullptr;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "SEH 0x%08X at %p (nested=%u)", code, addr,
                  info ? info->ExceptionRecord->NumberParameters : 0u);
    throw std::runtime_error(buf);
}
#endif

namespace fs = std::filesystem;

int main() {
#ifdef _MSC_VER
    _set_se_translator(&seh_to_std);
#endif

    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path fixture_dir = repo_root / "fixtures" / "terrain" / "dvxi5";
    const fs::path raw_path = fixture_dir / "Dvxi5_depth.raw";

    if (!fs::exists(raw_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n", raw_path.string().c_str());
        return 1;
    }

    constexpr size_t EXPECTED_RAW = 1024u * 1024u * 2u;
    std::ifstream f(raw_path, std::ios::binary | std::ios::ate);
    auto sz = f.tellg();
    if (static_cast<size_t>(sz) != EXPECTED_RAW) {
        std::fprintf(stderr, "FAIL: Dvxi5_depth.raw size %zu, expected %zu\n",
                     static_cast<size_t>(sz), EXPECTED_RAW);
        return 1;
    }
    f.seekg(0);
    std::vector<uint8_t> raw(EXPECTED_RAW);
    f.read(reinterpret_cast<char *>(raw.data()), raw.size());

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path output_dir =
        fs::path(test_paths_temp_dir()) / ("opennova_terrain_dvxi5_" + std::to_string(suffix));
    fs::remove_all(output_dir);
    fs::create_directories(output_dir);

    fs::path depth_path = output_dir / "_temp_depth.raw";
    {
        std::ofstream out(depth_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(raw.data()), raw.size());
    }

    opennova::TpjProject project;
    project.terrain_name = "Dvxi5";
    project.creator = "";
    project.path = "";
    project.depthmap = depth_path.string();
    project.output = "Dvxi5";

    std::fprintf(stderr, "[dvxi5_repro] starting bake...\n");
    try {
        opennova::TerrainBuildOptions options;
        options.depth_format = opennova::DepthFormat::CDEP;
        options.smooth_depthmap = false;
        options.rasterize_depth = true;
        opennova::build_terrain(project, output_dir.string(), options,
                       [](const opennova::TerrainBuildProgress &p) {
                           std::fprintf(stderr, "[dvxi5_repro] %s: %s\n",
                                        p.phase.c_str(), p.message.c_str());
                       });
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: bake threw: %s\n", e.what());
        fs::remove_all(output_dir);
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAIL: bake threw non-std exception (caught ...)\n");
        fs::remove_all(output_dir);
        return 1;
    }

    fs::path cpt_path = output_dir / "Dvxi5.cpt";
    std::error_code ec;
    auto out_size = fs::file_size(cpt_path, ec);
    if (ec) {
        std::fprintf(stderr, "FAIL: bake finished but %s is missing: %s\n",
                     cpt_path.string().c_str(), ec.message().c_str());
        fs::remove_all(output_dir);
        return 1;
    }

    std::printf("OK: wrote %ju bytes to %s\n",
                static_cast<uintmax_t>(out_size),
                cpt_path.string().c_str());
    fs::remove_all(output_dir);
    return 0;
}
