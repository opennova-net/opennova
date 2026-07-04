// Byte-identical bake parity across multiple fixtures. Replaces the
// single-fixture byte_parity_test. Returns nonzero if any case fails;
// keeps the test-binary count from ballooning while giving per-case
// status in ctest output.
//
// Cases share the convention `fixtures/terrain/<name>/<Capitalised>.tpj`
// with the golden CPT at `fixtures/terrain/<name>/<lowercase>.cpt`.

#include "terrain/builder.h"
#include "common/test_paths.h"

#include <tpj/tpj_io.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Case {
    const char *name;         // "sample", "gradient", …
    const char *tpj_basename; // "Sample", "Gradient", …
};

static std::vector<uint8_t> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char *>(buf.data()), sz);
    return buf;
}

static bool run_case(const fs::path &repo_root, const Case &tc) {
    const fs::path fixture_dir = repo_root / "fixtures" / "terrain" / tc.name;
    const fs::path tpj_path = fixture_dir / (std::string(tc.tpj_basename) + ".tpj");
    const fs::path golden_path = fixture_dir / (std::string(tc.name) + ".cpt");

    if (!fs::exists(tpj_path) || !fs::exists(golden_path)) {
        std::fprintf(stderr,
                     "SKIP %-10s — missing fixture under %s\n",
                     tc.name, fixture_dir.string().c_str());
        return true; // not a hard failure; keeps the test usable on fresh clones
    }

    opennova::TpjProject project;
    try {
        project = opennova::load_tpj_file(tpj_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %-10s — load_tpj_file: %s\n", tc.name, e.what());
        return false;
    }
    if (!fs::exists(project.path)) {
        project.path = fixture_dir.string();
    }

    const fs::path output_dir =
        fs::path(test_paths_temp_dir()) / (std::string("opennova_parity_") + tc.name);
    fs::remove_all(output_dir);
    fs::create_directories(output_dir);

    try {
        opennova::build_terrain(project, output_dir.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %-10s — build: %s\n", tc.name, e.what());
        fs::remove_all(output_dir);
        return false;
    }

    const fs::path generated_path = output_dir / (project.output + ".cpt");
    auto generated = read_file(generated_path);
    auto golden = read_file(golden_path);

    if (generated.empty()) {
        std::fprintf(stderr, "FAIL %-10s — bake did not produce %s\n",
                     tc.name, generated_path.string().c_str());
        fs::remove_all(output_dir);
        return false;
    }
    if (generated.size() != golden.size()) {
        std::fprintf(stderr,
                     "FAIL %-10s — size generated=%zu golden=%zu\n",
                     tc.name, generated.size(), golden.size());
        fs::remove_all(output_dir);
        return false;
    }

    size_t first_diff = 0;
    size_t diff_count = 0;
    for (size_t i = 0; i < generated.size(); ++i) {
        if (generated[i] != golden[i]) {
            if (diff_count == 0) first_diff = i;
            ++diff_count;
        }
    }
    fs::remove_all(output_dir);

    if (diff_count != 0) {
        std::fprintf(stderr,
                     "FAIL %-10s — %zu bytes differ (first at 0x%zx, size=%zu)\n",
                     tc.name, diff_count, first_diff, generated.size());
        return false;
    }

    std::printf("OK   %-10s — byte-identical CPT (%zu bytes)\n",
                tc.name, generated.size());
    return true;
}

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const Case cases[] = {
        {"sample", "Sample"},
        {"gradient", "Gradient"},
        {"checker64", "Checker64"},
        {"perlin", "Perlin"},
    };

    int failures = 0;
    for (const auto &tc : cases) {
        if (!run_case(repo_root, tc)) ++failures;
    }

    if (failures != 0) {
        std::fprintf(stderr, "\n%d of %d parity cases failed\n",
                     failures, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
        return 1;
    }
    return 0;
}
