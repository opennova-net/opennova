#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Compute per-face smoothing-group bitmask. faces: flat array of 3*face_count
// vertex indices. face_corner_normals: flat array of 3*face_count Vec3 normals
// (one per face corner). out: face_count uint32 bitmasks; bit k set means
// the face belongs to group k+1.
//
// Two faces sharing a vertex with the same (within tolerance) corner normal
// belong to the same group. Mirror of pyopennova/mesh_utils.py::compute_smoothing_groups.
void object_compute_smoothing_groups(int face_count,
                                     const int* faces,
                                     const float* face_corner_normals,
                                     uint32_t* out);

#ifdef __cplusplus
}
#endif
