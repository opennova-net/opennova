// Measure how much the bake's rasterization stage expands the per-block
// range of the heightmap. The editor guarantees `max - min <= 32767` per
// 256-px horizontal block on the INPUT; this tool reports the same
// statistic on the OUTPUT (the rasterized depth buffer that CDEP
// actually encodes).
//
// The gap between input and output is what tells us what safety margin
// the editor needs so a CDEP export never fails.
//
// Build: cmake -DBUILD_TOOLS=ON … && cmake --build build --target dvxi5_drift
// Run:   build/Release/dvxi5_drift.exe <path-to-depth.raw>

#include "terrain/builder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct BlockStats {
    int block_index;
    int row;
    int bx;
    int input_min, input_max;   // from pre-bake heightmap
    int output_min, output_max; // from post-bake Output.dep
    int input_range() const { return input_max - input_min; }
    int output_range() const { return output_max - output_min; }
    int drift() const { return output_range() - input_range(); }
};

static std::vector<uint16_t> read_u16(const fs::path &p, size_t expected) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint16_t> buf(expected);
    f.read(reinterpret_cast<char *>(buf.data()), buf.size() * 2);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <depth.raw>\n", argv[0]);
        return 2;
    }
    const fs::path raw_path = argv[1];
    constexpr int HM = 1024;
    constexpr int PIXELS = HM * HM;

    auto input = read_u16(raw_path, PIXELS);
    if (input.size() != PIXELS) {
        std::fprintf(stderr, "FAIL: couldn't read %s\n", raw_path.string().c_str());
        return 1;
    }

    const fs::path out_dir = fs::temp_directory_path() / "opennova_drift";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    fs::path copy_path = out_dir / "depth.raw";
    {
        std::ofstream out(copy_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(input.data()), input.size() * 2);
    }

    opennova::TpjProject project;
    project.terrain_name = "Drift";
    project.path = "";
    project.depthmap = copy_path.string();
    project.output = "Drift";

    try {
        opennova::TerrainBuildOptions options;
        options.depth_format = opennova::DepthFormat::CDEP;
        options.smooth_depthmap = false;
        options.rasterize_depth = true;
        opennova::build_terrain(project, out_dir.string(), options, {});
    } catch (const std::exception &e) {
        std::fprintf(stderr, "bake threw: %s\n", e.what());
        return 1;
    }

    // Output.dep sits alongside Drift.cpt with the rasterized depth.
    fs::path dep_path = out_dir / "DriftOutput.dep";
    auto output = read_u16(dep_path, PIXELS);
    if (output.size() != PIXELS) {
        std::fprintf(stderr, "FAIL: couldn't read %s\n", dep_path.string().c_str());
        return 1;
    }

    std::vector<BlockStats> stats(4096);
    int total_over = 0;
    int max_drift = 0;
    BlockStats worst = {};
    for (int b = 0; b < 4096; ++b) {
        BlockStats s = {b, b / 4, b % 4, 65535, 0, 65535, 0};
        int base = b * 256;
        for (int i = 0; i < 256; ++i) {
            int iv = input[base + i];
            int ov = output[base + i];
            if (iv < s.input_min)  s.input_min  = iv;
            if (iv > s.input_max)  s.input_max  = iv;
            if (ov < s.output_min) s.output_min = ov;
            if (ov > s.output_max) s.output_max = ov;
        }
        if (s.output_range() > 32767) ++total_over;
        if (s.drift() > max_drift) { max_drift = s.drift(); worst = s; }
        stats[b] = s;
    }

    // Histogram of drift.
    int buckets[11] = {0}; // 0, 1-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129-256, 257-512, 513-1024, >1024
    int negative = 0;
    for (const auto &s : stats) {
        int d = s.drift();
        if (d < 0) { ++negative; continue; }
        if (d == 0) ++buckets[0];
        else if (d <= 4)    ++buckets[1];
        else if (d <= 8)    ++buckets[2];
        else if (d <= 16)   ++buckets[3];
        else if (d <= 32)   ++buckets[4];
        else if (d <= 64)   ++buckets[5];
        else if (d <= 128)  ++buckets[6];
        else if (d <= 256)  ++buckets[7];
        else if (d <= 512)  ++buckets[8];
        else if (d <= 1024) ++buckets[9];
        else                ++buckets[10];
    }

    std::printf("Dvxi5 drift report\n");
    std::printf("  total blocks:                 4096\n");
    std::printf("  blocks with output range > 32767: %d\n", total_over);
    std::printf("  max drift (output_range - input_range): %d (at row %d bx %d, input=%d..%d output=%d..%d)\n",
                max_drift, worst.row, worst.bx,
                worst.input_min, worst.input_max, worst.output_min, worst.output_max);
    std::printf("  drift histogram (positive only, %d negatives ignored):\n", negative);
    const char *labels[] = {"  0", "1-4", "5-8", "9-16", "17-32", "33-64", "65-128", "129-256", "257-512", "513-1024", ">1024"};
    for (int i = 0; i < 11; ++i) {
        std::printf("    drift %-8s : %5d blocks\n", labels[i], buckets[i]);
    }

    // Also report blocks where the OUTPUT range exceeded 32767 — these are
    // the ones that would force CDEP clamping on encode.
    if (total_over > 0) {
        std::printf("\n  over-limit blocks (first 16):\n");
        int printed = 0;
        for (const auto &s : stats) {
            if (s.output_range() <= 32767) continue;
            std::printf("    row %4d bx %d: input=%5d..%5d (range=%d)  output=%5d..%5d (range=%d, +%d)\n",
                        s.row, s.bx,
                        s.input_min, s.input_max, s.input_range(),
                        s.output_min, s.output_max, s.output_range(),
                        s.drift());
            if (++printed >= 16) break;
        }
    }

    fs::remove_all(out_dir);
    return 0;
}
