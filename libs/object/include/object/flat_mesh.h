#pragma once

#include <cstdint>

// Phase B FlatMesh — host-agnostic per-part mesh data.
// See notes/3di-pipeline/specs/2026-05-15-dcc-roundtrip-design.md § 3.
// Field-by-field mirror of pyopennova.mesh_build.FlatMesh.

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_FLAT_MESH_EXPORT __declspec(dllexport)
#  else
#    define OBJECT_FLAT_MESH_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_FLAT_MESH_EXPORT __attribute__((visibility("default")))
#  else
#    define OBJECT_FLAT_MESH_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlatMeshVertex {
    float x, y, z;
} FlatMeshVertex;

typedef struct FlatMeshFace {
    int32_t v[3];                   // vertex indices into FlatMesh.vertices
    int32_t material_id;            // global TDP material index
    uint32_t smoothing_group_mask;  // 32-bit bitmask; 0 if none derived
    // Note: edge_vis_mask carried per-face once Phase C needs it; deferred for now.
} FlatMeshFace;

typedef struct FlatMeshFaceCornerData {
    // Per-face-corner; 3 entries per face, in face order.
    float u0, v0;                   // uv0
    float u1, v1;                   // uv1
    float nx, ny, nz;               // per-corner normal
} FlatMeshFaceCornerData;

typedef struct FlatMeshBoneInfluence {
    int32_t bone_index;
    float weight;
} FlatMeshBoneInfluence;

typedef struct FlatMesh {
    char name[64];
    int32_t part_index;
    float origin[3];                // part_abs_position in render space

    int32_t vertex_count;
    FlatMeshVertex* vertices;

    int32_t face_count;
    FlatMeshFace* faces;

    FlatMeshFaceCornerData* corners;  // sized face_count * 3

    int32_t material_id_set_count;
    int32_t* material_id_set;

    // Skinning (CSR-style); NULL if track_bone_data was 0 at convert time.
    int32_t* vertex_influence_offsets;  // sized vertex_count + 1
    FlatMeshBoneInfluence* vertex_influences;
} FlatMesh;

typedef struct FlatMeshArray {
    int32_t count;
    FlatMesh* meshes;
} FlatMeshArray;

// Lifecycle. All return 0 on success, non-zero on error.

// Allocate an array of `count` zero-initialized FlatMesh entries.
OBJECT_FLAT_MESH_EXPORT int object_flat_mesh_array_alloc(int count, FlatMeshArray* out);

// Free all per-mesh arrays AND the FlatMeshArray.meshes pointer.
// Safe to call on an already-freed or zero-initialized array.
OBJECT_FLAT_MESH_EXPORT void object_flat_mesh_array_free(FlatMeshArray* arr);

// Allocate vertex/face/corner/material_id_set arrays inside a single FlatMesh.
// If `total_influences` > 0, also allocates vertex_influence_offsets (size
// vertex_count+1) and vertex_influences (size total_influences).
// The mesh's `name`, `part_index`, `origin` are left untouched.
OBJECT_FLAT_MESH_EXPORT int object_flat_mesh_resize(FlatMesh* fm, int vertex_count, int face_count,
                                                    int material_id_set_count, int total_influences);

// Free all per-mesh arrays; leaves the FlatMesh struct zero-init except
// for name/part_index/origin (preserved). Safe to call on a freed mesh.
OBJECT_FLAT_MESH_EXPORT void object_flat_mesh_free(FlatMesh* fm);

#ifdef __cplusplus
}
#endif
