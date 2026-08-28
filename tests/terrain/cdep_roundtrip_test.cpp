// Stand-alone CDEP encode/decode round-trip. Independent of the bake: a
// synthesized 1024x1024 uint16 depth buffer (a 14-bit pattern plus a ramp
// aligned to the encoder's 4096-sample blocks, so blocks carry both narrow
// and wide deltas) goes into a CptFile with CDEP format, is written to a
// tmp file, read back, and must match sample for sample.
//
// Catches encoder regressions that the full-bake byte-parity tests might
// mask.

#include <formats/cpt/cpt.h>
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    constexpr size_t kDim = 1024;
    std::vector<uint16_t> original(kDim * kDim);
    for (size_t z = 0; z < kDim; ++z) {
        for (size_t x = 0; x < kDim; ++x) {
            const size_t i = z * kDim + x;
            // Per-block ramp: block b (4096 samples) sits b * 3 higher, so a
            // block's [min, max] range and its 4-bit delta width both vary.
            const uint16_t block_base = static_cast<uint16_t>((i / 4096u) * 3u);
            original[i] = static_cast<uint16_t>(block_base + ((x * 7u + z * 13u) & 0x3FFFu));
        }
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
