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

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void ir_to_godot_vec3(const float in[3], float out[3]) {
    out[0] = -in[0];
    out[1] = in[1];
    out[2] = in[2];
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

static void test_lod_flag_one_emits_skinning_in_model_space(void) {
    ThreediLwMaterial mat;
    memset(&mat, 0, sizeof(mat));

    ThreediLwSurface surface;
    memset(&surface, 0, sizeof(surface));
    surface.material_index = 0;

    ThreediLwSubObject parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].parent = 0;
    parts[1].vertex_count = 3;
    parts[1].face_count = 1;
    parts[1].normal_count = 1;
    parts[1].parent = 0;
    parts[1].pos[0] = 65536;    // 1.0 in LW subobject 16.16 space.
    parts[1].pos[1] = -131072;  // -2.0
    parts[1].pos[2] = 196608;   // 3.0

    ThreediLwVertex verts[3];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 266; verts[0].y = -508; verts[0].z = 773; verts[0].w = 1;
    verts[1].x = 276; verts[1].y = -508; verts[1].z = 773; verts[1].w = 1;
    verts[2].x = 266; verts[2].y = -502; verts[2].z = 778; verts[2].w = 1;

    ThreediLwVertex normals[1];
    memset(normals, 0, sizeof(normals));
    normals[0].x = 0;
    normals[0].y = 3;
    normals[0].z = 4;

    ThreediLwFace face;
    memset(&face, 0, sizeof(face));
    face.vertex[0] = 0;
    face.vertex[1] = 1;
    face.vertex[2] = 2;
    face.normal[0] = 0;
    face.normal[1] = 0;
    face.normal[2] = 0;
    face.surface_index = 0;

    ThreediLwLod lod;
    memset(&lod, 0, sizeof(lod));
    lod.flags = 1;
    lod.vertex_count = 3;
    lod.normal_count = 1;
    lod.faceref_count = 1;
    lod.subobject_count = 2;
    lod.surface_count = 1;
    lod.vertices = verts;
    lod.normals = normals;
    lod.faces = &face;
    lod.subobjects = parts;
    lod.surfaces = &surface;

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    lw.version = THREEDI_LW_VERSION_10;
    lw.lod_count = 1;
    lw.material_count = 1;
    lw.materials = &mat;
    lw.lods = &lod;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic flag1: conversion");
    CHECK(ir.lods[0].part_count == 2, "synthetic flag1: part_count");
    CHECK(nearf(ir.lods[0].parts[1].abs_position[0], 2.0f), "synthetic flag1: pivot raw y maps to IR x");
    CHECK(nearf(ir.lods[0].parts[1].abs_position[1], 3.0f), "synthetic flag1: pivot raw z maps to IR y");
    CHECK(nearf(ir.lods[0].parts[1].abs_position[2], 1.0f), "synthetic flag1: pivot raw x maps to IR z");
    CHECK(nearf(ir.lods[0].vertices[0].position[0], 508.0f / 256.0f), "synthetic flag1: model raw y maps to IR x");
    CHECK(nearf(ir.lods[0].vertices[1].position[0], 502.0f / 256.0f), "synthetic flag1: reversed corner raw y maps to IR x");
    CHECK(nearf(ir.lods[0].vertices[1].position[1], 778.0f / 256.0f), "synthetic flag1: model raw z maps to IR y");
    CHECK(nearf(ir.lods[0].vertices[2].position[2], 276.0f / 256.0f), "synthetic flag1: second model raw x maps to IR z");
    CHECK(nearf(ir.lods[0].vertices[0].normal[0], -0.6f), "synthetic flag1: normal raw y maps to IR x");
    CHECK(nearf(ir.lods[0].vertices[0].normal[1], 0.8f), "synthetic flag1: normal raw z maps to IR y");
    CHECK(nearf(ir.lods[0].vertices[0].normal[2], 0.0f), "synthetic flag1: normal raw x maps to IR z");
    CHECK(ir.lods[0].primitive_count == 1, "synthetic flag1: primitive_count");
    CHECK(ir.lods[0].primitives[0].bone_table_length == 1, "synthetic flag1: bone table length");
    CHECK(ir.lods[0].primitives[0].bone_table[0] == 1, "synthetic flag1: vertex w selects skeleton bone");
    CHECK(ir.lods[0].vertices[0].bone_indices[0] == 0, "synthetic flag1: vertex uses local bone slot");
    CHECK(nearf(ir.lods[0].vertices[0].bone_weights[0], 1.0f), "synthetic flag1: vertex bone weight");
    check_ir_integrity(&ir, "synthetic flag1: IR integrity");
    threedi_ir_free(&ir);
}

static void test_lod_flag_zero_keeps_model_space_vertices(void) {
    ThreediLwMaterial mat;
    memset(&mat, 0, sizeof(mat));

    ThreediLwSurface surface;
    memset(&surface, 0, sizeof(surface));
    surface.material_index = 0;

    ThreediLwSubObject parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].parent = 0;
    parts[1].vertex_count = 3;
    parts[1].face_count = 1;
    parts[1].normal_count = 1;
    parts[1].parent = 0;
    parts[1].pos[0] = 65536;    // Present on disk, but flag-0 LOD vertices are already model-space.
    parts[1].pos[1] = -131072;
    parts[1].pos[2] = 196608;

    ThreediLwVertex verts[3];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 266; verts[0].y = 2; verts[0].z = 217; verts[0].w = 1;
    verts[1].x = 276; verts[1].y = 2; verts[1].z = 217; verts[1].w = 1;
    verts[2].x = 266; verts[2].y = 10; verts[2].z = 227; verts[2].w = 1;

    ThreediLwVertex normals[1];
    memset(normals, 0, sizeof(normals));
    normals[0].x = 0;
    normals[0].y = 3;
    normals[0].z = 4;

    ThreediLwFace face;
    memset(&face, 0, sizeof(face));
    face.vertex[0] = 0;
    face.vertex[1] = 1;
    face.vertex[2] = 2;
    face.normal[0] = 0;
    face.normal[1] = 0;
    face.normal[2] = 0;
    face.surface_index = 0;

    ThreediLwLod lod;
    memset(&lod, 0, sizeof(lod));
    lod.flags = 0;
    lod.vertex_count = 3;
    lod.normal_count = 1;
    lod.faceref_count = 1;
    lod.subobject_count = 2;
    lod.surface_count = 1;
    lod.vertices = verts;
    lod.normals = normals;
    lod.faces = &face;
    lod.subobjects = parts;
    lod.surfaces = &surface;

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    lw.version = THREEDI_LW_VERSION_10;
    lw.lod_count = 1;
    lw.material_count = 1;
    lw.materials = &mat;
    lw.lods = &lod;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic flag0: conversion");
    CHECK(ir.lods[0].part_count == 2, "synthetic flag0: part_count");
    CHECK(nearf(ir.lods[0].parts[1].abs_position[0], 0.0f), "synthetic flag0: part transform stays identity");
    CHECK(nearf(ir.lods[0].vertices[0].position[0], -2.0f / 256.0f), "synthetic flag0: model-space raw y maps to IR x");
    CHECK(nearf(ir.lods[0].vertices[1].position[2], 266.0f / 256.0f), "synthetic flag0: reversed corner raw x maps to IR z");
    CHECK(nearf(ir.lods[0].vertices[0].position[1], 217.0f / 256.0f), "synthetic flag0: model-space raw z maps to IR y");
    CHECK(nearf(ir.lods[0].vertices[2].position[2], 276.0f / 256.0f), "synthetic flag0: second model-space raw x maps to IR z");
    CHECK(nearf(ir.lods[0].vertices[0].normal[0], -0.6f), "synthetic flag0: normal raw y maps to IR x");
    CHECK(nearf(ir.lods[0].vertices[0].normal[1], 0.8f), "synthetic flag0: normal raw z maps to IR y");
    CHECK(nearf(ir.lods[0].vertices[0].normal[2], 0.0f), "synthetic flag0: normal raw x maps to IR z");
    check_ir_integrity(&ir, "synthetic flag0: IR integrity");
    threedi_ir_free(&ir);
}

static void test_faces_emit_godot_clockwise_winding(void) {
    ThreediLwMaterial mat;
    memset(&mat, 0, sizeof(mat));

    ThreediLwSurface surface;
    memset(&surface, 0, sizeof(surface));
    surface.material_index = 0;

    ThreediLwSubObject part;
    memset(&part, 0, sizeof(part));
    part.vertex_count = 3;
    part.face_count = 1;
    part.normal_count = 1;

    ThreediLwVertex verts[3];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 0;   verts[0].y = 0;   verts[0].z = 0;
    verts[1].x = 256; verts[1].y = 0;   verts[1].z = 0;
    verts[2].x = 0;   verts[2].y = 256; verts[2].z = 0;

    ThreediLwVertex normals[1];
    memset(normals, 0, sizeof(normals));
    normals[0].z = 256;

    ThreediLwFace face;
    memset(&face, 0, sizeof(face));
    face.vertex[0] = 0;
    face.vertex[1] = 1;
    face.vertex[2] = 2;
    face.normal[0] = 0;
    face.normal[1] = 0;
    face.normal[2] = 0;
    face.surface_index = 0;

    ThreediLwLod lod;
    memset(&lod, 0, sizeof(lod));
    lod.vertex_count = 3;
    lod.normal_count = 1;
    lod.faceref_count = 1;
    lod.subobject_count = 1;
    lod.surface_count = 1;
    lod.vertices = verts;
    lod.normals = normals;
    lod.faces = &face;
    lod.subobjects = &part;
    lod.surfaces = &surface;

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    lw.version = THREEDI_LW_VERSION_10;
    lw.lod_count = 1;
    lw.material_count = 1;
    lw.materials = &mat;
    lw.lods = &lod;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic winding: conversion");
    const ThreediIRLod *out_lod = &ir.lods[0];
    const ThreediIRPrimitive *prim = &out_lod->primitives[0];
    const ThreediIRVertex *v0 = &out_lod->vertices[prim->vertex_offset + out_lod->indices[prim->index_offset + 0]];
    const ThreediIRVertex *v1 = &out_lod->vertices[prim->vertex_offset + out_lod->indices[prim->index_offset + 1]];
    const ThreediIRVertex *v2 = &out_lod->vertices[prim->vertex_offset + out_lod->indices[prim->index_offset + 2]];
    float p0[3], p1[3], p2[3], n0[3];
    ir_to_godot_vec3(v0->position, p0);
    ir_to_godot_vec3(v1->position, p1);
    ir_to_godot_vec3(v2->position, p2);
    ir_to_godot_vec3(v0->normal, n0);
    float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    float winding_normal[3];
    cross3(e1, e2, winding_normal);
    CHECK(dot3(winding_normal, n0) < 0.0f, "synthetic winding: emits Godot clockwise faces");
    threedi_ir_free(&ir);
}

static void test_surface_names_resolve_lw_materials(void) {
    ThreediLwMaterial mats[2];
    memset(mats, 0, sizeof(mats));
    strncpy(mats[0].tex_name_0, "BODY.PCX", sizeof(mats[0].tex_name_0) - 1);
    strncpy(mats[1].tex_name_0, "FACE.PCX", sizeof(mats[1].tex_name_0) - 1);

    ThreediLwSurface surfaces[2];
    memset(surfaces, 0, sizeof(surfaces));
    strncpy(surfaces[0].name, "BODY.PCX", sizeof(surfaces[0].name) - 1);
    strncpy(surfaces[1].name, "face.pcx", sizeof(surfaces[1].name) - 1);
    surfaces[0].material_index = 0;
    surfaces[1].material_index = 0; // LW links this from surface name/ref data after load.

    ThreediLwSubObject part;
    memset(&part, 0, sizeof(part));
    part.vertex_count = 4;
    part.face_count = 2;
    part.normal_count = 1;

    ThreediLwVertex verts[4];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 0;   verts[0].y = 0;   verts[0].z = 0;
    verts[1].x = 256; verts[1].y = 0;   verts[1].z = 0;
    verts[2].x = 0;   verts[2].y = 256; verts[2].z = 0;
    verts[3].x = 256; verts[3].y = 256; verts[3].z = 0;

    ThreediLwVertex normals[1];
    memset(normals, 0, sizeof(normals));
    normals[0].z = 256;

    ThreediLwFace faces[2];
    memset(faces, 0, sizeof(faces));
    faces[0].vertex[0] = 0; faces[0].vertex[1] = 1; faces[0].vertex[2] = 2;
    faces[0].normal[0] = 0; faces[0].normal[1] = 0; faces[0].normal[2] = 0;
    faces[0].surface_index = 0;
    faces[1].vertex[0] = 1; faces[1].vertex[1] = 3; faces[1].vertex[2] = 2;
    faces[1].normal[0] = 0; faces[1].normal[1] = 0; faces[1].normal[2] = 0;
    faces[1].surface_index = 1;

    ThreediLwLod lod;
    memset(&lod, 0, sizeof(lod));
    lod.vertex_count = 4;
    lod.normal_count = 1;
    lod.faceref_count = 2;
    lod.subobject_count = 1;
    lod.surface_count = 2;
    lod.vertices = verts;
    lod.normals = normals;
    lod.faces = faces;
    lod.subobjects = &part;
    lod.surfaces = surfaces;

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    lw.version = THREEDI_LW_VERSION_10;
    lw.lod_count = 1;
    lw.material_count = 2;
    lw.materials = mats;
    lw.lods = &lod;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic surface names: conversion");
    CHECK(ir.lods[0].primitive_count == 2, "synthetic surface names: two material primitives");
    CHECK(ir.lods[0].primitives[0].material_index == 0, "synthetic surface names: body material");
    CHECK(ir.lods[0].primitives[1].material_index == 1, "synthetic surface names: face material");
    CHECK(strcmp(ir.materials[1].textures[0].name, "FACE.PCX") == 0, "synthetic surface names: face texture");
    threedi_ir_free(&ir);
}

static void test_surface_selector_resolves_lw_materials(void) {
    ThreediLwMaterial mats[2];
    memset(mats, 0, sizeof(mats));
    strncpy(mats[0].tex_name_0, "BODY.PCX", sizeof(mats[0].tex_name_0) - 1);
    mats[0].selector_id = 0;
    strncpy(mats[1].tex_name_0, "SUIT.PCX", sizeof(mats[1].tex_name_0) - 1);
    mats[1].selector_id = 7;

    ThreediLwSurface surface;
    memset(&surface, 0, sizeof(surface));
    strncpy(surface.name, "MISLEAD.PCX", sizeof(surface.name) - 1);
    surface.flags = 1;
    surface.material_index = 0;
    surface.material_selectors[0] = 7;

    ThreediLwSubObject part;
    memset(&part, 0, sizeof(part));
    part.vertex_count = 3;
    part.face_count = 1;
    part.normal_count = 1;

    ThreediLwVertex verts[3];
    memset(verts, 0, sizeof(verts));
    verts[0].x = 0;   verts[0].y = 0;   verts[0].z = 0;
    verts[1].x = 256; verts[1].y = 0;   verts[1].z = 0;
    verts[2].x = 0;   verts[2].y = 256; verts[2].z = 0;

    ThreediLwVertex normals[1];
    memset(normals, 0, sizeof(normals));
    normals[0].z = 256;

    ThreediLwFace face;
    memset(&face, 0, sizeof(face));
    face.vertex[0] = 0;
    face.vertex[1] = 1;
    face.vertex[2] = 2;
    face.normal[0] = 0;
    face.normal[1] = 0;
    face.normal[2] = 0;
    face.surface_index = 0;

    ThreediLwLod lod;
    memset(&lod, 0, sizeof(lod));
    lod.flags = 1;
    lod.vertex_count = 3;
    lod.normal_count = 1;
    lod.faceref_count = 1;
    lod.subobject_count = 1;
    lod.surface_count = 1;
    lod.vertices = verts;
    lod.normals = normals;
    lod.faces = &face;
    lod.subobjects = &part;
    lod.surfaces = &surface;

    ThreediLwFile lw;
    threedi_lw_init(&lw);
    lw.version = THREEDI_LW_VERSION_10;
    lw.lod_count = 1;
    lw.material_count = 2;
    lw.materials = mats;
    lw.lods = &lod;

    ThreediModelIR ir;
    threedi_ir_init(&ir);
    CHECK(threedi_ir_from_lw(&lw, &ir) == 0, "synthetic selector: conversion");
    CHECK(ir.lods[0].primitive_count == 1, "synthetic selector: one primitive");
    CHECK(ir.lods[0].primitives[0].material_index == 1, "synthetic selector: selector picks material");
    threedi_ir_free(&ir);
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
        CHECK(ir.lods[0].vertex_count == 125 * 3, "50CAL: LOD0 skips DONTDRAW faces");
        CHECK(ir.lods[0].index_count == 125 * 3, "50CAL: LOD0 index_count skips DONTDRAW faces");
        CHECK(ir.lods[0].part_count == 3, "50CAL: LOD0 part_count == 3 (3 subobjects)");
        CHECK(ir.mesh_type == THREEDI_IR_MESH_SKINNED, "50CAL: mesh_type SKINNED (multi-part)");
        // parent hierarchy carried through: part 2's parent is part 1
        CHECK(ir.lods[0].parts[2].parent_index == 1, "50CAL: part[2].parent_index == 1");
        // Flag-0 LW LODs keep geometry in model space. The loader does not run
        // the local-space pivot fixup, so the preview must not transform parts
        // again from the raw subobject pivot fields.
        CHECK(nearf(ir.lods[0].parts[1].abs_position[0], 0.0f), "50CAL: flag0 part[1] transform is identity");
        CHECK(nearf(ir.lods[0].parts[1].abs_position[1], 0.0f), "50CAL: flag0 part[1] transform y is identity");
        CHECK(nearf(ir.lods[0].parts[1].abs_position[2], 0.0f), "50CAL: flag0 part[1] transform z is identity");
        CHECK(nearf(ir.lods[0].parts[2].abs_position[0], 0.0f), "50CAL: flag0 part[2] transform is identity");
        CHECK(nearf(ir.lods[0].parts[2].abs_position[2], 0.0f), "50CAL: flag0 part[2] transform z is identity");
        {
            const ThreediIRLod *lod = &ir.lods[0];
            const ThreediIRPrimitive *part1 = &lod->primitives[lod->parts[1].primitive_start];
            const ThreediIRVertex *v = &lod->vertices[part1->vertex_offset];
            CHECK(part1->part_index == 1, "50CAL: part1 primitive is attached to part 1");
            CHECK(nearf(v->position[0], -2.0f / 256.0f), "50CAL: part1 first vertex raw y maps to IR x");
            CHECK(nearf(v->position[1], 217.0f / 256.0f), "50CAL: part1 first vertex raw z maps to IR y");
            CHECK(nearf(v->position[2], 132.0f / 256.0f), "50CAL: part1 first vertex raw x maps to IR z");
            CHECK(nearf(v->normal[0], -0.9551606f), "50CAL: part1 first normal raw y maps to IR x");
            CHECK(nearf(v->normal[1], 0.2960882f), "50CAL: part1 first normal raw z maps to IR y");
            CHECK(nearf(v->normal[2], 0.0f), "50CAL: part1 first normal raw x uses subobject normal base");
        }
        check_ir_integrity(&ir, "50CAL: IR integrity");
        threedi_ir_free(&ir);
    }
    {
        ThreediModelIR ir;
        load_ir(root, "BADGUY.3DI", &ir);
        CHECK(ir.lod_count == 4, "BADGUY: lod_count == 4");
        CHECK(ir.material_count == 7, "BADGUY: material_count == 7");
        CHECK(ir.lods[0].part_count == 15, "BADGUY: LOD0 part_count == 15");
        CHECK(ir.lods[0].primitives[0].bone_table_length > 1, "BADGUY: mixed vertex bones use a bone table");
        {
            const ThreediIRLod *lod = &ir.lods[0];
            const ThreediIRPrimitive *prim = &lod->primitives[0];
            int saw_nonzero_bone = 0;
            for (uint32_t i = 0; i < prim->vertex_count; ++i) {
                if (lod->vertices[prim->vertex_offset + i].bone_indices[0] != 0) {
                    saw_nonzero_bone = 1;
                    break;
                }
            }
            CHECK(saw_nonzero_bone, "BADGUY: mixed vertices carry local bone indices");
        }
        check_ir_integrity(&ir, "BADGUY: IR integrity");
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

    test_lod_flag_one_emits_skinning_in_model_space();
    test_lod_flag_zero_keeps_model_space_vertices();
    test_faces_emit_godot_clockwise_winding();
    test_surface_names_resolve_lw_materials();
    test_surface_selector_resolves_lw_materials();

    if (failures) { printf("lw_ir test: %d failures\n", failures); return 1; }
    printf("lw_ir test: all checks passed\n");
    return 0;
}
