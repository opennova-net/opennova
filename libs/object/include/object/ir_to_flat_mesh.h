#pragma once

#include "object/flat_mesh.h"
#include "threedi/threedi_3di3.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convert one render LOD of a Threedi3di3 model into a FlatMeshArray
// (one FlatMesh per render-object part).
//
// Args:
//   ir                  Source model; must be loaded and parsed.
//   lod_index           Which render LOD to flatten.
//   include_empty_parts If non-zero, parts with zero vertices still get an
//                       (empty) FlatMesh entry; useful for index stability.
//   track_bone_data     If non-zero AND the LOD is skinned, populates the
//                       vertex_influence_offsets + vertex_influences arrays
//                       inside each FlatMesh.
//   out                 Receives the array; caller frees with
//                       object_flat_mesh_array_free().
//
// Returns 0 on success, -1 on error.
//
// Mirrors pyopennova.mesh_build.flatten_lod() with preserve_source_indexing=1
// hardcoded (vertex sharing 1:1 from the IR; matches what nlascexp.dle ::
// ASE_GetExportTriObject @ 0x1000a6f0 expects from a Max scene).
//
// Equivalent to object_ir_to_flat_meshes_v2(..., preserve_source_indexing=0).
OBJECT_FLAT_MESH_EXPORT int object_ir_to_flat_meshes(const Threedi3di3* ir, int lod_index,
                                                     int include_empty_parts, int track_bone_data,
                                                     FlatMeshArray* out);

// V2: adds preserve_source_indexing flag.
// When preserve_source_indexing=1, dedup vertices by source IR index (first-seen wins),
// matching pyopennova.mesh_build.flatten_lod() with preserve_source_indexing=True.
// When preserve_source_indexing=0, uses the slice-based vertex range (v0 behavior).
OBJECT_FLAT_MESH_EXPORT int object_ir_to_flat_meshes_v2(const Threedi3di3* ir, int lod_index,
                                                        int include_empty_parts, int track_bone_data,
                                                        int preserve_source_indexing,
                                                        FlatMeshArray* out);

#ifdef __cplusplus
}
#endif
