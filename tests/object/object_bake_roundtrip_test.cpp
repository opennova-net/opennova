#include "object/bake.h"
#include "object/nlascexp_options.h"
#include "object/three_di_policy.h"
#include "object/write_ase_files.h"
#include "tdp/tdp.h"
#include "threedi/threedi_3di3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "common/test_paths.h"

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char *, 6> kStockFixtures = {
    "wtrfall.3di",
    "Wcrate5.3di",
    "Lstrng1.3di",
    "Cbunker1.3di",
    "Armry01.3di",
    "DT801.3di",
};

void copy_fixed(char *dst, size_t dst_size, const std::string &src) {
    if (!dst || dst_size == 0) {
        return;
    }
    const size_t count = std::min(dst_size - 1, src.size());
    std::memcpy(dst, src.data(), count);
    dst[count] = '\0';
}

int lod_triangle_count(const ThreediLod &lod) {
    int total = 0;
    for (size_t i = 0; i < lod.strip_count; ++i) {
        total += static_cast<int>(lod.strips[i].num_triangles);
    }
    return total;
}

int derive_poly_collision_lod(const Threedi3di3 &model) {
    if (!model.collision || model.lod_count == 0) {
        return -1;
    }
    const ThreediCollisionModel &collision = *model.collision;
    const int target_faces = static_cast<int>(collision.face_count);
    const int target_vertices = static_cast<int>(collision.vertex_count);
    if (target_faces <= 0 || target_vertices <= 0) {
        return -1;
    }
    if (model.lod_count == 1) {
        return 0;
    }

    int best_face = -1;
    int best_vertex = -1;
    for (size_t i = 0; i < model.lod_count; ++i) {
        const ThreediLod &lod = model.lods[i];
        const int lod_vertices = static_cast<int>(lod.vertices.count);
        if (lod_triangle_count(lod) == target_faces) {
            if (lod_vertices == target_vertices) {
                best_face = static_cast<int>(i);
            } else if (best_face < 0) {
                best_face = static_cast<int>(i);
            }
        }
        if (lod_vertices == target_vertices) {
            best_vertex = static_cast<int>(i);
        }
    }
    return best_face >= 0 ? best_face : best_vertex;
}

std::string read_text(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool write_source_project(const Threedi3di3 &model,
                          const fs::path &tdp_path,
                          const fs::path &primary_ase_path) {
    TdpProject project;
    if (tdp_from_3di3(&model, &project) != 0) {
        std::fprintf(stderr, "tdp_from_3di3 failed\n");
        return false;
    }

    bool ok = true;
    for (size_t lod_index = 0;
         lod_index < model.lod_count && lod_index < TDP_MAX_LODS;
         ++lod_index) {
        const std::string scene_file = lod_index == 0
            ? primary_ase_path.filename().string()
            : (primary_ase_path.stem().string() + "_lod" +
               std::to_string(lod_index) + primary_ase_path.extension().string());
        copy_fixed(project.lods[lod_index].scene_file,
                   sizeof(project.lods[lod_index].scene_file),
                   scene_file);
    }

    const int poly_collision_lod = derive_poly_collision_lod(model);
    if (poly_collision_lod >= 0) {
        project.poly_collision_lod = poly_collision_lod;
    }

    const fs::path tda_path = tdp_path.parent_path() /
        (tdp_path.stem().string() + ".3da");
    if (tdp_write(tdp_path.string().c_str(), &project) != 0) {
        std::fprintf(stderr, "tdp_write failed for %s\n",
                     tdp_path.string().c_str());
        ok = false;
    }
    if (ok && tdp_write_3da(tda_path.string().c_str(), &project) != 0) {
        std::fprintf(stderr, "tdp_write_3da failed for %s\n",
                     tda_path.string().c_str());
        ok = false;
    }
    tdp_free(&project);
    return ok;
}

bool write_source_ase_files(const Threedi3di3 &model,
                            const fs::path &primary_ase_path) {
    NlascexpOptions options;
    object_nlascexp_options_init_defaults(&options);
    options.include_collisions = 1;
    options.include_occlusion = 1;
    options.include_lights = 1;

    constexpr int kCapacity = 16;
    std::array<std::array<char, 260>, kCapacity> buffers{};
    std::array<char *, kCapacity> paths{};
    for (int i = 0; i < kCapacity; ++i) {
        paths[i] = buffers[i].data();
    }

    int count = 0;
    const int rc = object_write_ase_files_from_3di3(
        &model,
        primary_ase_path.string().c_str(),
        &options,
        paths.data(),
        kCapacity,
        &count);
    if (rc != 0 || count <= 0) {
        std::fprintf(stderr,
                     "object_write_ase_files_from_3di3 failed for %s (rc=%d, count=%d)\n",
                     primary_ase_path.string().c_str(),
                     rc,
                     count);
        return false;
    }
    return true;
}

bool compare_baked_output(const fs::path &source_3di,
                          const fs::path &baked_3di,
                          const fs::path &work_dir) {
    const fs::path compare_report = work_dir / "compare.json";
    const Object3diPolicyStatus compare_status = object_3di_compare_files(
        source_3di.string().c_str(),
        baked_3di.string().c_str(),
        OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        compare_report.string().c_str());
    if (compare_status != OBJECT_3DI_POLICY_OK) {
        std::fprintf(stderr,
                     "object_3di_compare_files failed for %s -> %s (status=%d)\n%s\n",
                     source_3di.string().c_str(),
                     baked_3di.string().c_str(),
                     static_cast<int>(compare_status),
                     read_text(compare_report).c_str());
        return false;
    }

    const fs::path geometry_report = work_dir / "geometry.json";
    const Object3diPolicyStatus geometry_status =
        object_3di_validate_geometry_chunks(
            baked_3di.string().c_str(),
            0,
            geometry_report.string().c_str());
    if (geometry_status != OBJECT_3DI_POLICY_OK) {
        std::fprintf(stderr,
                     "object_3di_validate_geometry_chunks failed for %s (status=%d)\n%s\n",
                     baked_3di.string().c_str(),
                     static_cast<int>(geometry_status),
                     read_text(geometry_report).c_str());
        return false;
    }
    return true;
}

bool roundtrip_fixture(const fs::path &source_3di, const fs::path &root) {
    const std::string stem = source_3di.stem().string();
    const fs::path work_dir = root / stem;
    fs::create_directories(work_dir);

    Threedi3di3 model{};
    if (threedi_read_model_auto(source_3di.string().c_str(), &model) != 0) {
        std::fprintf(stderr, "threedi_read_model_auto failed for %s\n",
                     source_3di.string().c_str());
        return false;
    }

    const fs::path ase_path = work_dir / (stem + ".ase");
    const fs::path tdp_path = work_dir / (stem + ".3dp");
    const fs::path baked_path = work_dir / (stem + ".baked.3di");

    bool ok = write_source_ase_files(model, ase_path) &&
              write_source_project(model, tdp_path, ase_path);
    threedi_3di3_free(&model);

    if (!ok) {
        return false;
    }

    const BakeStatus bake_status = bake_project_export(
        tdp_path.string().c_str(),
        baked_path.string().c_str(),
        stem.c_str(),
        BAKE_UPDATE_ALL);
    if (bake_status != BAKE_STATUS_OK) {
        std::fprintf(stderr, "bake_project_export failed for %s (status=%d)\n",
                     tdp_path.string().c_str(),
                     static_cast<int>(bake_status));
        return false;
    }
    if (!fs::is_regular_file(baked_path) || fs::file_size(baked_path) == 0) {
        std::fprintf(stderr, "native bake did not produce %s\n",
                     baked_path.string().c_str());
        return false;
    }

    return compare_baked_output(source_3di, baked_path, work_dir);
}

}  // namespace

int main() {
    const fs::path repo_root = test_paths_repo_root(__FILE__);
    const fs::path fixture_root = repo_root / "fixtures" / "threedi" / "stock_jo";
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const fs::path work_root = fs::temp_directory_path() /
        ("opennova_object_bake_roundtrip_" + std::to_string(stamp));

    fs::remove_all(work_root);
    fs::create_directories(work_root);

    for (const char *fixture : kStockFixtures) {
        const fs::path source_3di = fixture_root / fixture;
        if (!fs::is_regular_file(source_3di)) {
            std::fprintf(stderr, "missing fixture %s\n",
                         source_3di.string().c_str());
            return EXIT_FAILURE;
        }
        std::printf("object bake roundtrip: %s\n", fixture);
        std::fflush(stdout);
        if (!roundtrip_fixture(source_3di, work_root)) {
            std::fprintf(stderr, "kept failure artifacts in %s\n",
                         work_root.string().c_str());
            return EXIT_FAILURE;
        }
    }

    fs::remove_all(work_root);
    return EXIT_SUCCESS;
}
