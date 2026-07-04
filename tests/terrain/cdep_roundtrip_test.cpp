// Stand-alone CDEP encode/decode round-trip. Independent of the bake —
// just reads a raw 1024×1024 uint16 depth buffer (Output.dep), wraps it
// in a CptFile with CDEP format, writes to a tmp file, reads back, and
// asserts the decoded depth buffer matches byte-for-byte.
//
// Catches encoder regressions that the full-bake byte-parity tests
// might mask.

#include <cpt/cpt.h>
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path dep_path =
        repo_root / "fixtures" / "terrain" / "sample" / "Output.dep";
    if (!fs::exists(dep_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n",
                     dep_path.string().c_str());
        return 1;
    }

    std::ifstream f(dep_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "FAIL: can't open %s\n",
                     dep_path.string().c_str());
        return 1;
    }
    std::vector<uint16_t> original(1024u * 1024u);
    f.read(reinterpret_cast<char *>(original.data()),
           original.size() * sizeof(uint16_t));
    if (!f) {
        std::fprintf(stderr, "FAIL: short read from %s\n",
                     dep_path.string().c_str());
        return 1;
    }

    fs::path tmp_path =
        fs::path(test_paths_temp_dir()) / "cdep_roundtrip.cpt";

    opennova::CptFile cpt_out;
    cpt_out.header.magic = opennova::CptFile::MAGIC;
    std::strncpy(cpt_out.header.terrain_name, "RoundTrip",
                 sizeof(cpt_out.header.terrain_name) - 1);
    std::strncpy(cpt_out.header.creator, "Test",
                 sizeof(cpt_out.header.creator) - 1);
    cpt_out.depth_format = opennova::DepthFormat::CDEP;
    cpt_out.depth_buffer = original;

    try {
        cpt_out.write(tmp_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: CptFile::write threw: %s\n", e.what());
        return 1;
    }

    opennova::CptFile cpt_in;
    try {
        cpt_in = opennova::CptFile::read(tmp_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: CptFile::read threw: %s\n", e.what());
        fs::remove(tmp_path);
        return 1;
    }
    fs::remove(tmp_path);

    if (cpt_in.depth_format != opennova::DepthFormat::CDEP) {
        std::fprintf(stderr, "FAIL: round-trip did not preserve CDEP format\n");
        return 1;
    }
    if (cpt_in.depth_buffer.size() != original.size()) {
        std::fprintf(stderr,
                     "FAIL: size mismatch decoded=%zu original=%zu\n",
                     cpt_in.depth_buffer.size(), original.size());
        return 1;
    }
    if (cpt_in.depth_buffer != original) {
        // Localise first diff for triage.
        size_t first_diff = 0;
        for (size_t i = 0; i < original.size(); ++i) {
            if (cpt_in.depth_buffer[i] != original[i]) {
                first_diff = i;
                break;
            }
        }
        std::fprintf(stderr,
                     "FAIL: depth buffer mismatch at pixel %zu "
                     "(orig=%u, decoded=%u)\n",
                     first_diff, original[first_diff],
                     cpt_in.depth_buffer[first_diff]);
        return 1;
    }

    std::printf("OK: CDEP round-trip preserves all 1024x1024 samples\n");
    return 0;
}
