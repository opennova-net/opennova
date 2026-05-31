// LW .3di -> IR conversion test. Verifies threedi_ir_from_lw produces a valid
// ThreediModelIR (vertices, indices, primitives, parts, materials) suitable for
// the importer/scene builder.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "threedi/threedi_lw.h"
#include "threedi/threedi_ir.h"
#include "common/test_paths.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static int nearf(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.0001f;
}

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
            CHECK((size_t)pr->vertex_offset + pr->vertex_count <= lod->vertex_count, tag);
            for (uint32_t i = 0; i < pr->index_count; ++i) {
                CHECK(lod->indices[pr->index_offset + i] < pr->vertex_count, tag);
            }
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
        {
            const ThreediIRLod *lod = &ir.lods[0];
            const ThreediIRPrimitive *part1 = &lod->primitives[lod->parts[1].primitive_start];
            const ThreediIRVertex *v = &lod->vertices[part1->vertex_offset];
            CHECK(part1->part_index == 1, "50CAL: part1 primitive is attached to part 1");
            CHECK(nearf(v->position[0], 132.0f / 256.0f), "50CAL: part1 first vertex x uses subobject vertex base");
            CHECK(nearf(v->position[1], 2.0f / 256.0f), "50CAL: part1 first vertex y uses subobject vertex base");
            CHECK(nearf(v->position[2], 217.0f / 256.0f), "50CAL: part1 first vertex z uses subobject vertex base");
            CHECK(nearf(v->normal[0], 0.0f), "50CAL: part1 first normal x uses subobject normal base");
            CHECK(nearf(v->normal[1], 0.9551606f), "50CAL: part1 first normal y uses subobject normal base");
            CHECK(nearf(v->normal[2], 0.2960882f), "50CAL: part1 first normal z uses subobject normal base");
        }
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
    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/JPAN8.3DI", root);
        ThreediModelIR ir;
        threedi_ir_init(&ir);
        CHECK(threedi_ir_read(path, &ir) != 0, "dispatch: v8 fixture must stay unsupported");
        threedi_ir_free(&ir);
    }

    {
        ThreediLwMaterial mat;
        memset(&mat, 0, sizeof(mat));
        strncpy(mat.tex_name_0, "PRIMARY.PCX", sizeof(mat.tex_name_0) - 1);
        strncpy(mat.tex_name_1, "DETAIL.PCX", sizeof(mat.tex_name_1) - 1);

        ThreediLwFile lw;
        threedi_lw_init(&lw);
        lw.version = THREEDI_LW_VERSION_10;
        strncpy(lw.name, "DUALTEX", sizeof(lw.name) - 1);
        lw.material_count = 1;
        lw.materials = &mat;

        ThreediModelIR ir;
        threedi_ir_init(&ir);
        CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic dual texture: conversion");
        CHECK(ir.material_count == 1, "synthetic dual texture: material_count");
        CHECK(ir.materials[0].texture_count == 2, "synthetic dual texture: texture_count");
        CHECK(ir.materials[0].textures[0].slot == THREEDI_IR_TEX_SLOT_DIFFUSE, "synthetic dual texture: diffuse slot");
        CHECK(ir.materials[0].textures[1].slot == THREEDI_IR_TEX_SLOT_DETAIL, "synthetic dual texture: detail slot");
        CHECK(strcmp(ir.materials[0].textures[1].name, "DETAIL.PCX") == 0, "synthetic dual texture: detail name");
        threedi_ir_free(&ir);
        lw.materials = NULL;
    }

    if (failures) { printf("lw_ir test: %d failures\n", failures); return 1; }
    printf("lw_ir test: all checks passed\n");
    return 0;
}
