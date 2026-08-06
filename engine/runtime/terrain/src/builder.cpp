#include "terrain/builder.h"
#include "build_quadtree.h"
#include "cpt_export.h"
#include "depthmap.h"
#include "terrain_mesh.h"
#include "trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <io/log.h>

namespace fs = std::filesystem;

namespace opennova {

namespace {

constexpr int kLoadDepthUnits = 1;
constexpr int kBaseMeshUnits = 1;
constexpr int kWriteDepthUnits = 1;

int count_block_units(int tile_size, int min_tile_size) {
    int count = 0;
    int tile = tile_size;
    int mult = 1;
    while (tile >= min_tile_size) {
        count += mult;
        tile >>= 1;
        mult *= 4;
    }
    return count;
}

struct ProgressReporter {
    explicit ProgressReporter(const TerrainBuildProgressCallback& p_callback, int p_total_units)
        : callback(p_callback), total_units(std::max(p_total_units, 1)) {}

    void report(const std::string& phase,
                const std::string& message,
                int phase_current,
                int phase_total) const {
        if (!callback) {
            return;
        }

        const int safe_phase_total = std::max(phase_total, 1);
        const int clamped_phase_current = std::clamp(phase_current, 0, safe_phase_total);
        const int global_current = std::min(total_units, completed_units + clamped_phase_current);

        TerrainBuildProgress progress;
        progress.phase = phase;
        progress.message = message;
        progress.current = global_current;
        progress.total = total_units;
        progress.ratio = static_cast<float>(global_current) / static_cast<float>(total_units);
        callback(progress);
    }

    void finish_phase(const std::string& phase,
                      const std::string& message,
                      int phase_units) {
        completed_units = std::min(total_units, completed_units + std::max(phase_units, 0));
        if (!callback) {
            return;
        }

        TerrainBuildProgress progress;
        progress.phase = phase;
        progress.message = message;
        progress.current = completed_units;
        progress.total = total_units;
        progress.ratio = static_cast<float>(completed_units) / static_cast<float>(total_units);
        callback(progress);
    }

    void complete(const std::string& message) {
        completed_units = total_units;
        if (!callback) {
            return;
        }

        TerrainBuildProgress progress;
        progress.phase = "complete";
        progress.message = message;
        progress.current = total_units;
        progress.total = total_units;
        progress.ratio = 1.0f;
        callback(progress);
    }

    const TerrainBuildProgressCallback& callback;
    const int total_units;
    int completed_units = 0;
};

struct TraceGuard {
    ~TraceGuard() {
        if (g_trace) {
            std::fclose(g_trace);
            g_trace = nullptr;
        }
    }
};

} // namespace

// Global trace file pointer (declared in trace.h)
FILE* g_trace = nullptr;

// [orig: build_terrain_thread @ 0x4013A0; docs/terrain/terrain-re.md]
// Ported from build_terrain_thread (0x4013A0).
// Pipeline: load depth map -> smooth -> quadtree -> LOD -> tile files -> CPT export
void build_terrain(const TpjProject& project,
                   const std::string& output_dir,
                   const TerrainBuildOptions& options,
                   const TerrainBuildProgressCallback& progress_callback) {
    const bool apply_smoothing = options.smooth_depthmap;
    const bool should_rasterize_depth = options.rasterize_depth;
    const DepthFormat depth_format = options.depth_format;

    const char* trace_path = std::getenv("TRNGEN_TRACE");
    if (trace_path && trace_path[0] != '\0') {
        g_trace = std::fopen(trace_path, "w");
    }
    TraceGuard trace_guard;

    std::string base_path = project.path;
    for (auto& c : base_path) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (!base_path.empty() && base_path.back() != '/') {
        base_path += '/';
    }

    std::string depthmap_path = base_path + project.depthmap;
    if (!fs::exists(depthmap_path) && fs::exists(project.depthmap)) {
        depthmap_path = project.depthmap;
        base_path.clear();
    }

    constexpr int tile_size = 1024;
    constexpr int min_tile_size = 64;
    const int quadtree_units = count_block_units(tile_size, min_tile_size);
    const int cpt_units = count_block_units(tile_size, min_tile_size);
    const int tms_units = count_block_units(tile_size, min_tile_size);
    ProgressReporter progress(progress_callback,
                              kLoadDepthUnits + kBaseMeshUnits + quadtree_units +
                              kWriteDepthUnits + cpt_units + tms_units + cpt_units);

    std::string output_prefix = output_dir + "/" + project.output;
    fs::create_directories(output_dir);

    progress.report("load_depthmap",
                    apply_smoothing ? "Loading and smoothing depth map..." : "Loading 16-bit depth map...",
                    0, kLoadDepthUnits);

    std::vector<uint16_t> smoothed;
    if (!apply_smoothing) {
        opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Loading 16-bit depth map: %s", depthmap_path.c_str());
        smoothed = load_depthmap_raw16(depthmap_path);
    } else {
        opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Loading depth map: %s", depthmap_path.c_str());
        auto raw_depth = load_depthmap_raw(depthmap_path);
        opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Smoothing depth map...");
        smoothed = smooth_depthmap(raw_depth);
    }
    progress.finish_phase("load_depthmap",
                          apply_smoothing ? "Loaded and smoothed depth map." : "Loaded 16-bit depth map.",
                          kLoadDepthUnits);

    progress.report("base_meshes", "Generating base terrain meshes...", 0, kBaseMeshUnits);
    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Generating base terrain meshes...");
    set_mesh_corner_locks(
        project.lock_topleft.x, project.lock_topleft.y,
        project.lock_topright.x, project.lock_topright.y,
        project.lock_bottomleft.x, project.lock_bottomleft.y,
        project.lock_bottomright.x, project.lock_bottomright.y);
    generate_base_terrain_meshes();
    progress.finish_phase("base_meshes", "Generated base terrain meshes.", kBaseMeshUnits);

    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Building quadtree (tile=%d, min=%d)...", tile_size, min_tile_size);
    auto root = build_quadtree(tile_size, min_tile_size);

    std::vector<uint16_t> rasterized_depth = smoothed;
    QuadtreeContext ctx;
    ctx.tile_size = tile_size;
    ctx.min_tile_size = min_tile_size;
    ctx.depth_buffer = smoothed.data();
    ctx.rasterized_depth = &rasterized_depth;
    ctx.output_prefix = output_prefix;
    ctx.progress_callback = [&](int current, int total, const std::string& message) {
        opennova::io::logf(opennova::io::LogLevel::kInfo,
		"  %s", message.c_str());
        progress.report("quadtree", message, current, total);
    };

    progress.report("quadtree", "Processing quadtree...", 0, quadtree_units);
    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Processing quadtree...");
    process_quadtree(root.get(), ctx);
    progress.finish_phase("quadtree", "Processed quadtree.", quadtree_units);

    {
        const auto& dep_out = should_rasterize_depth ? rasterized_depth : smoothed;
        std::string dep_path = output_prefix + "Output.dep";
        progress.report("write_depth", "Writing Output.dep...", 0, kWriteDepthUnits);
        opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Writing %s...%s", dep_path.c_str(),
                    should_rasterize_depth ? "" : " (passthrough, no rasterize)");
        FILE* f = std::fopen(dep_path.c_str(), "wb");
        if (f) {
            std::fwrite(dep_out.data(), 2, dep_out.size(), f);
            std::fclose(f);
        }
        progress.finish_phase("write_depth", "Wrote Output.dep.", kWriteDepthUnits);
    }

    std::string cpt_path = output_prefix + ".cpt";
    progress.report("cpt_tml", "Exporting CPT (.tml pass)...", 0, cpt_units);
    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Exporting CPT (tml pass)...");
    export_terrain_cpt(
        output_prefix, "tml", cpt_path,
        tile_size, min_tile_size,
        project.terrain_name, project.creator,
        depth_format,
        [&](int current, int total, const std::string& message) {
            progress.report("cpt_tml", message, current, total);
        });
    progress.finish_phase("cpt_tml", "Exported CPT (.tml pass).", cpt_units);

    progress.report("tms_tiles", "Generating multi-resolution tiles...", 0, tms_units);
    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Generating multi-resolution tiles...");
    {
        int sz = tile_size;
        int depth = 0;
        int processed_tiles = 0;
        while (sz >= min_tile_size) {
            for (int y = 0; y < tile_size; y += sz) {
                for (int x = 0; x < tile_size; x += sz) {
                    char tml_name[512];
                    char tms_name[512];

                    std::snprintf(tml_name, sizeof(tml_name), "%sS%d.tml",
                                  output_prefix.c_str(), depth);
                    std::snprintf(tms_name, sizeof(tms_name), "%sS%d.tms",
                                  output_prefix.c_str(), depth);
                    bool is_single = true;

                    if (!fs::exists(tml_name)) {
                        is_single = false;
                        std::snprintf(tml_name, sizeof(tml_name), "%sS%d_%02x_%02x.tml",
                                      output_prefix.c_str(), depth, x >> 4, y >> 4);
                        std::snprintf(tms_name, sizeof(tms_name), "%sS%d_%02x_%02x.tms",
                                      output_prefix.c_str(), depth, x >> 4, y >> 4);
                    }

                    if (!fs::exists(tml_name)) {
                        ++processed_tiles;
                        char msg[256];
                        std::snprintf(msg, sizeof(msg), "Generating .tms tiles depth=%d %02x/%02x",
                                      depth, x >> 4, y >> 4);
                        progress.report("tms_tiles", msg, processed_tiles, tms_units);
                        continue;
                    }

                    try {
                        MeshData mesh;
                        if (depth != 0) {
                            mesh = MeshData::read(tml_name);
                            mesh.remap_vertex_ordering();
                        }
                        mesh.write(tms_name);
                    } catch (...) {
                        // Skip on error
                    }

                    ++processed_tiles;
                    {
                        char msg[256];
                        std::snprintf(msg, sizeof(msg), "Generating .tms tiles depth=%d %02x/%02x",
                                      depth, x >> 4, y >> 4);
                        progress.report("tms_tiles", msg, processed_tiles, tms_units);
                    }

                    if (is_single) {
                        goto next_tms_level;
                    }
                }
            }
next_tms_level:
            sz >>= 1;
            depth++;
        }
    }
    progress.finish_phase("tms_tiles", "Generated multi-resolution tiles.", tms_units);

    progress.report("cpt_tms", "Exporting CPT (.tms pass)...", 0, cpt_units);
    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Exporting CPT (tms pass)...");
    export_terrain_cpt(
        output_prefix, "tms", cpt_path,
        tile_size, min_tile_size,
        project.terrain_name, project.creator,
        depth_format,
        [&](int current, int total, const std::string& message) {
            progress.report("cpt_tms", message, current, total);
        });
    progress.finish_phase("cpt_tms", "Exported CPT (.tms pass).", cpt_units);

    opennova::io::logf(opennova::io::LogLevel::kInfo,
		"Build complete: %s", cpt_path.c_str());
    progress.complete("Build complete.");
}

} // namespace opennova

