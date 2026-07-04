// TPM1 (.tml tile file) round-trip. The on-disk format has section
// headers that include stale in-memory pointers from the original
// writer; our reader/writer normalise these to zero, so a byte-compare
// against the fixture allows up to 4 bytes × 8 sections = 32 bytes of
// drift (matches the scratch reference's tolerance).

#include "terrain/mesh_data.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char *>(buf.data()), sz);
    return buf;
}

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path tml_path =
        repo_root / "fixtures" / "terrain" / "sample" / "S0_00_00.tml";
    if (!fs::exists(tml_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n",
                     tml_path.string().c_str());
        return 1;
    }

    auto golden = read_file(tml_path);
    if (golden.empty()) {
        std::fprintf(stderr, "FAIL: could not read %s\n",
                     tml_path.string().c_str());
        return 1;
    }

    fs::path tmp_path =
        fs::path(test_paths_temp_dir()) / "tpm1_roundtrip.tml";

    opennova::MeshData mesh;
    try {
        mesh = opennova::MeshData::read(tml_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: MeshData::read threw: %s\n", e.what());
        return 1;
    }

    try {
        mesh.write(tmp_path.string());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: MeshData::write threw: %s\n", e.what());
        return 1;
    }

    auto written = read_file(tmp_path);
    fs::remove(tmp_path);

    if (written.size() != golden.size()) {
        std::fprintf(stderr,
                     "FAIL: size mismatch written=%zu golden=%zu\n",
                     written.size(), golden.size());
        return 1;
    }

    int diff_count = 0;
    size_t first_diff = 0;
    for (size_t i = 0; i < golden.size(); ++i) {
        if (written[i] != golden[i]) {
            if (diff_count == 0) first_diff = i;
            ++diff_count;
        }
    }

    if (diff_count > 32) {
        std::fprintf(stderr,
                     "FAIL: %d bytes differ (first at 0x%zx); tolerance is 32\n",
                     diff_count, first_diff);
        return 1;
    }

    std::printf("OK: TPM1 round-trip preserves %zu bytes (%d within tolerance)\n",
                golden.size(), diff_count);
    return 0;
}
