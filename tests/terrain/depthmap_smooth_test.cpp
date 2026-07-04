// Depthmap smoothing parity. The 8-bit -> 16-bit smoothing is
// `out[r][c] = 32 * (raw[r][c] + raw[r][c+1] + raw[r+1][c] + raw[r+1][c+1])`
// with wrap at 1024 on both axes. This test verifies the formula at
// (0,0), (511,511), and (1023,1023) — the three positions the scratch
// reference uses to pin down the implementation.

#include "common/test_paths.h"
#include "terrain/depthmap.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path raw_path =
        repo_root / "fixtures" / "terrain" / "sample" / "Sample_d.raw";
    if (!fs::exists(raw_path)) {
        std::fprintf(stderr, "FAIL: missing fixture %s\n",
                     raw_path.string().c_str());
        return 1;
    }

    auto raw = opennova::load_depthmap_raw(raw_path.string());
    if (check(raw.size() == 1024u * 1024u, "raw size != 1024*1024")) return 1;

    auto smoothed = opennova::smooth_depthmap(raw);
    if (check(smoothed.size() == 1024u * 1024u,
              "smoothed size != 1024*1024"))
        return 1;

    auto idx = [](int r, int c) {
        return (r & 0x3FF) * 1024 + (c & 0x3FF);
    };

    uint16_t expected_0_0 =
        static_cast<uint16_t>(32 * (raw[idx(0, 0)] + raw[idx(0, 1)] +
                                    raw[idx(1, 0)] + raw[idx(1, 1)]));
    if (smoothed[0] != expected_0_0) {
        std::fprintf(stderr,
                     "FAIL: smoothed[0] = %u, expected %u\n",
                     smoothed[0], expected_0_0);
        return 1;
    }

    uint16_t expected_511_511 = static_cast<uint16_t>(
        32 * (raw[idx(511, 511)] + raw[idx(511, 512)] +
              raw[idx(512, 511)] + raw[idx(512, 512)]));
    if (smoothed[511 * 1024 + 511] != expected_511_511) {
        std::fprintf(stderr,
                     "FAIL: smoothed[511,511] = %u, expected %u\n",
                     smoothed[511 * 1024 + 511], expected_511_511);
        return 1;
    }

    uint16_t expected_wrap = static_cast<uint16_t>(
        32 * (raw[idx(1023, 1023)] + raw[idx(1023, 0)] +
              raw[idx(0, 1023)] + raw[idx(0, 0)]));
    if (smoothed[1023 * 1024 + 1023] != expected_wrap) {
        std::fprintf(stderr,
                     "FAIL: smoothed[1023,1023] (wrap) = %u, expected %u\n",
                     smoothed[1023 * 1024 + 1023], expected_wrap);
        return 1;
    }

    std::printf("OK: depthmap smoothing matches at (0,0) (511,511) (1023,1023)\n");
    return 0;
}
