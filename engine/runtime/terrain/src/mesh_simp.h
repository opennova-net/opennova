#pragma once

#include "terrain/types.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace opennova {

// Exact port of TrnGen.exe mesh simplification system.
// Uses GLOBAL state (matching the original's global arrays).
// MeshVertex = 0x34 (52) bytes, heap-allocated.
// MeshFace = 0x18 (24) bytes, heap-allocated.
// Both stored as pointer arrays in globals.

// MeshVertex — 52 bytes, heap-allocated via malloc(0x34)
// Ported from sub_4083E0 (MeshVertex_Init)
struct MeshVertex {
    float pos[3];               // +0x00: x, y, z
    int id;                     // +0x0C: sequential vertex ID
    MeshVertex** neighbors;     // +0x10: array of neighbor vertex pointers
    int neighbor_count;         // +0x14
    int neighbor_capacity;      // +0x18
    MeshVertex** faces_array;   // +0x1C: actually MeshFace** — faces referencing this vertex
    int face_count;             // +0x20
    int face_capacity;          // +0x24
    float collapse_cost;        // +0x28: minimum collapse cost
    float collapse_cost2;       // +0x2C: secondary cost (threshold comparison)
    MeshVertex* collapse_target; // +0x30: best neighbor to collapse to
};
static_assert(sizeof(MeshVertex) == 52 || sizeof(void*) == 8,
    "MeshVertex size check (exact only on 32-bit)");

// MeshFace — 24 bytes, heap-allocated via malloc(0x18)
// Ported from sub_4071B0 (MeshFace_Init)
struct MeshFace {
    MeshVertex* v[3];           // +0x00: 3 vertex pointers
    float normal[3];            // +0x0C: face normal (computed by sub_4075C0)
};

// Global mesh simplification state (matches TrnGen.exe globals)
struct MeshSimpGlobals {
    // Vertex pointer array — g_vertex_array (dword_42E818)
    MeshVertex** vertices;
    int vertex_count;           // dword_42E81C
    int vertex_capacity;        // dword_42E820

    // Face pointer array — g_face_array (dword_42E7E8)
    MeshFace** faces;
    int face_count;             // dword_42E7EC
    int face_capacity;          // dword_42E7F0

    // Temp face array for cost computation — dword_42E7F8
    MeshFace** temp_faces;
    int temp_face_count;        // dword_42E7FC
    int temp_face_capacity;     // dword_42E800

    // Mode flag — dword_42E824
    int simplify_mode;

    // Parameter — dword_42E7D8
    int param;

    // LOD level tracking — dword_42E7DC
    int current_lod_level;
};

// Global instance
extern MeshSimpGlobals g_simp;

// ===== Functions ported from IDA =====

// Initialize simplification from vertex/face data (replaces sub_409080).
// vertex_data: 12 bytes per vertex (3 floats: x,y,z)
// face_data: 12 bytes per face (3 int32: vertex indices)
void MeshSimp_Init(const uint8_t* vertex_data, int vertex_count,
                   const uint8_t* face_data, int face_count, int param);

// Rebuild global vertex array from 12-byte vertex records
void MeshSimp_RebuildVertices(const uint8_t* data, int count);

// Rebuild global face array from 12-byte face records (3 int32 indices)
void MeshSimp_RebuildFaces(const uint8_t* data, int count);

// sub_408C90: Compute collapse costs for all vertices
void MeshSimp_ComputeAllCosts();

// sub_408C10: Compute minimum collapse cost for one vertex
void MeshSimp_ComputeMinCost(MeshVertex* v);

// sub_409030: Find vertex with minimum collapse cost
MeshVertex* MeshSimp_FindMinCostVertex();

// sub_409180: Reduce vertex count to target (stops after 10 failures)
void MeshSimp_ReduceToTarget(int target);

// sub_409110: Reduce with threshold check
void MeshSimp_ReduceWithThreshold(int target, float threshold);

// sub_408D40: Collapse a vertex to its target
void MeshSimp_CollapseVertex(MeshVertex* v, MeshVertex* target);

// sub_4083E0: Initialize a MeshVertex and add to global array
MeshVertex* MeshVertex_Init(MeshVertex* v, int x, int y, int z, int id);

// sub_4071B0: Initialize a MeshFace and add to global array
MeshFace* MeshFace_Init(MeshFace* f, MeshVertex* v0, MeshVertex* v1, MeshVertex* v2);

// sub_4075C0: Compute face normal
void MeshFace_ComputeNormal(MeshFace* f);

// sub_407480: Remove a face from the mesh
void MeshFace_Remove(MeshFace* f);

// sub_4084E0: Destroy a vertex
void MeshVertex_Destroy(MeshVertex* v);

// sub_408160: Collapse edge in a face (replace v_old with v_new)
void MeshEdge_Collapse(MeshFace* f, MeshVertex* v_old, MeshVertex* v_new);

// sub_408800: Compute full edge collapse cost
// Returns double — matches x87 st(0) return at PC=2 precision
double MeshEdge_ComputeCostFull(MeshVertex* v, MeshVertex* target, float* out_cost2);

// sub_407710: Compute edge normal cost
// sub_407710, sub_4078D0, sub_407A90: Cost helper functions
// Defined as static inline in mesh_simp.cpp for x87 FPU precision matching

// sub_408720: Check if vertex is boundary
bool MeshVertex_IsBoundary(MeshVertex* v);

// sub_408690: Remove vertex from neighbor lists in faces
void MeshVertex_RemoveFromFaceNeighbors(MeshVertex* v_to_remove, MeshVertex* in_vertex);

} // namespace opennova

