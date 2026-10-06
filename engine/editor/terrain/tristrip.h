#pragma once

#include <cstdint>
#include <vector>

#include "mesh_data.h"

namespace opennova::editor::trngen {

// Ported from triangle strip optimization functions at 0x405A20-0x406E00.
// Converts triangle face lists into optimized triangle strips with
// degenerate triangle connectors between strips.

struct TriStripResult {
    std::vector<uint16_t> indices;  // triangle strip index buffer (with degenerate connectors)
    int total_indices = 0;
    bool is_strip = true;           // true = triangle strip, false = triangle list
};

// Build optimized triangle strips from face data.
// [orig: TrnGen.exe sub_4068E0 @ 0x4068E0] Tries two strategies (with/without look-ahead),
// picks the one producing fewer total indices, then concatenates strips
// with cache-aware ordering (sub_406140/sub_405EF0).
//
// face_indices: array of vertex index triples (3 uint16 per face)
// face_count: number of faces
// vertex_count: total vertices — unused by the strip walk; retained as a
// structural mirror of sub_4068E0's parameter list
TriStripResult build_triangle_strips(
    const uint16_t* face_indices, int face_count, int vertex_count);

// The bake pass over a parsed TPM1 tile mesh: converts face lists to triangle
// strips and reorders vertices for cache coherency. Ported from sub_404480
// (via thunk sub_404610); called by the builder before writing .tms files.
// Lives with the tristripper (not the tpm format lib) — bake machinery over
// the parsed model, ADR 0030 decision 1.
void remap_vertex_ordering(MeshData &mesh);

} // namespace opennova::editor::trngen

