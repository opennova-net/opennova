#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "threedi/threedi_3di3.h"
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

int main(void) {
    test_3di3_material_ir_preserves_full_texture_table_and_fields();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
