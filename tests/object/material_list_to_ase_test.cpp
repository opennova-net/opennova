// tests/object/material_list_to_ase_test.cpp
//
// Unit tests for:
//   object_build_global_to_local_map — slim material ID collection
//   object_populate_material_list    — slot allocation + submaterial population

#include "object/material_list_to_ase.h"
#include "ase/ase.h"
#include "ase/types.h"
#include "threedi/threedi_3di3.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

#define EXPECT_EQ(actual, expected) \
    do { auto a = (actual); auto e = (expected); \
        if (a != e) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n", \
                __FILE__, __LINE__, #actual, (long long)a, (long long)e); \
            return 1; \
        } \
    } while (0)

#define EXPECT_FLOAT_NEAR(actual, expected, tol) \
    do { float a = (float)(actual); float e = (float)(expected); \
        float diff = a - e; if (diff < 0.f) diff = -diff; \
        if (diff > (tol)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6f, expected %.6f\n", \
                __FILE__, __LINE__, #actual, (double)a, (double)e); \
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

// ---------------------------------------------------------------------------
// Test 1: object_build_global_to_local_map — dedup + sort
// ---------------------------------------------------------------------------
//
// Two specs:
//   spec 0: material_id_set = {3, 5},  face_material_ids = {3}
//   spec 1: material_id_set = {0},     face_material_ids = {5, 0}
//
// Expected sorted_ids = [0, 3, 5], global_to_local = [-1..] except
//   [0]=0, [3]=1, [5]=2.

static int test_build_global_to_local()
{
    // Flat per-spec arrays.
    int mat_id_sets[]            = {3, 5,  0};     // spec0: {3,5}, spec1: {0}
    int mat_id_set_counts[]      = {2,     1};
    int face_mat_ids[]           = {3,  5, 0};     // spec0: {3}, spec1: {5,0}
    int face_mat_id_counts[]     = {1,     2};
    int spec_count               = 2;
    int mat_upper_bound          = 8;

    int sorted_ids[8]            = {-1,-1,-1,-1,-1,-1,-1,-1};
    int g2l[8]                   = {0};

    int n = object_build_global_to_local_map(
        mat_id_sets, mat_id_set_counts,
        face_mat_ids, face_mat_id_counts,
        spec_count, mat_upper_bound,
        sorted_ids, 8,
        g2l, 8);

    EXPECT_EQ(n, 3);
    EXPECT_EQ(sorted_ids[0], 0);
    EXPECT_EQ(sorted_ids[1], 3);
    EXPECT_EQ(sorted_ids[2], 5);

    EXPECT_EQ(g2l[0], 0);
    EXPECT_EQ(g2l[1], -1);
    EXPECT_EQ(g2l[2], -1);
    EXPECT_EQ(g2l[3], 1);
    EXPECT_EQ(g2l[4], -1);
    EXPECT_EQ(g2l[5], 2);
    EXPECT_EQ(g2l[6], -1);
    EXPECT_EQ(g2l[7], -1);

    std::printf("PASS build_global_to_local\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Test 2: empty spec list → 0 IDs
// ---------------------------------------------------------------------------

static int test_build_global_to_local_empty()
{
    int counts[] = {0};
    int sorted_ids[8] = {};
    int g2l[8] = {};
    int n = object_build_global_to_local_map(
        nullptr, counts, nullptr, counts,
        0, 4,
        sorted_ids, 8, g2l, 8);
    EXPECT_EQ(n, 0);
    std::printf("PASS build_global_to_local_empty\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Test 3: object_populate_material_list — 3-material IR, 2 used
// ---------------------------------------------------------------------------
//
// IR has 3 materials at global indices 0, 1, 2.
// We use sorted_global_ids = {0, 2} → 2 subs in 1 slot.

static int test_populate_material_list()
{
    // Build minimal IR with 3 materials.
    ThreediMaterial mats[3] = {};

    // mat 0: shader "FF_ST_OP", diffuse texture "tex0.tga"
    std::strncpy(mats[0].shader_name, "FF_ST_OP", 32);
    mats[0].texture_count = 1;
    mats[0].textures[0].slot = THREEDI_TEX_SLOT_DIFFUSE;
    mats[0].textures[0].type = THREEDI_TEX_TYPE_DIFFUSE;
    std::strncpy(mats[0].textures[0].name, "tex0.tga", 16);

    // mat 1: not referenced by this LOD — will be skipped.
    std::strncpy(mats[1].shader_name, "FF_ST_AB", 32);

    // mat 2: shader "VS_PHONGOP", two-sided, alpha-test, detail texture
    std::strncpy(mats[2].shader_name, "VS_PHONGOP", 32);
    mats[2].material_flags = THREEDI_MATERIAL_FLAG_ALPHA_TEST |
                             THREEDI_MATERIAL_FLAG_TWO_SIDED;
    mats[2].texture_count = 2;
    mats[2].textures[0].slot = THREEDI_TEX_SLOT_DIFFUSE;
    std::strncpy(mats[2].textures[0].name, "tex2d.tga", 16);
    mats[2].textures[1].slot = THREEDI_TEX_SLOT_DETAIL;
    std::strncpy(mats[2].textures[1].name, "tex2det.tga", 16);

    Threedi3di3 ir = {};
    ir.material_count = 3;
    ir.materials = mats;

    // sorted_global_ids: use indices 0 and 2 only.
    int sorted_ids[2] = {0, 2};
    int sorted_count  = 2;

    // Allocate document: 1 material slot holds both subs (SUBS_PER_SLOT=64 >> 2).
    ase_Document doc = {};
    ase_alloc(&doc, 0, 1, 0);

    int rc = object_populate_material_list(&ir, sorted_ids, sorted_count, &doc);
    EXPECT_EQ(rc, 1);                  // 1 slot used
    EXPECT_EQ(doc.material_count, 1);  // caller set this via ase_alloc

    ase_Material* slot0 = &doc.materials[0];
    EXPECT_EQ(slot0->submaterial_count, 2);

    // Sub 0 comes from mat[0] (global_id=0).
    // Name format: "Material_{global_id}_{shader}" (Python materials.py line 406).
    ase_Material* sub0 = &slot0->submaterials[0];
    EXPECT_STREQ(sub0->name, "Material_0_FF_ST_OP");
    EXPECT_STREQ(sub0->maps[0], "tex0.tga");
    // No detail → maps[1] empty.
    EXPECT_EQ((int)sub0->maps[1][0], 0);
    // No alpha test → maps[2] empty.
    EXPECT_EQ((int)sub0->maps[2][0], 0);
    // Not two-sided.
    EXPECT_EQ((int)(sub0->extra_flags & 1), 0);
    EXPECT_FLOAT_NEAR(sub0->diffuse[0], 0.5882f, 0.001f);
    EXPECT_FLOAT_NEAR(sub0->ambient[0], 0.0588f, 0.001f);

    // Sub 1 comes from mat[2] (global_id=2).
    ase_Material* sub1 = &slot0->submaterials[1];
    EXPECT_STREQ(sub1->name, "Material_2_VS_PHONGOP");
    EXPECT_STREQ(sub1->maps[0], "tex2d.tga");
    EXPECT_STREQ(sub1->maps[1], "tex2det.tga");
    // Alpha test + diffuse → maps[2] = diffuse name.
    EXPECT_STREQ(sub1->maps[2], "tex2d.tga");
    // Two-sided → extra_flags bit 0 set.
    EXPECT_EQ((int)(sub1->extra_flags & 1), 1);
    // VS_PHONGOP contains "PHONG" → shading = 1.
    EXPECT_EQ(sub1->shading, 1);

    ase_free(&doc);
    std::printf("PASS populate_material_list\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Test 4: empty sorted_ids → 0 slots, returns 0
// ---------------------------------------------------------------------------

static int test_populate_material_list_empty()
{
    Threedi3di3 ir = {};
    ir.material_count = 3;
    // No materials array needed since sorted_count=0.

    ase_Document doc = {};
    ase_alloc(&doc, 0, 0, 0); // 0 material slots

    int rc = object_populate_material_list(&ir, nullptr, 0, &doc);
    EXPECT_EQ(rc, 0);
    std::printf("PASS populate_material_list_empty\n");
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    int r = 0;
    r |= test_build_global_to_local();
    r |= test_build_global_to_local_empty();
    r |= test_populate_material_list();
    r |= test_populate_material_list_empty();

    if (r == 0) std::printf("PASS material_list_to_ase_test\n");
    return r;
}
