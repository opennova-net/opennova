// LW .3di -> IR conversion test. Verifies threedi_ir_from_lw produces a valid
// ThreediModelIR (vertices, indices, primitives, parts, materials) suitable for
// the importer/scene builder.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "threedi/threedi_lw.h"
#include "threedi/threedi_ir.h"
#include "common/test_paths.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void load_ir(const char *root, const char *file, ThreediModelIR *ir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/%s", root, file);
    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_read(path, &m) == 0, "lw_read failed");
    threedi_ir_init(ir);
    CHECK(threedi_ir_from_lw(&m, ir) == 0, "threedi_ir_from_lw failed");
    threedi_lw_free(&m);
}

// All indices reference valid vertices; every primitive points at a valid part
// and material; parts cover the primitives.
static void check_ir_integrity(const ThreediModelIR *ir, const char *tag) {
    CHECK(ir->lod_count >= 1, tag);
    for (size_t l = 0; l < ir->lod_count; ++l) {
        const ThreediIRLod *lod = &ir->lods[l];
        CHECK(lod->part_count >= 1, tag);
        for (size_t i = 0; i < lod->index_count; ++i) {
            CHECK(lod->indices[i] < lod->vertex_count, tag);
        }
        for (size_t p = 0; p < lod->primitive_count; ++p) {
            const ThreediIRPrimitive *pr = &lod->primitives[p];
            CHECK(pr->part_index >= 0 && (size_t)pr->part_index < lod->part_count, tag);
            CHECK(pr->material_index >= 0 && (size_t)pr->material_index < ir->material_count, tag);
            CHECK((size_t)pr->index_offset + pr->index_count <= lod->index_count, tag);
        }
    }
}

int main(void) {
    const char *root = test_paths_repo_root(__FILE__);

    {
        ThreediModelIR ir;
        load_ir(root, "ARBLU.3DI", &ir);
        CHECK(ir.source_format == THREEDI_IR_SOURCE_LW10, "ARBLU: source_format");
        CHECK(ir.lod_count == 1, "ARBLU: lod_count");
        CHECK(ir.material_count == 1, "ARBLU: material_count");
        CHECK(strcmp((char*)ir.materials[0].textures[0].name, "AMIDAL.PCX") == 0, "ARBLU: tex name");
        CHECK(ir.lods[0].vertex_count == 3, "ARBLU: vertex_count == 3 (1 face x 3)");
        CHECK(ir.lods[0].index_count == 3, "ARBLU: index_count == 3");
        CHECK(ir.lods[0].part_count == 1, "ARBLU: part_count == 1 (1 subobject)");
        CHECK(ir.mesh_type == THREEDI_IR_MESH_BASIC, "ARBLU: mesh_type BASIC (single part)");
        check_ir_integrity(&ir, "ARBLU: IR integrity");
        threedi_ir_free(&ir);
    }
    {
        ThreediModelIR ir;
        load_ir(root, "50CAL.3DI", &ir);
        CHECK(ir.lod_count == 4, "50CAL: lod_count == 4");
        CHECK(ir.material_count == 7, "50CAL: material_count == 7");
        CHECK(ir.lods[0].vertex_count == 165 * 3, "50CAL: LOD0 vertex_count == 495");
        CHECK(ir.lods[0].index_count == 165 * 3, "50CAL: LOD0 index_count == 495");
        CHECK(ir.lods[0].part_count == 3, "50CAL: LOD0 part_count == 3 (3 subobjects)");
        CHECK(ir.mesh_type == THREEDI_IR_MESH_SKINNED, "50CAL: mesh_type SKINNED (multi-part)");
        // parent hierarchy carried through: part 2's parent is part 1
        CHECK(ir.lods[0].parts[2].parent_index == 1, "50CAL: part[2].parent_index == 1");
        check_ir_integrity(&ir, "50CAL: IR integrity");
        threedi_ir_free(&ir);
    }

    // Dispatcher: threedi_ir_read must auto-detect LW by magic and route to the
    // LW converter (this is what the onimport FFI calls).
    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/ARBLU.3DI", root);
        ThreediModelIR ir;
        threedi_ir_init(&ir);
        CHECK(threedi_ir_read(path, &ir) == 0, "dispatch: ARBLU read via threedi_ir_read");
        CHECK(ir.source_format == THREEDI_IR_SOURCE_LW10, "dispatch: source_format LW10");
        CHECK(ir.lod_count == 1, "dispatch: lod_count");
        threedi_ir_free(&ir);
    }

    if (failures) { printf("lw_ir test: %d failures\n", failures); return 1; }
    printf("lw_ir test: all checks passed\n");
    return 0;
}
