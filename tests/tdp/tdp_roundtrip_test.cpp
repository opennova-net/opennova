// TDP roundtrip test: build a TdpProject (materials), write to file,
// parse back, verify.

#include "tdp/tdp.h"
#include "tdp/tdp_material.h"
#include "threedi/threedi_material_class.h"
#include "threedi/threedi_panm.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_expect.h"
#include "common/test_paths.h"

static void copy_str(char *dst, size_t n, const char *src) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static bool float_eq(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) < eps;
}

// Find static texture by slot in TDP material.
static const TdpMaterialTexture *
find_tex(const TdpMaterial *m, uint8_t slot) {
    for (uint32_t i = 0; i < m->texture_count && i < TDP_MAX_MATERIAL_TEXTURES; ++i) {
        const TdpMaterialTexture *t = &m->textures[i];
        if (t->slot == slot && !(t->flags & 0x01u)) return t;
    }
    return nullptr;
}

// Helper: append a static texture entry
static void append_tex(TdpMaterial *m, uint8_t slot, const char *path,
                       int clamped) {
    if (m->texture_count >= TDP_MAX_MATERIAL_TEXTURES) return;
    TdpMaterialTexture *t = &m->textures[m->texture_count++];
    std::memset(t, 0, sizeof(*t));
    copy_str(t->name, sizeof(t->name), path);
    t->slot = slot;
    t->flags = clamped ? 0x02u : 0x00u;
}

int main() {
    // Build a project in memory
    TdpProject src;
    tdp_init(&src);
    src.version = 1;
    src.poly_collision_lod = 0;

    src.material_count = 2;
    src.materials = static_cast<TdpMaterial *>(
        std::calloc(2, sizeof(TdpMaterial)));

    // Material 0: basic
    {
        TdpMaterial *m = &src.materials[0];
        copy_str(m->name, sizeof(m->name), "Material_0_FF_ST_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_ST_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;  // Dirt → ptype 9
        m->alpha_test_value_byte = 128;
        m->alpha_threshold = 128.0f / 255.0f;
        append_tex(m, TDP_TEX_SLOT_DIFFUSE, "body.pic", 0);
        append_tex(m, TDP_TEX_SLOT_DETAIL,  "detail.pic", 1);
        append_tex(m, TDP_TEX_SLOT_NORMAL,  "body_n.pic", 0);
        // reflect_color BGRA float (200,100,50 R,G,B)
        m->reflect_color[0] = 50.0f / 255.0f;
        m->reflect_color[1] = 100.0f / 255.0f;
        m->reflect_color[2] = 200.0f / 255.0f;
        m->reflect_color[3] = 0.0f;
        m->rgb_gen.style = 2;
        m->rgb_gen.rate = 1.5f;
        m->rgb_gen.phase = 0.25f;
        m->rgb_gen.start_color[0] = 1.0f;  // 255
        m->rgb_gen.end_color[2] = 128.0f / 255.0f;
        m->alpha_gen.style = 1;
        m->alpha_gen.rate = 0.5f;
        m->alpha_gen.phase = 0.1f;
        m->alpha_gen.start = 5;   // int16 (no fractional roundtrip)
        m->alpha_gen.end = 10;
        m->u_params.style = 3;
        m->u_params.gen_rate = 2.0f;
        m->v_params.style = 1;
        m->v_params.end = 0.5f;
    }

    // Material 1: minimal with empty normaltex (fresh export default)
    {
        TdpMaterial *m = &src.materials[1];
        copy_str(m->name, sizeof(m->name), "Material_1_FF_DT_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_MT_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.has_detail = 1;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;
    }

    // LOD 0: 3 part anims, 1 light
    TdpLod *lod = &src.lods[0];
    copy_str(lod->scene_file, sizeof(lod->scene_file), "test_model.ase");
    lod->attributes = 5;
    copy_str(lod->render_function, sizeof(lod->render_function), "gnrc");
    lod->threshold = 0.0f;
    lod->part_anim_enabled = 1;

    lod->part_anim_count = 3;
    lod->part_anims = static_cast<TdpPartAnim *>(std::calloc(3, sizeof(TdpPartAnim)));
    for (int i = 0; i < 3; ++i) lod->part_anims[i].transform_as = i;
    {
        TdpPartAnim *pa = &lod->part_anims[1];
        pa->rotate_type = 2;
        pa->yaw.func_id = 1;
        pa->yaw.param0 = 0.5f;
        pa->yaw.param1 = 0.0f;
        pa->yaw.param2 = 10.0f;
        pa->yaw.param3 = 350.0f;
    }

    lod->light_count = 1;
    lod->lights = static_cast<TdpLight *>(std::calloc(1, sizeof(TdpLight)));
    copy_str(lod->lights[0].name, sizeof(lod->lights[0].name), "LP00");
    lod->lights[0].colorgen_style = 1;
    lod->lights[0].colorgen_start[0] = 255;
    lod->lights[0].colorgen_start[1] = 200;
    lod->lights[0].colorgen_start[2] = 100;
    lod->lights[0].colorgen_end[0] = 50;

    // Write to temp file
    char tmp_path[4096];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s%ctdp_roundtrip_test.3dp",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    int rc = tdp_write(tmp_path, &src);
    TEST_EXPECT(rc == 0 && "tdp_write failed");

    // Parse back
    TdpProject dst;
    rc = tdp_parse(tmp_path, &dst);
    TEST_EXPECT(rc == 0 && "tdp_parse failed");

    // Verify header
    TEST_EXPECT(dst.version == src.version);
    TEST_EXPECT(dst.poly_collision_lod == src.poly_collision_lod);

    // Verify materials
    TEST_EXPECT(dst.material_count == src.material_count);
    for (size_t i = 0; i < src.material_count; ++i) {
        const TdpMaterial *s = &src.materials[i];
        const TdpMaterial *d = &dst.materials[i];
        TEST_EXPECT(std::strcmp(s->name, d->name) == 0);
        TEST_EXPECT(std::strcmp(s->shader_name, d->shader_name) == 0);
        TEST_EXPECT(s->surface_type == d->surface_type);
        TEST_EXPECT(s->alpha_test_value_byte == d->alpha_test_value_byte);

        // Textures
        const TdpMaterialTexture *sd0 = find_tex(s, TDP_TEX_SLOT_DIFFUSE);
        const TdpMaterialTexture *dd0 = find_tex(d, TDP_TEX_SLOT_DIFFUSE);
        if (sd0) {
            TEST_EXPECT(dd0 != nullptr);
            TEST_EXPECT(std::strcmp(sd0->name, dd0->name) == 0);
        }
        const TdpMaterialTexture *sd1 = find_tex(s, TDP_TEX_SLOT_DETAIL);
        const TdpMaterialTexture *dd1 = find_tex(d, TDP_TEX_SLOT_DETAIL);
        if (sd1) {
            TEST_EXPECT(dd1 != nullptr);
            TEST_EXPECT(std::strcmp(sd1->name, dd1->name) == 0);
            // Clamped flag (bit 1) preserved
            TEST_EXPECT((sd1->flags & 0x02u) == (dd1->flags & 0x02u));
        }
        const TdpMaterialTexture *sn0 = find_tex(s, TDP_TEX_SLOT_NORMAL);
        const TdpMaterialTexture *dn0 = find_tex(d, TDP_TEX_SLOT_NORMAL);
        if (sn0) {
            TEST_EXPECT(dn0 != nullptr);
            TEST_EXPECT(std::strcmp(sn0->name, dn0->name) == 0);
        }

        // reflect_color (BGRA, byte-quantized roundtrip)
        TEST_EXPECT(float_eq(s->reflect_color[0], d->reflect_color[0], 0.005f));
        TEST_EXPECT(float_eq(s->reflect_color[1], d->reflect_color[1], 0.005f));
        TEST_EXPECT(float_eq(s->reflect_color[2], d->reflect_color[2], 0.005f));

        // RGB gen
        TEST_EXPECT(s->rgb_gen.style == d->rgb_gen.style);
        TEST_EXPECT(float_eq(s->rgb_gen.rate, d->rgb_gen.rate));
        TEST_EXPECT(float_eq(s->rgb_gen.phase, d->rgb_gen.phase));
        TEST_EXPECT(float_eq(s->rgb_gen.start_color[0], d->rgb_gen.start_color[0], 0.005f));
        TEST_EXPECT(float_eq(s->rgb_gen.end_color[2], d->rgb_gen.end_color[2], 0.005f));

        TEST_EXPECT(s->alpha_gen.style == d->alpha_gen.style);
        TEST_EXPECT(float_eq(s->alpha_gen.rate, d->alpha_gen.rate));
        TEST_EXPECT(s->alpha_gen.start == d->alpha_gen.start);
        TEST_EXPECT(s->alpha_gen.end == d->alpha_gen.end);

        TEST_EXPECT(s->u_params.style == d->u_params.style);
        TEST_EXPECT(float_eq(s->u_params.gen_rate, d->u_params.gen_rate));
        TEST_EXPECT(s->v_params.style == d->v_params.style);
        TEST_EXPECT(float_eq(s->v_params.end, d->v_params.end));
    }

    // Verify LOD 0
    const TdpLod *sl = &src.lods[0];
    const TdpLod *dl = &dst.lods[0];
    TEST_EXPECT(std::strcmp(sl->scene_file, dl->scene_file) == 0);
    TEST_EXPECT(sl->attributes == dl->attributes);
    TEST_EXPECT(std::strcmp(sl->render_function, dl->render_function) == 0);
    TEST_EXPECT(sl->part_anim_enabled == dl->part_anim_enabled);
    TEST_EXPECT(sl->part_anim_count == dl->part_anim_count);

    for (size_t p = 0; p < sl->part_anim_count; ++p) {
        const TdpPartAnim *sp = &sl->part_anims[p];
        const TdpPartAnim *dp = &dl->part_anims[p];
        TEST_EXPECT(sp->transform_as == dp->transform_as);
        TEST_EXPECT(sp->rotate_type == dp->rotate_type);
        TEST_EXPECT(sp->yaw.func_id == dp->yaw.func_id);
        TEST_EXPECT(float_eq(sp->yaw.param0, dp->yaw.param0));
        TEST_EXPECT(float_eq(sp->yaw.param2, dp->yaw.param2));
        TEST_EXPECT(float_eq(sp->yaw.param3, dp->yaw.param3));
    }

    TEST_EXPECT(sl->light_count == dl->light_count);
    TEST_EXPECT(std::strcmp(sl->lights[0].name, dl->lights[0].name) == 0);
    TEST_EXPECT(sl->lights[0].colorgen_style == dl->lights[0].colorgen_style);
    TEST_EXPECT(sl->lights[0].colorgen_start[0] == dl->lights[0].colorgen_start[0]);
    TEST_EXPECT(sl->lights[0].colorgen_start[1] == dl->lights[0].colorgen_start[1]);
    TEST_EXPECT(sl->lights[0].colorgen_end[0] == dl->lights[0].colorgen_end[0]);

    tdp_free(&src);
    tdp_free(&dst);
    std::remove(tmp_path);

    std::printf("TDP roundtrip test PASSED\n");


    return 0;
}
