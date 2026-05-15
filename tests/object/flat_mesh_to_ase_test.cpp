// tests/object/flat_mesh_to_ase_test.cpp
//
// Synthetic FlatMesh (4 verts, 2 faces, no skinning) -> ase_Object.
// Pins: name, parent_name, vert_count, face_count, uv_count, material_ref,
//        smoothing_mask, UV indices, vert positions (with origin offset).

#include "object/flat_mesh_to_ase.h"
#include "object/flat_mesh.h"
#include "object/nlascexp_options.h"
#include "ase/ase.h"
#include "ase/types.h"
#include "threedi/threedi_3di3.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define EXPECT_EQ(actual, expected) \
    do { auto a = (actual); auto e = (expected); \
        if (a != e) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n", \
                __FILE__, __LINE__, #actual, (long long)a, (long long)e); \
            return 1; \
        } \
    } while (0)

#define EXPECT_FLOAT_NEAR(actual, expected, tol) \
    do { float a = (actual); float e = (expected); \
        if (std::abs(a - e) > (tol)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n", \
                __FILE__, __LINE__, #actual, a, e); \
            return 1; \
        } \
    } while (0)

#define EXPECT_STREQ(actual, expected) \
    do { const char* a = (actual); const char* e = (expected); \
        if (std::strcmp(a, e) != 0) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #actual, a, e); \
            return 1; \
        } \
    } while (0)

int main()
{
    // --- Build synthetic FlatMesh ---
    // 4-vert quad split into 2 triangles, no skinning.
    FlatMesh fm = {};
    std::strncpy(fm.name, "01 Mesh0", 63);
    fm.part_index = 0;
    fm.origin[0] = fm.origin[1] = fm.origin[2] = 0.0f;

    FlatMeshVertex verts[4] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    fm.vertex_count = 4;
    fm.vertices = verts;

    // Two triangles: (0,2,1) and (0,3,2), material 0, smoothing bit 0 = mask 1.
    FlatMeshFace faces[2] = {
        {{0, 2, 1}, 0, 1u},
        {{0, 3, 2}, 0, 1u},
    };
    fm.face_count = 2;
    fm.faces = faces;

    // 6 corners (3 per face). u0/v0 are the only used fields.
    FlatMeshFaceCornerData corners[6] = {
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    fm.corners = corners;

    int matset[] = {7};
    fm.material_id_set = matset;
    fm.material_id_set_count = 1;

    // No skinning: leave vertex_influence_offsets / vertex_influences NULL.

    FlatMeshArray arr = {1, &fm};

    // --- Build minimal IR: 1 LOD, 1 render object at (0,0,0) ---
    ThreediRenderObject ro = {};
    ro.abs[0] = 0.0f;
    ro.abs[1] = 0.0f;
    ro.abs[2] = 0.0f;

    ThreediLod lod = {};
    lod.render_object_count = 1;
    lod.render_objects = &ro;

    Threedi3di3 ir = {};
    ir.lod_count = 1;
    ir.lods = &lod;

    NlascexpOptions opts;
    object_nlascexp_options_init_defaults(&opts);

    // --- Run ---
    ase_Object out[1] = {};
    int n = 0;
    int rc = object_emit_main_mesh_objects(
        &arr, &ir, 0, &opts,
        /*global_to_local=*/nullptr, /*g2l_count=*/0,
        out, &n);

    // --- Assert ---
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(n, 1);

    EXPECT_STREQ(out[0].name, "01 Mesh0");
    // parent_name: _export_name("PN01") strips "PN" → "01" (Python line 324)
    EXPECT_STREQ(out[0].parent_name, "01");

    EXPECT_EQ(out[0].vert_count, 4);
    EXPECT_EQ(out[0].face_count, 2);
    EXPECT_EQ(out[0].uv_count, 6);   // face_count * 3

    // material_ref = 7 / 16 = 0
    EXPECT_EQ(out[0].material_ref, 0);

    // node_id should be -1
    EXPECT_EQ(out[0].node_id, -1);

    // skinned = 0 (no weights)
    EXPECT_EQ(out[0].skinned, 0);

    // Vert positions: origin (0,0,0) + fm.vertices[i]
    EXPECT_FLOAT_NEAR(out[0].verts[0], 0.0f, 1e-6f);  // vert 0 x
    EXPECT_FLOAT_NEAR(out[0].verts[1], 0.0f, 1e-6f);  // vert 0 y
    EXPECT_FLOAT_NEAR(out[0].verts[2], 0.0f, 1e-6f);  // vert 0 z
    EXPECT_FLOAT_NEAR(out[0].verts[3], 1.0f, 1e-6f);  // vert 1 x
    EXPECT_FLOAT_NEAR(out[0].verts[4], 0.0f, 1e-6f);  // vert 1 y
    EXPECT_FLOAT_NEAR(out[0].verts[5], 0.0f, 1e-6f);  // vert 1 z

    // Face 0: verts (0,2,1), smoothing_mask=1, uv indices {0,1,2}, material_id 0%16=0
    EXPECT_EQ(out[0].faces[0].vert[0], 0);
    EXPECT_EQ(out[0].faces[0].vert[1], 2);
    EXPECT_EQ(out[0].faces[0].vert[2], 1);
    EXPECT_EQ((int)out[0].faces[0].smoothing_mask, 1);
    EXPECT_EQ(out[0].faces[0].uv[0], 0);
    EXPECT_EQ(out[0].faces[0].uv[1], 1);
    EXPECT_EQ(out[0].faces[0].uv[2], 2);
    EXPECT_EQ(out[0].faces[0].material_id, 0);

    // Face 1: verts (0,3,2), uv indices {3,4,5}
    EXPECT_EQ(out[0].faces[1].vert[0], 0);
    EXPECT_EQ(out[0].faces[1].vert[1], 3);
    EXPECT_EQ(out[0].faces[1].vert[2], 2);
    EXPECT_EQ(out[0].faces[1].uv[0], 3);
    EXPECT_EQ(out[0].faces[1].uv[1], 4);
    EXPECT_EQ(out[0].faces[1].uv[2], 5);

    // Edge visibility: all 1
    EXPECT_EQ((int)out[0].faces[0].edge_visibility[0], 1);
    EXPECT_EQ((int)out[0].faces[0].edge_visibility[1], 1);
    EXPECT_EQ((int)out[0].faces[0].edge_visibility[2], 1);

    // UV[0]: u=0.0, v=1.0, w=0.0
    EXPECT_FLOAT_NEAR(out[0].uvs[0].u, 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].uvs[0].v, 1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].uvs[0].w, 0.0f, 1e-6f);

    // TM: identity rotation, translation (0,0,0).
    EXPECT_FLOAT_NEAR(out[0].tm_row[0][0], 1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[0][1], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[0][2], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[1][0], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[1][1], 1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[1][2], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[2][0], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[2][1], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[2][2], 1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[3][0], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[3][1], 0.0f, 1e-6f);
    EXPECT_FLOAT_NEAR(out[0].tm_row[3][2], 0.0f, 1e-6f);

    // face_normals: now populated from fm->corners (Task 10 fix: match Python path).
    // Python ase_from_3di3.py (deleted Phase D 2026-05-17) passed normals from FlatMesh.face_normals to ase_ffi.
    EXPECT_EQ(out[0].face_normal_count, 2);
    // face_normals pointer should be non-null since corners were provided.
    // Free the allocation to avoid leak in test.
    delete[] out[0].face_normals;
    out[0].face_normals = nullptr;
    out[0].face_normal_count = 0;

    // Cleanup: ase_free only frees ase_Document internals; for a bare ase_Object
    // we leak (test process exits cleanly). No ase_free_object_arrays exists.

    std::printf("PASS flat_mesh_to_ase_test\n");
    return 0;
}
