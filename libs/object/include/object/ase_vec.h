#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Build an axis-aligned cube mesh centered at origin with half-extent `half`.
// 8 vertices, 12 triangles. Mirrors pyopennova/mesh_primitives.py::cube_mesh
// where Python's `size` = 2*half (cube_mesh(2*half) gives identical geometry).
// Caller-owned: out_verts[24] (8 verts x 3 floats), out_faces[36] (12 tris x 3 ints).
// Faces are fan-triangulated from Python's 6 quads: (q0,q1,q2) + (q0,q2,q3).
void object_cube_mesh(float half, float* out_verts, int* out_faces);

// Build a 4x3 TM (3-row basis + translation row) with identity rotation.
// out_tm[0..2][0..2] = identity, out_tm[3][0..2] = translation.
// Mirrors pyopennova/ase_from_3di3.py::_tm(translation) with no rot arg.
void object_tm_identity(const float translation[3], float out_tm[4][3]);

// Build a 4x3 TM with provided 3x3 rotation rows + translation.
// Mirror of pyopennova/ase_from_3di3.py::_tm(translation, rot=rot) @ line 851.
// Python transposes the rot rows when rot is provided:
//   rows[0] = (rot[0][0], rot[1][0], rot[2][0])
//   rows[1] = (rot[0][1], rot[1][1], rot[2][1])
//   rows[2] = (rot[0][2], rot[1][2], rot[2][2])
// out_tm receives the transposed basis + translation row.
void object_tm_from_rot(const float rot[3][3], const float translation[3], float out_tm[4][3]);

#ifdef __cplusplus
}
#endif
