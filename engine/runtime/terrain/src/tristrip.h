#pragma once

#include <cstdint>
#include <vector>

namespace opennova {

// Ported from triangle strip optimization functions at 0x405A20-0x406E00.
// Converts triangle face lists into optimized triangle strips with
// degenerate triangle connectors between strips.

struct TriStripResult {
    std::vector<uint16_t> indices;  // triangle strip index buffer (with degenerate connectors)
    int total_indices = 0;
    bool is_strip = true;           // true = triangle strip, false = triangle list
};

// Build optimized triangle strips from face data.
// Ported from sub_4068E0. Tries two strategies (with/without look-ahead),
// picks the one producing fewer total indices, then concatenates strips
// with cache-aware ordering (sub_406140/sub_405EF0).
//
// face_indices: array of vertex index triples (3 uint16 per face)
// face_count: number of faces
// vertex_count: total vertices (unused, kept for interface compatibility)
TriStripResult build_triangle_strips(
    const uint16_t* face_indices, int face_count, int vertex_count);

} // namespace opennova

