#include "object/ase_vec.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define EXPECT_FLOAT_NEAR(actual, expected, tol) \
    do { float a = (actual); float e = (expected); \
        if (std::abs(a - e) > (tol)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n", \
                __FILE__, __LINE__, #actual, a, e); \
            return 1; \
        } \
    } while (0)

#define EXPECT_EQ(actual, expected) \
    do { auto a = (actual); auto e = (expected); \
        if (a != e) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n", \
                __FILE__, __LINE__, #actual, (long long)a, (long long)e); \
            return 1; \
        } \
    } while (0)

int main()
{
    // cube_mesh(1.0f) with half=1.0
    // Matches Python cube_mesh(size=2.0) where h = size*0.5 = 1.0
    //
    // Python vertex order:
    //   0: (-1,-1,-1)  1: ( 1,-1,-1)  2: ( 1, 1,-1)  3: (-1, 1,-1)
    //   4: (-1,-1, 1)  5: ( 1,-1, 1)  6: ( 1, 1, 1)  7: (-1, 1, 1)
    //
    // Python quads fan-triangulated -> 12 tris:
    //   -Z: (0,1,2) (0,2,3)
    //   +Z: (4,7,6) (4,6,5)
    //   -Y: (0,4,5) (0,5,1)
    //   +Y: (2,6,7) (2,7,3)
    //   +X: (1,5,6) (1,6,2)
    //   -X: (0,3,7) (0,7,4)
    {
        float verts[24];
        int faces[36];
        object_cube_mesh(1.0f, verts, faces);

        // Verify all 8 vertices (indices 0..7, each 3 floats)
        // vert 0: (-1,-1,-1)
        EXPECT_FLOAT_NEAR(verts[0],  -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[1],  -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[2],  -1.0f, 1e-6f);
        // vert 1: (1,-1,-1)
        EXPECT_FLOAT_NEAR(verts[3],   1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[4],  -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[5],  -1.0f, 1e-6f);
        // vert 2: (1,1,-1)
        EXPECT_FLOAT_NEAR(verts[6],   1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[7],   1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[8],  -1.0f, 1e-6f);
        // vert 3: (-1,1,-1)
        EXPECT_FLOAT_NEAR(verts[9],  -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[10],  1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[11], -1.0f, 1e-6f);
        // vert 4: (-1,-1,1)
        EXPECT_FLOAT_NEAR(verts[12], -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[13], -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[14],  1.0f, 1e-6f);
        // vert 5: (1,-1,1)
        EXPECT_FLOAT_NEAR(verts[15],  1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[16], -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[17],  1.0f, 1e-6f);
        // vert 6: (1,1,1)
        EXPECT_FLOAT_NEAR(verts[18],  1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[19],  1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[20],  1.0f, 1e-6f);
        // vert 7: (-1,1,1)
        EXPECT_FLOAT_NEAR(verts[21], -1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[22],  1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(verts[23],  1.0f, 1e-6f);

        // Verify all 12 triangles (36 ints)
        // tri 0,1: -Z quad (0,1,2,3) -> (0,1,2), (0,2,3)
        EXPECT_EQ(faces[0], 0); EXPECT_EQ(faces[1], 1); EXPECT_EQ(faces[2], 2);
        EXPECT_EQ(faces[3], 0); EXPECT_EQ(faces[4], 2); EXPECT_EQ(faces[5], 3);
        // tri 2,3: +Z quad (4,7,6,5) -> (4,7,6), (4,6,5)
        EXPECT_EQ(faces[6],  4); EXPECT_EQ(faces[7],  7); EXPECT_EQ(faces[8],  6);
        EXPECT_EQ(faces[9],  4); EXPECT_EQ(faces[10], 6); EXPECT_EQ(faces[11], 5);
        // tri 4,5: -Y quad (0,4,5,1) -> (0,4,5), (0,5,1)
        EXPECT_EQ(faces[12], 0); EXPECT_EQ(faces[13], 4); EXPECT_EQ(faces[14], 5);
        EXPECT_EQ(faces[15], 0); EXPECT_EQ(faces[16], 5); EXPECT_EQ(faces[17], 1);
        // tri 6,7: +Y quad (2,6,7,3) -> (2,6,7), (2,7,3)
        EXPECT_EQ(faces[18], 2); EXPECT_EQ(faces[19], 6); EXPECT_EQ(faces[20], 7);
        EXPECT_EQ(faces[21], 2); EXPECT_EQ(faces[22], 7); EXPECT_EQ(faces[23], 3);
        // tri 8,9: +X quad (1,5,6,2) -> (1,5,6), (1,6,2)
        EXPECT_EQ(faces[24], 1); EXPECT_EQ(faces[25], 5); EXPECT_EQ(faces[26], 6);
        EXPECT_EQ(faces[27], 1); EXPECT_EQ(faces[28], 6); EXPECT_EQ(faces[29], 2);
        // tri 10,11: -X quad (0,3,7,4) -> (0,3,7), (0,7,4)
        EXPECT_EQ(faces[30], 0); EXPECT_EQ(faces[31], 3); EXPECT_EQ(faces[32], 7);
        EXPECT_EQ(faces[33], 0); EXPECT_EQ(faces[34], 7); EXPECT_EQ(faces[35], 4);

        // Verify non-unit half-extent scales correctly
        float v2[24]; int f2[36];
        object_cube_mesh(0.5f, v2, f2);
        EXPECT_FLOAT_NEAR(v2[0], -0.5f, 1e-6f);
        EXPECT_FLOAT_NEAR(v2[3],  0.5f, 1e-6f);
    }

    // tm_identity: translation (1,2,3) -> tm[3] = (1,2,3), tm[0..2] = identity
    {
        float tr[3] = {1.0f, 2.0f, 3.0f};
        float tm[4][3];
        object_tm_identity(tr, tm);
        EXPECT_FLOAT_NEAR(tm[0][0], 1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[0][1], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[0][2], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][0], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][1], 1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][2], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][0], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][1], 0.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][2], 1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][0], 1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][1], 2.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][2], 3.0f, 1e-6f);
    }

    // tm_from_rot: transposes rot into TM basis rows + translation.
    // Python: rows[0] = (rot[0][0], rot[1][0], rot[2][0]) = (1,4,7)
    //         rows[1] = (rot[0][1], rot[1][1], rot[2][1]) = (2,5,8)
    //         rows[2] = (rot[0][2], rot[1][2], rot[2][2]) = (3,6,9)
    {
        float rot[3][3] = {{1.0f,2.0f,3.0f}, {4.0f,5.0f,6.0f}, {7.0f,8.0f,9.0f}};
        float tr[3] = {10.0f, 11.0f, 12.0f};
        float tm[4][3];
        object_tm_from_rot(rot, tr, tm);
        EXPECT_FLOAT_NEAR(tm[0][0], 1.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[0][1], 4.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[0][2], 7.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][0], 2.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][1], 5.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[1][2], 8.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][0], 3.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][1], 6.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[2][2], 9.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][0], 10.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][1], 11.0f, 1e-6f);
        EXPECT_FLOAT_NEAR(tm[3][2], 12.0f, 1e-6f);
    }

    std::printf("PASS ase_vec_test\n");
    return 0;
}
