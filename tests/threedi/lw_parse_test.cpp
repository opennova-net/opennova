// Land Warrior (.3di v10) parse + IR-conversion test.
//
// Golden values were derived from Dflw.exe RE and the sample corpus
// (notes/3di-lw). Two checked-in fixtures cover both paths:
//   BADGUY.3DI - skeleton-skinned character (flags&1), exercises per-vertex
//                vertex.w bone assignment and cross-bone primitives.
//   50CAL.3DI  - rigid multi-part static model (flags&0 LODs), exercises the
//                material_index fallback that prevents over-dropping faces.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "threedi/threedi_ir.h"
#include "threedi/threedi_lw.h"
#include "common/test_paths.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: ");                                       \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// BADGUY: skinned character. Confirms golden parse counts, that the model is
// promoted as SKINNED (not silently demoted to BASIC, which would make the bone
// data dead), that no faces are dropped, and that cross-bone primitives survive.
static void test_badguy(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/BADGUY.3DI", root);

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    if (threedi_lw_read(path, &lw) != 0) {
        fprintf(stderr, "  FAIL: cannot read %s\n", path);
        ++g_failures;
        return;
    }

    CHECK(lw.version == THREEDI_LW_VERSION_10, "BADGUY version %d != 10", (int)lw.version);
    CHECK(lw.lod_count == 4, "BADGUY lod_count %u != 4", lw.lod_count);
    if (lw.lod_count >= 1) {
        const ThreediLwLod *l = &lw.lods[0];
        CHECK(l->vertex_count == 407, "BADGUY LOD0 verts %u != 407", l->vertex_count);
        CHECK(l->face_count == 492, "BADGUY LOD0 faces %u != 492", l->face_count);
        CHECK(l->subobject_count == 15, "BADGUY LOD0 subobjects %u != 15", l->subobject_count);
        CHECK((l->flags & 1u) != 0, "BADGUY LOD0 missing skinned flag (flags=0x%X)", l->flags);
    }

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    if (threedi_ir_from_lw(&lw, &ir) != 0) {
        fprintf(stderr, "  FAIL: BADGUY IR conversion failed\n");
        ++g_failures;
        threedi_lw_free(&lw);
        return;
    }

    CHECK(ir.mesh_type == THREEDI_IR_MESH_SKINNED,
          "BADGUY mesh_type %d != SKINNED(3) (skinning would be dead)", (int)ir.mesh_type);
    CHECK(ir.lod_count == 4, "BADGUY IR lod_count %zu != 4", ir.lod_count);

    if (ir.lod_count >= 1) {
        const ThreediIRLod *l = &ir.lods[0];
        CHECK(l->part_count == 15, "BADGUY LOD0 part_count %zu != 15", l->part_count);
        // All 492 faces resolve to a material (none hidden) -> 492*3 corners.
        CHECK(l->vertex_count == 1476,
              "BADGUY LOD0 emitted corners %zu != 1476 (silent face drop?)", l->vertex_count);
        CHECK(l->index_count == 1476, "BADGUY LOD0 index_count %zu != 1476", l->index_count);

        int multibone = 0;
        size_t max_bones = 0;
        int empty_table = 0;
        for (size_t p = 0; p < l->primitive_count; ++p) {
            uint8_t bl = l->primitives[p].bone_table_length;
            if (bl > max_bones) max_bones = bl;
            if (bl >= 2) multibone = 1;
            if (bl == 0) empty_table = 1;
        }
        CHECK(multibone,
              "BADGUY LOD0 has no multi-bone primitive (max bones/prim=%zu); cross-bone skinning lost",
              max_bones);
        CHECK(!empty_table, "BADGUY LOD0 has a skinned primitive with an empty bone table");
    }

    threedi_ir_free(&ir);
    threedi_lw_free(&lw);
}

// 50CAL: rigid static gun. Confirms BASIC promotion and that the material_index
// fallback keeps every non-hidden face. The old selector-only resolver dropped
// 8 visible faces here (kept 117); the fallback keeps all 125 (40 are genuinely
// hidden DONTDRAW surfaces).
static void test_50cal(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/50CAL.3DI", root);

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    if (threedi_lw_read(path, &lw) != 0) {
        fprintf(stderr, "  FAIL: cannot read %s\n", path);
        ++g_failures;
        return;
    }

    CHECK(lw.version == THREEDI_LW_VERSION_10, "50CAL version %d != 10", (int)lw.version);
    CHECK(lw.lod_count == 4, "50CAL lod_count %u != 4", lw.lod_count);
    if (lw.lod_count >= 1) {
        const ThreediLwLod *l = &lw.lods[0];
        CHECK(l->vertex_count == 192, "50CAL LOD0 verts %u != 192", l->vertex_count);
        CHECK(l->face_count == 165, "50CAL LOD0 faces %u != 165", l->face_count);
        CHECK((l->flags & 1u) == 0, "50CAL LOD0 unexpectedly skinned (flags=0x%X)", l->flags);
    }

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    if (threedi_ir_from_lw(&lw, &ir) != 0) {
        fprintf(stderr, "  FAIL: 50CAL IR conversion failed\n");
        ++g_failures;
        threedi_lw_free(&lw);
        return;
    }

    CHECK(ir.mesh_type == THREEDI_IR_MESH_BASIC, "50CAL mesh_type %d != BASIC(1)", (int)ir.mesh_type);
    if (ir.lod_count >= 1) {
        const ThreediIRLod *l = &ir.lods[0];
        CHECK(l->part_count == 3, "50CAL LOD0 part_count %zu != 3", l->part_count);
        // 165 faces - 40 hidden (DONTDRAW) = 125 kept -> 375 corners.
        CHECK(l->vertex_count == 375,
              "50CAL LOD0 emitted corners %zu != 375 (over-drop regression: expected 125 kept faces)",
              l->vertex_count);
    }

    threedi_ir_free(&ir);
    threedi_lw_free(&lw);
}

int main(void) {
    const char *root = test_paths_repo_root(__FILE__);
    printf("LW .3di import tests (fixtures/threedi/lw)\n");

    test_badguy(root);
    test_50cal(root);

    if (g_failures) {
        printf("LW parse test: %d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    printf("All LW import checks passed.\n");
    return EXIT_SUCCESS;
}
