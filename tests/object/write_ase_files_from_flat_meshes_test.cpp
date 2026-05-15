// tests/object/write_ase_files_from_flat_meshes_test.cpp
//
// Phase E Task 2: LOD-0 byte-match between the IR-driven wrapper
// (object_write_ase_files_from_3di3) and the direct FlatMesh entry
// (object_write_ase_files_from_flat_meshes).
//
// Higher LODs may legitimately differ when collision-LOD substitution
// applies (the wrapper does substitution; the direct entry doesn't),
// so only LOD 0 is byte-compared.

#include "object/write_ase_files.h"
#include "object/write_ase_files_from_flat_meshes.h"
#include "object/flat_mesh.h"
#include "object/ir_to_flat_mesh.h"
#include "object/nlascexp_options.h"
#include "threedi/threedi_3di3.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<unsigned char> read_file_bytes(const char* path) {
    std::vector<unsigned char> bytes;
    FILE* f = std::fopen(path, "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        bytes.resize((size_t)sz);
        std::fread(bytes.data(), 1, (size_t)sz, f);
    }
    std::fclose(f);
    return bytes;
}

static int write_via_wrapper(const Threedi3di3* ir, const char* primary_path,
                              std::vector<std::string>& out_paths)
{
    NlascexpOptions opts;
    object_nlascexp_options_init_defaults(&opts);

    const int CAP = 16;
    std::vector<std::vector<char>> bufs(CAP, std::vector<char>(260, 0));
    std::vector<char*> ptrs(CAP);
    for (int i = 0; i < CAP; ++i) ptrs[i] = bufs[i].data();
    int count = 0;
    int rc = object_write_ase_files_from_3di3(ir, primary_path, &opts,
                                                ptrs.data(), CAP, &count);
    if (rc != 0) return rc;
    out_paths.clear();
    for (int i = 0; i < count; ++i) out_paths.emplace_back(ptrs[i]);
    return 0;
}

static int write_via_flat_meshes(const Threedi3di3* ir, const char* primary_path,
                                   std::vector<std::string>& out_paths)
{
    NlascexpOptions opts;
    object_nlascexp_options_init_defaults(&opts);

    int lod_count = (int)ir->lod_count;
    bool skinned = ((int)ir->header.mesh_type == (int)THREEDI_MESH_SKINNED);

    // Build per-LOD FlatMeshArrays via the same flattening the wrapper would
    // use for non-collision-LOD cases. For LOD 0 this matches the wrapper's
    // behavior (akcrate's collision LOD is NOT 0).
    std::vector<FlatMeshArray> per_lod(lod_count);
    for (int li = 0; li < lod_count; ++li) {
        std::memset(&per_lod[li], 0, sizeof(FlatMeshArray));
        int rc = object_ir_to_flat_meshes_v2(ir, li,
            /*include_empty_parts=*/1,
            /*track_bone_data=*/skinned ? 1 : 0,
            /*preserve_source_indexing=*/1,
            &per_lod[li]);
        if (rc != 0) {
            for (int j = 0; j < li; ++j) object_flat_mesh_array_free(&per_lod[j]);
            return rc;
        }
    }

    const int CAP = 16;
    std::vector<std::vector<char>> bufs(CAP, std::vector<char>(260, 0));
    std::vector<char*> ptrs(CAP);
    for (int i = 0; i < CAP; ++i) ptrs[i] = bufs[i].data();
    int count = 0;
    int rc = object_write_ase_files_from_flat_meshes(
        per_lod.data(), lod_count, ir, primary_path, &opts,
        ptrs.data(), CAP, &count);

    for (auto& arr : per_lod) object_flat_mesh_array_free(&arr);

    if (rc != 0) return rc;
    out_paths.clear();
    for (int i = 0; i < count; ++i) out_paths.emplace_back(ptrs[i]);
    return 0;
}

int main() {
    const char* repo_root = test_paths_repo_root(__FILE__);

    char fixture_path[4096];
    std::snprintf(fixture_path, sizeof(fixture_path),
                  "%s/fixtures/stock_3di/akcrate/akcrate.3di", repo_root);

    Threedi3di3 ir;
    std::memset(&ir, 0, sizeof(ir));
    TEST_EXPECT(threedi_3di3_read(fixture_path, &ir) == 0);
    TEST_EXPECT(ir.lod_count > 0);

    // Use the system temp dir to write outputs.
    const char* tmp_root = test_paths_temp_dir();

    char wrapper_primary[4096];
    char direct_primary[4096];
    std::snprintf(wrapper_primary, sizeof(wrapper_primary),
                  "%s%cphase_e_task2_wrapper_akcrate.ase",
                  tmp_root, TEST_PATHS_SEP);
    std::snprintf(direct_primary, sizeof(direct_primary),
                  "%s%cphase_e_task2_direct_akcrate.ase",
                  tmp_root, TEST_PATHS_SEP);

    std::vector<std::string> wrapper_files;
    TEST_EXPECT(write_via_wrapper(&ir, wrapper_primary, wrapper_files) == 0);
    TEST_EXPECT(!wrapper_files.empty());

    std::vector<std::string> direct_files;
    TEST_EXPECT(write_via_flat_meshes(&ir, direct_primary, direct_files) == 0);
    TEST_EXPECT(!direct_files.empty());

    TEST_EXPECT(wrapper_files.size() == direct_files.size());

    // LOD 0 byte-match.
    auto wrapper_bytes = read_file_bytes(wrapper_files[0].c_str());
    auto direct_bytes = read_file_bytes(direct_files[0].c_str());

    TEST_EXPECT(!wrapper_bytes.empty());
    TEST_EXPECT(!direct_bytes.empty());

    if (wrapper_bytes.size() != direct_bytes.size()) {
        std::fprintf(stderr,
                     "FAIL LOD 0 size: wrapper=%zu direct=%zu\n",
                     wrapper_bytes.size(), direct_bytes.size());
        return 1;
    }

    if (std::memcmp(wrapper_bytes.data(), direct_bytes.data(), wrapper_bytes.size()) != 0) {
        // Find first divergence for diagnostic.
        size_t i = 0;
        for (; i < wrapper_bytes.size(); ++i) {
            if (wrapper_bytes[i] != direct_bytes[i]) break;
        }
        std::fprintf(stderr,
                     "FAIL LOD 0 bytes: divergence at offset %zu (wrapper=0x%02x direct=0x%02x)\n",
                     i, wrapper_bytes[i], direct_bytes[i]);
        return 1;
    }

    threedi_3di3_free(&ir);

    std::fprintf(stdout, "PASS: LOD 0 .ase byte-match (wrapper == direct entry) for akcrate\n");
    return 0;
}
