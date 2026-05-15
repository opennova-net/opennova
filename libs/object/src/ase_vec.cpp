#include "object/ase_vec.h"

// object_cube_mesh
// ----------------
// Mirrors pyopennova/mesh_primitives.py::cube_mesh(size) where size = 2*half.
// Python: h = size * 0.5, then 8 verts and 6 quad faces.
// Faces fan-triangulated (q0,q1,q2) + (q0,q2,q3) to yield 12 triangles.
//
// Vertex layout (Python order):
//   0: (-h, -h, -h)
//   1: ( h, -h, -h)
//   2: ( h,  h, -h)
//   3: (-h,  h, -h)
//   4: (-h, -h,  h)
//   5: ( h, -h,  h)
//   6: ( h,  h,  h)
//   7: (-h,  h,  h)
//
// Quad -> tri (fan) mapping:
//   -Z: (0,1,2,3)  -> (0,1,2), (0,2,3)
//   +Z: (4,7,6,5)  -> (4,7,6), (4,6,5)
//   -Y: (0,4,5,1)  -> (0,4,5), (0,5,1)
//   +Y: (2,6,7,3)  -> (2,6,7), (2,7,3)
//   +X: (1,5,6,2)  -> (1,5,6), (1,6,2)
//   -X: (0,3,7,4)  -> (0,3,7), (0,7,4)
void object_cube_mesh(float half, float* out_verts, int* out_faces)
{
    float h = half;
    // 8 vertices x 3 floats = 24 floats
    float verts[24] = {
        -h, -h, -h,  // 0
         h, -h, -h,  // 1
         h,  h, -h,  // 2
        -h,  h, -h,  // 3
        -h, -h,  h,  // 4
         h, -h,  h,  // 5
         h,  h,  h,  // 6
        -h,  h,  h,  // 7
    };
    for (int i = 0; i < 24; ++i)
        out_verts[i] = verts[i];

    // 12 triangles x 3 ints = 36 ints (fan-triangulated from 6 quads)
    int faces[36] = {
        0, 1, 2,   0, 2, 3,   // -Z quad (0,1,2,3)
        4, 7, 6,   4, 6, 5,   // +Z quad (4,7,6,5)
        0, 4, 5,   0, 5, 1,   // -Y quad (0,4,5,1)
        2, 6, 7,   2, 7, 3,   // +Y quad (2,6,7,3)
        1, 5, 6,   1, 6, 2,   // +X quad (1,5,6,2)
        0, 3, 7,   0, 7, 4,   // -X quad (0,3,7,4)
    };
    for (int i = 0; i < 36; ++i)
        out_faces[i] = faces[i];
}

// object_tm_identity
// ------------------
// Mirrors pyopennova/ase_from_3di3.py::_tm(translation) with no rot arg.
// Returns 3 identity rows + 1 translation row.
void object_tm_identity(const float translation[3], float out_tm[4][3])
{
    out_tm[0][0] = 1.0f; out_tm[0][1] = 0.0f; out_tm[0][2] = 0.0f;
    out_tm[1][0] = 0.0f; out_tm[1][1] = 1.0f; out_tm[1][2] = 0.0f;
    out_tm[2][0] = 0.0f; out_tm[2][1] = 0.0f; out_tm[2][2] = 1.0f;
    out_tm[3][0] = translation[0];
    out_tm[3][1] = translation[1];
    out_tm[3][2] = translation[2];
}

// object_tm_from_rot
// ------------------
// Mirrors pyopennova/ase_from_3di3.py::_tm(translation, rot=rot) @ line 851.
// Python transposes the 3x3 rotation matrix into TM basis rows:
//   tm row 0 = (rot[0][0], rot[1][0], rot[2][0])  -- column 0 of rot
//   tm row 1 = (rot[0][1], rot[1][1], rot[2][1])  -- column 1 of rot
//   tm row 2 = (rot[0][2], rot[1][2], rot[2][2])  -- column 2 of rot
//   tm row 3 = translation
void object_tm_from_rot(const float rot[3][3], const float translation[3], float out_tm[4][3])
{
    out_tm[0][0] = rot[0][0]; out_tm[0][1] = rot[1][0]; out_tm[0][2] = rot[2][0];
    out_tm[1][0] = rot[0][1]; out_tm[1][1] = rot[1][1]; out_tm[1][2] = rot[2][1];
    out_tm[2][0] = rot[0][2]; out_tm[2][1] = rot[1][2]; out_tm[2][2] = rot[2][2];
    out_tm[3][0] = translation[0];
    out_tm[3][1] = translation[1];
    out_tm[3][2] = translation[2];
}
