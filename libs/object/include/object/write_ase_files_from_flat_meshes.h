// libs/object/include/object/write_ase_files_from_flat_meshes.h
//
// Public entry point for writing .ase files from caller-edited FlatMeshes
// plus IR side-data (collision, occlusion, lights, markers, bones, materials).
//
// Used by the Blender DCC bridge (Phase E): Blender hands back edited FlatMeshes
// after artist edits; this function combines them with the original IR's
// non-mesh side-data and writes the full .ase document.
//
// The Threedi3di3 IR-driven entry point (object_write_ase_files_from_3di3) is
// now a thin wrapper that flattens IR -> FlatMeshes and then calls into this.

#pragma once

#include "object/flat_mesh.h"
#include "object/nlascexp_options.h"

struct Threedi3di3;
struct BadFile;

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define WRITE_ASE_FROM_FLAT_MESHES_EXPORT __declspec(dllexport)
#  else
#    define WRITE_ASE_FROM_FLAT_MESHES_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define WRITE_ASE_FROM_FLAT_MESHES_EXPORT __attribute__((visibility("default")))
#  else
#    define WRITE_ASE_FROM_FLAT_MESHES_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Write .ase files for all LODs of the given IR model, sourcing render-mesh
// geometry from caller-provided FlatMeshArrays.
//
//   - meshes_per_lod: array of FlatMeshArray, length lod_count. Each entry
//     holds the edited (or unedited) FlatMeshes for that LOD. The caller owns
//     these FlatMeshArrays; this function does not free them.
//   - lod_count: must equal ir->lod_count.
//   - primary_path: path for LOD 0 (e.g., "out/akcrate.ase"); per-LOD files
//     are named "<stem>_lod<N>.ase".
//   - opts: nullable; if null, defaults are used.
//   - out_paths_array: caller-owned array of char* buffers (each >= 260 chars).
//   - out_paths_capacity: number of entries in out_paths_array.
//   - out_paths_count: written by us — number of files actually emitted.
//
// Returns 0 on success, nonzero on error.
//
// Per-LOD emission policy:
//   - Primary LOD (0): bones (if skinned) + main mesh + centers + attaches
//     + userpoints + collision helpers (if opts.include_collisions)
//     + occlusion helpers (if opts.include_occlusion) + lights.
//   - Secondary LOD (1+): main mesh + centers + attaches only.
//   - LODs with render_object_count == 0 are skipped entirely.
//
// Side-data (collision, occlusion, markers, lights, materials, bones) comes
// from the IR. Only the per-LOD render mesh geometry comes from meshes_per_lod.
WRITE_ASE_FROM_FLAT_MESHES_EXPORT int object_write_ase_files_from_flat_meshes(
    const struct FlatMeshArray* meshes_per_lod,
    int lod_count,
    const struct Threedi3di3* ir,
    const char* primary_path,
    const NlascexpOptions* opts,
    char** out_paths_array,
    int out_paths_capacity,
    int* out_paths_count);

// Variant of object_write_ase_files_from_flat_meshes that accepts a BadFile
// for bone emission. When bad_file == NULL, behavior is identical to
// object_write_ase_files_from_flat_meshes. When bad_file != NULL &&
// bad_file->num_bones > 0, the primary LOD's bone emission uses BAD bones
// + a root_motion bone instead of lod0.render_objects. See
// object_emit_bone_objects_with_bad for semantics.
WRITE_ASE_FROM_FLAT_MESHES_EXPORT int object_write_ase_files_from_flat_meshes_with_bad(
    const struct FlatMeshArray* meshes_per_lod,
    int lod_count,
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
