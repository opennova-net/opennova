#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "threedi/threedi_3di3.h"
#include "threedi/threedi_gp.h"
#include "threedi/threedi_ir.h"

static int failures = 0;

static void expect_true(const char *label, int condition) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static int float_eq(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

static void copy_name(char *dst, size_t dst_size, const char *src) {
    std::memset(dst, 0, dst_size);
    std::strncpy(dst, src, dst_size - 1);
}

static void test_3di3_material_ir_preserves_full_texture_table_and_fields() {
    Threedi3di3 model;
    ThreediMaterial mat;
    ThreediModelIR ir;
    std::memset(&model, 0, sizeof(model));
    std::memset(&mat, 0, sizeof(mat));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(model.header.name, sizeof(model.header.name), "MaterialIR");
    model.header.mesh_type = THREEDI_MESH_BASIC;
    model.material_count = 1;
    model.materials = &mat;

    mat.index = 3;
    copy_name(mat.shader_name, sizeof(mat.shader_name), "FF_MT_AB");
    mat.texture_count = THREEDI_IR_MAX_MATERIAL_TEXTURES;
    for (uint32_t i = 0; i < mat.texture_count; ++i) {
        char name[17];
        std::snprintf(name, sizeof(name), "tex%02u.tga", i);
        copy_name(mat.textures[i].name, sizeof(mat.textures[i].name), name);
        mat.textures[i].slot = static_cast<uint8_t>((i % 4u) + 1u);
        mat.textures[i].type = mat.textures[i].slot >= 3 ? THREEDI_TEX_TYPE_NORMAL_TGA : THREEDI_TEX_TYPE_DIFFUSE;
        mat.textures[i].flags = static_cast<uint8_t>(i & 0x03u);
        mat.textures[i].frame = static_cast<uint8_t>(i);
    }

    mat.material_flags = THREEDI_MATERIAL_FLAG_ALPHA_TEST |
                         THREEDI_MATERIAL_FLAG_ALPHA_INVERT |
                         THREEDI_MATERIAL_FLAG_TWO_SIDED;
    mat.alpha_test_value_byte = 96;
    mat.pad[0] = 7;
    mat.pad[1] = 8;
    mat.rgb_gen2.style = 115;
    mat.rgb_gen2.reg = 4;
    mat.rgb_gen2.rate = 1.25f;
    mat.rgb_gen2.start_color[0] = 0.1f;
    mat.rgb_gen2.end_color[3] = 0.9f;
    mat.reflect_color[0] = 0.2f;
    mat.reflect_color2[2] = 0.75f;
    mat.emissive_type2 = THREEDI_EMISSIVE_FULL;
    mat.is_glass = 1;
    mat.glass_type2 = 5;

    expect_true("conversion succeeds", threedi_ir_from_3di3(&model, &ir) == 0);
    expect_true("one material", ir.material_count == 1);

    const ThreediIRMaterial *out = &ir.materials[0];
    expect_true("texture count preserves 24 entries", out->texture_count == THREEDI_IR_MAX_MATERIAL_TEXTURES);
    expect_true("last texture name preserved", std::strcmp(out->textures[23].name, "tex23.tga") == 0);
    expect_true("last texture slot preserved", out->textures[23].slot == 4);
    expect_true("last texture type preserved", out->textures[23].type == THREEDI_TEX_TYPE_NORMAL_TGA);
    expect_true("last texture flags preserved", out->textures[23].flags == 3);
    expect_true("last texture frame preserved", out->textures[23].frame == 23);

    expect_true("raw material flags preserved", out->material_flags == mat.material_flags);
    expect_true("raw alpha byte preserved", out->alpha_test_value_byte == 96);
    expect_true("reserved byte 0 preserved", out->material_pad[0] == 7);
    expect_true("reserved byte 1 preserved", out->material_pad[1] == 8);
    expect_true("compat alpha threshold derived", float_eq(out->alpha_threshold, 96.0f / 255.0f));
    expect_true("compat flags include emissive from secondary channel",
                (out->flags & THREEDI_IR_MATERIAL_FLAG_EMISSIVE) != 0);
    expect_true("blend mode inferred from shader", out->blend_mode == THREEDI_IR_BLEND_ALPHA);
    expect_true("rgb gen 2 style copied", out->rgb_gen2.style == 115);
    expect_true("rgb gen 2 reg copied", out->rgb_gen2.reg == 4);
    expect_true("rgb gen 2 rate copied", float_eq(out->rgb_gen2.rate, 1.25f));
    expect_true("rgb gen 2 color copied", float_eq(out->rgb_gen2.start_color[0], 0.1f));
    expect_true("reflect color 2 copied", float_eq(out->reflect_color2[2], 0.75f));
    expect_true("emissive type 2 copied", out->emissive_type2 == THREEDI_EMISSIVE_FULL);
    expect_true("glass type 2 copied", out->glass_type2 == 5);

    threedi_ir_free(&ir);
}

static void test_gp_detail_material_preserves_phong_shader_and_detail_slot() {
    ThreediGpFile gp;
    ThreediGpRModel rmodel;
    ThreediGpMaterial mat;
    ThreediGpMaterialLookup lookups[2];
    ThreediModelIR ir;
    std::memset(&gp, 0, sizeof(gp));
    std::memset(&rmodel, 0, sizeof(rmodel));
    std::memset(&mat, 0, sizeof(mat));
    std::memset(lookups, 0, sizeof(lookups));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(gp.header.name, sizeof(gp.header.name), "GpDetail");
    gp.header.mesh_type = THREEDI_GP_MESH_BASIC;
    gp.rmodel_count = 1;
    gp.rmodels = &rmodel;
    gp.material_lookup_count = 2;
    gp.material_lookups = lookups;

    rmodel.material_count = 1;
    rmodel.materials = &mat;

    copy_name(mat.texture_name, sizeof(mat.texture_name), "Base.tga");
    mat.render_lookup = 0;
    mat.render_attributes = 0x2;
    mat.shader_flags = 0x200 | 2;

    copy_name(lookups[0].texture_name, sizeof(lookups[0].texture_name), "Base.tga");
    lookups[0].seq_index = 0;
    lookups[0].slot_type = 0x02;

    copy_name(lookups[1].texture_name, sizeof(lookups[1].texture_name), "Detail.tga");
    lookups[1].seq_index = 1;
    lookups[1].slot_type = 0x04;

    expect_true("GP conversion succeeds", threedi_ir_from_gp(&gp, &ir) == 0);
    expect_true("one GP material", ir.material_count == 1);

    const ThreediIRMaterial *out = &ir.materials[0];
    expect_true("GP shader type 2 stays phong", std::strcmp(out->shader_name, "VS_PHONGT") == 0);
    expect_true("GP detail texture count", out->texture_count == 2);
    expect_true("GP diffuse texture preserved", std::strcmp(out->textures[0].name, "Base.tga") == 0);
    expect_true("GP detail texture preserved", std::strcmp(out->textures[1].name, "Detail.tga") == 0);
    expect_true("GP detail texture slot", out->textures[1].slot == THREEDI_IR_TEX_SLOT_DETAIL);
    expect_true("GP material flags normalized",
                out->material_flags == (THREEDI_MATERIAL_FLAG_ALPHA_TEST | THREEDI_MATERIAL_FLAG_TWO_SIDED));
    expect_true("GP alpha test byte normalized", out->alpha_test_value_byte == 128);
    expect_true("GP detail+phong is not glass", out->is_glass == 0);

    threedi_ir_free(&ir);
}

static void test_3di3_glass_shader_preserves_raw_blend_and_glass_flag() {
    /* Glass tags do NOT mutate IR blend_mode or is_glass — DCC consumers
       (Python descriptor / Max / Blender) promote glass to alpha-blend via
       the shared shader-tag table. The IR keeps raw source semantics. */
    Threedi3di3 model;
    ThreediMaterial mat;
    ThreediModelIR ir;
    std::memset(&model, 0, sizeof(model));
    std::memset(&mat, 0, sizeof(mat));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(model.header.name, sizeof(model.header.name), "GlassMat");
    model.header.mesh_type = THREEDI_MESH_BASIC;
    model.material_count = 1;
    model.materials = &mat;

    mat.index = 0;
    copy_name(mat.shader_name, sizeof(mat.shader_name), "FFP_GLASS");
    mat.material_flags = 0;
    mat.is_glass = 1;

    expect_true("FFP_GLASS conversion succeeds",
                threedi_ir_from_3di3(&model, &ir) == 0);
    expect_true("FFP_GLASS one material", ir.material_count == 1);

    const ThreediIRMaterial *out = &ir.materials[0];
    expect_true("FFP_GLASS shader name copied",
                std::strcmp(out->shader_name, "FFP_GLASS") == 0);
    expect_true("FFP_GLASS is_glass preserved", out->is_glass == 1);
    expect_true("FFP_GLASS blend_mode stays raw OPAQUE",
                out->blend_mode == THREEDI_IR_BLEND_OPAQUE);

    threedi_ir_free(&ir);
}

static void test_3di3_ff_st_ab_preserves_raw_blend() {
    /* FF_ST_AB has the "_AB" suffix, so blend_mode_from_shader_name detects
       ALPHA without consulting the table. This locks that path. */
    Threedi3di3 model;
    ThreediMaterial mat;
    ThreediModelIR ir;
    std::memset(&model, 0, sizeof(model));
    std::memset(&mat, 0, sizeof(mat));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(model.header.name, sizeof(model.header.name), "AbMat");
    model.header.mesh_type = THREEDI_MESH_BASIC;
    model.material_count = 1;
    model.materials = &mat;

    mat.index = 0;
    copy_name(mat.shader_name, sizeof(mat.shader_name), "FF_ST_AB");
    mat.material_flags = 0;

    expect_true("FF_ST_AB conversion succeeds",
                threedi_ir_from_3di3(&model, &ir) == 0);

    const ThreediIRMaterial *out = &ir.materials[0];
    expect_true("FF_ST_AB blend_mode is ALPHA",
                out->blend_mode == THREEDI_IR_BLEND_ALPHA);
    expect_true("FF_ST_AB is_glass stays 0", out->is_glass == 0);

    threedi_ir_free(&ir);
}

static void test_gp_multi_lod_material_offsets() {
    /* Each GP rmodel has its own per-LOD materials array. The IR concatenates
       all rmodels' materials in rmodel order; per-LOD primitive material_index
       gets offset by the start of that LOD's segment so it indexes the right
       rmodel's contribution. Without this fix, LOD 1+ primitives bind to
       LOD 0's materials and produce wrong textures. */
    ThreediGpFile gp;
    ThreediGpRModel rmodels[2];
    ThreediGpMaterial mats0[2];
    ThreediGpMaterial mats1[1];
    ThreediGpVariablePoly poly0;
    ThreediGpVariablePoly poly1;
    ThreediGpSubObject sub0;
    ThreediGpSubObject sub1;
    uint16_t indices0[3] = {0, 1, 2};
    uint16_t indices1[3] = {0, 1, 2};
    ThreediGpRVert rverts[3];
    ThreediModelIR ir;

    std::memset(&gp, 0, sizeof(gp));
    std::memset(rmodels, 0, sizeof(rmodels));
    std::memset(mats0, 0, sizeof(mats0));
    std::memset(mats1, 0, sizeof(mats1));
    std::memset(&poly0, 0, sizeof(poly0));
    std::memset(&poly1, 0, sizeof(poly1));
    std::memset(&sub0, 0, sizeof(sub0));
    std::memset(&sub1, 0, sizeof(sub1));
    std::memset(rverts, 0, sizeof(rverts));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(gp.header.name, sizeof(gp.header.name), "MultiLodGp");
    gp.header.mesh_type = THREEDI_GP_MESH_BASIC;
    gp.rmodel_count = 2;
    gp.rmodels = rmodels;
    gp.rverts = rverts;
    gp.rvert_count = 3;

    /* LOD 0: 2 materials. */
    rmodels[0].material_count = 2;
    rmodels[0].materials = mats0;
    rmodels[0].subobject_count = 1;
    rmodels[0].subobjects = &sub0;
    rmodels[0].poly_count = 1;
    rmodels[0].polys = &poly0;
    copy_name(mats0[0].texture_name, sizeof(mats0[0].texture_name), "MAT_A0.tga");
    mats0[0].render_lookup = 0xFFFFFF;
    copy_name(mats0[1].texture_name, sizeof(mats0[1].texture_name), "MAT_A1.tga");
    mats0[1].render_lookup = 0xFFFFFF;
    poly0.material_index = 1;       /* LOD 0 primitive uses MAT_A1. */
    poly0.subobject_index = 0;
    poly0.first_vertex = 0;
    poly0.indices = indices0;
    poly0.index_count = 3;
    poly0.topology = 0;             /* triangle list */

    /* LOD 1: 1 material. */
    rmodels[1].material_count = 1;
    rmodels[1].materials = mats1;
    rmodels[1].subobject_count = 1;
    rmodels[1].subobjects = &sub1;
    rmodels[1].poly_count = 1;
    rmodels[1].polys = &poly1;
    copy_name(mats1[0].texture_name, sizeof(mats1[0].texture_name), "MAT_B0.tga");
    mats1[0].render_lookup = 0xFFFFFF;
    poly1.material_index = 0;       /* LOD 1 primitive uses MAT_B0 (rmodel-local idx 0). */
    poly1.subobject_index = 0;
    poly1.first_vertex = 0;
    poly1.indices = indices1;
    poly1.index_count = 3;
    poly1.topology = 0;

    expect_true("multi-LOD GP conversion succeeds",
                threedi_ir_from_gp(&gp, &ir) == 0);
    expect_true("multi-LOD material count is sum of rmodels",
                ir.material_count == 3);
    expect_true("LOD 0 material 0 -> MAT_A0",
                std::strcmp((const char *)ir.materials[0].textures[0].name, "MAT_A0.tga") == 0);
    expect_true("LOD 0 material 1 -> MAT_A1",
                std::strcmp((const char *)ir.materials[1].textures[0].name, "MAT_A1.tga") == 0);
    expect_true("LOD 1 material at offset -> MAT_B0",
                std::strcmp((const char *)ir.materials[2].textures[0].name, "MAT_B0.tga") == 0);
    /* Primitive material indices should reflect the per-LOD offset. */
    expect_true("LOD 0 prim 0 material_index unchanged (offset 0)",
                ir.lods[0].primitives[0].material_index == 1);
    expect_true("LOD 1 prim 0 material_index offset by LOD 0 size",
                ir.lods[1].primitives[0].material_index == 2);

    threedi_ir_free(&ir);
}

static void test_gp_multi_lod_dedupes_content_identical_materials() {
    /* When LODs share content-identical materials (typical for NovaLogic
       assets — every rmodel often defines the same material at the same
       slot for LOD continuity), the GP converter concatenates them into
       the IR materials array (one per rmodel) and then dedupes by content
       so .3dp/.3da and per-LOD .ase outputs reference a single canonical
       entry per unique material. */
    ThreediGpFile gp;
    ThreediGpRModel rmodels[2];
    ThreediGpMaterial mats0[2];
    ThreediGpMaterial mats1[2];
    ThreediGpVariablePoly poly0;
    ThreediGpVariablePoly poly1;
    ThreediGpSubObject sub0;
    ThreediGpSubObject sub1;
    uint16_t indices0[3] = {0, 1, 2};
    uint16_t indices1[3] = {0, 1, 2};
    ThreediGpRVert rverts[3];
    ThreediModelIR ir;

    std::memset(&gp, 0, sizeof(gp));
    std::memset(rmodels, 0, sizeof(rmodels));
    std::memset(mats0, 0, sizeof(mats0));
    std::memset(mats1, 0, sizeof(mats1));
    std::memset(&poly0, 0, sizeof(poly0));
    std::memset(&poly1, 0, sizeof(poly1));
    std::memset(&sub0, 0, sizeof(sub0));
    std::memset(&sub1, 0, sizeof(sub1));
    std::memset(rverts, 0, sizeof(rverts));
    std::memset(&ir, 0, sizeof(ir));

    copy_name(gp.header.name, sizeof(gp.header.name), "DedupGp");
    gp.header.mesh_type = THREEDI_GP_MESH_BASIC;
    gp.rmodel_count = 2;
    gp.rmodels = rmodels;
    gp.rverts = rverts;
    gp.rvert_count = 3;

    /* Both rmodels define the SAME 2 materials at slots [0, 1]. */
    rmodels[0].material_count = 2;
    rmodels[0].materials = mats0;
    rmodels[0].subobject_count = 1;
    rmodels[0].subobjects = &sub0;
    rmodels[0].poly_count = 1;
    rmodels[0].polys = &poly0;
    copy_name(mats0[0].texture_name, sizeof(mats0[0].texture_name), "MAT_X.tga");
    mats0[0].render_lookup = 0xFFFFFF;
    copy_name(mats0[1].texture_name, sizeof(mats0[1].texture_name), "MAT_Y.tga");
    mats0[1].render_lookup = 0xFFFFFF;
    poly0.material_index = 0;       /* LOD 0 prim uses MAT_X. */
    poly0.subobject_index = 0;
    poly0.first_vertex = 0;
    poly0.indices = indices0;
    poly0.index_count = 3;
    poly0.topology = 0;

    rmodels[1].material_count = 2;
    rmodels[1].materials = mats1;
    rmodels[1].subobject_count = 1;
    rmodels[1].subobjects = &sub1;
    rmodels[1].poly_count = 1;
    rmodels[1].polys = &poly1;
    copy_name(mats1[0].texture_name, sizeof(mats1[0].texture_name), "MAT_X.tga");
    mats1[0].render_lookup = 0xFFFFFF;
    copy_name(mats1[1].texture_name, sizeof(mats1[1].texture_name), "MAT_Y.tga");
    mats1[1].render_lookup = 0xFFFFFF;
    poly1.material_index = 1;       /* LOD 1 prim uses MAT_Y (rmodel-local). */
    poly1.subobject_index = 0;
    poly1.first_vertex = 0;
    poly1.indices = indices1;
    poly1.index_count = 3;
    poly1.topology = 0;

    expect_true("dedup conversion succeeds",
                threedi_ir_from_gp(&gp, &ir) == 0);
    /* Pre-dedup the IR would have 4 materials (2 per rmodel). After dedup,
       it's 2 — MAT_X and MAT_Y. */
    expect_true("dedup collapses identical materials to 2",
                ir.material_count == 2);
    expect_true("canonical 0 is MAT_X",
                std::strcmp((const char *)ir.materials[0].textures[0].name, "MAT_X.tga") == 0);
    expect_true("canonical 1 is MAT_Y",
                std::strcmp((const char *)ir.materials[1].textures[0].name, "MAT_Y.tga") == 0);
    /* LOD 0 prim referenced MAT_X (offset 0 + local 0 = global 0) → canonical 0. */
    expect_true("LOD 0 prim 0 remapped to canonical MAT_X",
                ir.lods[0].primitives[0].material_index == 0);
    /* LOD 1 prim referenced MAT_Y (offset 2 + local 1 = global 3) → canonical 1. */
    expect_true("LOD 1 prim 0 remapped to canonical MAT_Y",
                ir.lods[1].primitives[0].material_index == 1);

    threedi_ir_free(&ir);
}

int main(void) {
    test_3di3_material_ir_preserves_full_texture_table_and_fields();
    test_gp_detail_material_preserves_phong_shader_and_detail_slot();
    test_3di3_glass_shader_preserves_raw_blend_and_glass_flag();
    test_3di3_ff_st_ab_preserves_raw_blend();
    test_gp_multi_lod_material_offsets();
    test_gp_multi_lod_dedupes_content_identical_materials();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
