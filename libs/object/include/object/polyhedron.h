#pragma once
// libs/object/include/object/polyhedron.h
//
// Port of pyopennova/polyhedron.py::compute_polyhedron_controlled.
// Halfspace-intersection convex hull builder for collision volumes.
// Host-agnostic: takes/returns plain float arrays.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compute a convex polyhedron from halfspace planes.
//
// Each halfspace is encoded as 4 floats: (nx, ny, nz, d) representing
// n . x + d <= 0.  Plane count is hs_count.
//
// Intersects every triple of planes, keeps vertices satisfying all
// halfspaces, then builds one wound polygon per plane.
//
// On success returns 1 and writes:
//   out_verts       — flat float triples, one per vertex
//   out_vert_count  — number of vertices written
//   out_faces       — face index lists packed as:  [count, i0, i1, ..., count, i0, ...]
//   out_face_ints   — total ints written to out_faces
//   out_face_count  — number of faces
//
// Caller must pre-allocate:
//   out_verts      at least hs_count^3 * 3 floats  (worst-case unique vertices)
//   out_faces      at least hs_count * (hs_count+1) ints (worst-case per-plane polygon)
//
// Returns 0 if fewer than 4 unique hull points found (caller falls back to
// axis-aligned box).
int polyhedron_compute(
    const float* halfspaces,    // hs_count * 4 floats
    int hs_count,
    float* out_verts,           // output vertex buffer (flat float triples)
    int* out_vert_count,
    int* out_faces,             // output face buffer (packed: count, idx0, idx1, ...)
    int* out_face_ints,         // total ints written
    int* out_face_count);

#ifdef __cplusplus
}
#endif
