// libs/object/include/object/flat_mesh_to_ase.h
#pragma once

#include "object/flat_mesh.h"
#include "object/nlascexp_options.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare the IR + AseDocument types so callers don't need both headers.
struct Threedi3di3;
struct ase_Document;

// Build per-FlatMesh ase_Object array AND populate the AseDocument's main mesh
// objects. Caller provides:
//   - meshes: from object_ir_to_flat_meshes_v2 (Phase C prep)
//   - ir + lod_index: for part_abs computation, skinned check, material refs
//   - opts: NlascexpOptions (used for include_collisions/include_occlusion gates)
//   - global_to_local: optional material index remap (used by per-LOD slim variant);
//     pass nullptr for identity (no remapping)
//   - global_to_local_count: length of global_to_local array
//   - out_objects: caller-allocated ase_Object array (size >= meshes->count)
//   - out_count: total ase_Object slots filled
//
// On success returns 0. Per-object alloc is done internally via ase_alloc_object.
int object_emit_main_mesh_objects(
    const struct FlatMeshArray* meshes,
    const struct Threedi3di3* ir,
    int lod_index,
    const NlascexpOptions* opts,
    const int* global_to_local,
    int global_to_local_count,
    struct ase_Object* out_objects,
    int* out_count);

#ifdef __cplusplus
}
#endif
