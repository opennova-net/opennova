// libs/object/include/object/write_ase_files.h
//
// Top-level orchestrator: write primary + per-LOD .ase files from a Threedi3di3 IR.
// Port of pyopennova/ase_from_3di3.py::write_ase_from_3di3 (line 132) and
// ThreediAseWriter (lines 152-248).
#pragma once

#include "object/nlascexp_options.h"

struct Threedi3di3;

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define WRITE_ASE_FILES_EXPORT __declspec(dllexport)
#  else
#    define WRITE_ASE_FILES_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define WRITE_ASE_FILES_EXPORT __attribute__((visibility("default")))
#  else
#    define WRITE_ASE_FILES_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Write .ase files for all LODs of the given IR model.
//   - primary_path: path for LOD 0 (e.g., "out/akcrate.ase").
//     Per-LOD files use "<stem>_lod<N>.ase" naming.
//   - out_paths_array: caller-owned array of char* buffers (each must hold >= 260 chars).
//     We write the actual output paths here.
//   - out_paths_capacity: number of entries in out_paths_array.
//   - out_paths_count: written by us — number of files actually emitted.
// Returns 0 on success, nonzero on error.
WRITE_ASE_FILES_EXPORT int object_write_ase_files_from_3di3(
    const struct Threedi3di3* ir,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count);

struct BadFile;

// Variant that accepts a BadFile for bone emission. See
// object_write_ase_files_from_flat_meshes_with_bad for semantics.
// When bad_file == NULL, behavior is identical to object_write_ase_files_from_3di3.
WRITE_ASE_FILES_EXPORT int object_write_ase_files_from_3di3_with_bad(
    const struct Threedi3di3* ir,
    const struct BadFile* bad_file,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count);

#ifdef __cplusplus
}
#endif
