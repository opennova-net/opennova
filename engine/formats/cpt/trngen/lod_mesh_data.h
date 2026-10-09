#pragma once

#include "mesh_data.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace opennova::trngen {

// Exact port of TrnGen.exe LODMeshData struct (0x1EC = 492 bytes).
// Field offsets verified from IDA decompilation of:
//   sub_4091D0 (init), sub_4092A0 (resize), sub_409310 (set vertex),
//   sub_409380 (set face), sub_4091F0 (free), sub_409400 (generate LOD).

static constexpr int NUM_LOD_LEVELS = 16;

// NOTE: The original is a 32-bit PE binary. We use native pointers and don't
// try to match the original struct layout in memory — we just need the same
// logical field organization. The static_asserts from the 32-bit layout are
// for documentation only.

struct LODLevel {
    uint16_t* face_index_data = nullptr;  // 3 shorts per face
    int   face_count = 0;
    int   _field_8 = 0;
    int   _field_12 = 0;
    int   vertex_count = 0;
    void* _data2 = nullptr;
    int   _field_24 = 0;
};

struct LODMeshData {
    int   vertex_count = 0;       // current vertex count (high water mark)
    int   face_count = 0;         // current face count (high water mark)
    int   vertex_capacity = 0;    // allocated vertex slots
    int   face_capacity = 0;      // allocated face slots
    LODMeshData* next = nullptr;  // linked list next pointer
    int   _field_20 = 0;
    int   _field_24 = 0;
    int   _field_28 = 0;
    uint8_t* vertex_data = nullptr;  // 40 bytes per vertex (10 floats)
    uint8_t* face_data = nullptr;    // 6 bytes per face (3 × uint16)
    void* extra_data = nullptr;      // auxiliary data pointer
    LODLevel levels[NUM_LOD_LEVELS] = {}; // 16 LOD levels
    // Port-only (TrnGen reallocates whatever it is asked): the first buffer size the port refused, in
    // words, a count so large only corrupt data asks for it; empty while the mesh is sound. A refused
    // Resize/SetVertex/SetFace returns -1 having changed nothing, and the bake stops on it
    // (make_node_mesh, generate_base_terrain_meshes).
    std::string failure;
};

// [orig: TrnGen.exe sub_4091D0 @ 0x4091D0]: zero-initialize and link into global list
void LODMeshData_Init(LODMeshData* mesh);

// [orig: TrnGen.exe sub_4092A0 @ 0x4092A0]: resize vertex and face buffers
int LODMeshData_Resize(LODMeshData* mesh, int vertex_count, int face_count);

// [orig: TrnGen.exe sub_409310 @ 0x409310]: set 40-byte vertex at index
int LODMeshData_SetVertex(LODMeshData* mesh, int index, const void* data);

// [orig: TrnGen.exe sub_409380 @ 0x409380]: set face (3 uint16 vertex indices) at index
int LODMeshData_SetFace(LODMeshData* mesh, int index, uint16_t v0, uint16_t v1, uint16_t v2);

// [orig: TrnGen.exe sub_4091F0 @ 0x4091F0]: free all internal buffers
void LODMeshData_Free(LODMeshData* mesh);

// [orig: TrnGen.exe sub_409400 @ 0x409400]: generate 16 LOD levels
void LODMeshData_GenerateLOD(LODMeshData* mesh, int max_vertices, int min_vertices);

// [orig: TrnGen.exe sub_409950 @ 0x409950]: generate LOD with configurable params
void LODMeshData_GenerateLODParams(LODMeshData* mesh, int max_vertices, int min_vertices,
                                    float param_a, float param_b);

// [orig: TrnGen.exe sub_403AD0 @ 0x403AD0]: create compact MeshData from LOD data
MeshData LODMeshData_CreateCompact(const LODMeshData* mesh);

// [orig: TrnGen.exe sub_409ED0 @ 0x409ED0]: load mesh section from parsed data
void LODMeshData_LoadSection(LODMeshData* dst, const LODMeshData* src, int section_index);

// [orig: TrnGen.exe sub_409FA0 @ 0x409FA0]: load mesh section with vertex dedup
void LODMeshData_LoadSectionDedup(LODMeshData* dst, const LODMeshData* src, int section_index);

} // namespace opennova::trngen

