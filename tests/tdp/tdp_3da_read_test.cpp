// Test for tdp_read_3da: write a known TDP fixture to .3da, read it back,
// write it again, and verify the two .3da files match byte-for-byte (modulo
// the timestamp comment on line 1).
//
// This is a write-read-write round-trip rather than a string-to-material
// equality test because:
//   - 3DA's text format isn't a perfect bijection with the 3DI3 model (some
//     fields are inferred by the writer from classification),
//   - the canonical encoder is df4oed::sub_421120, which we mirror
//     byte-for-byte; verifying byte equality through round-trip catches
//     any reader/writer asymmetry.

#include "tdp/tdp.h"
#include "tdp/tdp_material.h"
#include "threedi/threedi_material_class.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

static void copy_str(char *dst, size_t n, const char *src) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static std::string read_file(const char *path) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return "";
    std::string result;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), fp))
        result.append(buf, n);
    std::fclose(fp);
    return result;
}

// Drop the first line (the timestamp comment) so we can compare bodies.
static std::string drop_first_line(const std::string &s) {
    size_t pos = s.find('\n');
    if (pos == std::string::npos) return s;
    return s.substr(pos + 1);
}

static void set_diffuse_texture(TdpMaterial *m, int slot_idx,
                                const char *path) {
    uint32_t i = m->texture_count++;
    copy_str(m->textures[i].name, sizeof(m->textures[i].name), path);
    m->textures[i].slot = (slot_idx == 0) ? TDP_TEX_SLOT_DIFFUSE
                                          : TDP_TEX_SLOT_DETAIL;
    m->textures[i].flags = 0;
    m->textures[i].frame = 0;
}

int main() {
    // -----------------------------------------------------------------------
    // Build a fixture project covering the major 3DA-relevant fields.
    // -----------------------------------------------------------------------
    TdpProject src;
    tdp_init(&src);
    src.version = 3;
    src.poly_collision_lod = 2;

    src.material_count = 3;
    src.materials = static_cast<TdpMaterial *>(
        std::calloc(3, sizeof(TdpMaterial)));

    // Material 0: alpha-tested, single texture, classification flags + rgbgen
    {
        TdpMaterial *m = &src.materials[0];
        copy_str(m->name, sizeof(m->name), "Material_0_FF_ST_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_ST_OP");
        m->surface_type = 0x12;
        m->alpha_test_value_byte = 128;
        m->alpha_threshold = 128.0f / 255.0f;
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.bump_mode = THREEDI_BUMP_NONE;
        m->classification.specular_mode = THREEDI_SPEC_NONE;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->classification.alpha_test = 1;
        m->classification.two_sided = 1;
        m->animation.num_frames = 1;
        set_diffuse_texture(m, 0, "body.pic");
        m->reflect_color[0] = 50.0f / 255.0f;   // B
        m->reflect_color[1] = 100.0f / 255.0f;  // G
        m->reflect_color[2] = 200.0f / 255.0f;  // R
        m->rgb_gen.style = 2;
        m->rgb_gen.rate = 1.5f;
        m->rgb_gen.start_color[0] = 255.0f / 255.0f;
        m->rgb_gen.start_color[1] = 128.0f / 255.0f;
        m->rgb_gen.start_color[2] = 64.0f / 255.0f;
        m->rgb_gen.end_color[0] = 10.0f / 255.0f;
        m->rgb_gen.end_color[1] = 20.0f / 255.0f;
        m->rgb_gen.end_color[2] = 30.0f / 255.0f;
        m->u_params.style = 3;
        m->u_params.gen_rate = 2.0f;
        m->alpha_gen.style = 1;
        m->alpha_gen.rate = 0.5f;
        m->color_type = 2;
        m->luminosity = 7;
        m->transparency = 11;
        m->specular_intensity = 13;
        m->specular_sharpness = 17;
        m->color_green[0] = 1;
        m->color_green[1] = 2;
        m->color_green[2] = 3;
        m->color_alpha[0] = 4;
        m->color_alpha[1] = 5;
        m->color_alpha[2] = 6;
        m->reflect_type = 1;
        m->reflect_alpha = 2;
        m->actionplane_type = 3;
        m->projector_type = 0;
        m->u_offset = 0.5f;
        m->v_offset = 0.25f;
        m->u_tiling = 2.0f;
        m->v_tiling = 1.5f;
    }

    // Material 1: multi-texture (DIFFUSE + DETAIL) — exercises the split/fold
    {
        TdpMaterial *m = &src.materials[1];
        copy_str(m->name, sizeof(m->name), "Material_1_FF_DT_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_MT_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.has_detail = 1;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;
        m->color_type = 2;
        set_diffuse_texture(m, 0, "hull.pic");
        set_diffuse_texture(m, 1, "hull_d.pic");
    }

    // Material 2: blank
    {
        TdpMaterial *m = &src.materials[2];
        copy_str(m->name, sizeof(m->name), "Material_2_FF_NONE");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_ST_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;
        m->color_type = 2;
    }

    // LOD 0
    TdpLod *lod = &src.lods[0];
    copy_str(lod->scene_file, sizeof(lod->scene_file), "test.ase");
    lod->attributes = 5;
    copy_str(lod->render_function, sizeof(lod->render_function), "gnrc");
    lod->threshold = 0.0f;
    lod->part_anim_enabled = 1;

    lod->part_anim_count = 2;
    lod->part_anims = static_cast<TdpPartAnim *>(
        std::calloc(2, sizeof(TdpPartAnim)));
    lod->part_anims[0].transform_as = 0;
    lod->part_anims[1].transform_as = 1;
    lod->part_anims[1].rotate_type = 2;
    lod->part_anims[1].yaw.func_id = 1;
    lod->part_anims[1].yaw.param0 = 0.5f;
    lod->part_anims[1].yaw.param2 = 10.0f;
    lod->part_anims[1].yaw.param3 = 350.0f;

    lod->light_count = 1;
    lod->lights = static_cast<TdpLight *>(std::calloc(1, sizeof(TdpLight)));
    copy_str(lod->lights[0].name, sizeof(lod->lights[0].name), "LP01");
    lod->lights[0].colorgen_style = 1;
    lod->lights[0].colorgen_rate = 0.5f;
    lod->lights[0].colorgen_phase = 0.25f;
    lod->lights[0].colorgen_start[0] = 255;
    lod->lights[0].colorgen_start[1] = 200;
    lod->lights[0].colorgen_start[2] = 100;
    lod->lights[0].colorgen_end[0] = 50;
    lod->lights[0].colorgen_end[1] = 60;
    lod->lights[0].colorgen_end[2] = 70;

    // -----------------------------------------------------------------------
    // First write: produce a .3da from the source TDP fixture.
    // -----------------------------------------------------------------------
    char path1[4096];
    std::snprintf(path1, sizeof(path1), "%s%ctdp_3da_read_test_a.3da",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    int rc = tdp_write_3da(path1, &src);
    TEST_EXPECT(rc == 0 && "tdp_write_3da [first write] failed");

    // -----------------------------------------------------------------------
    // Read back via tdp_read_3da into a fresh project.
    // -----------------------------------------------------------------------
    TdpProject mid;
    rc = tdp_read_3da(path1, &mid);
    TEST_EXPECT(rc == 0 && "tdp_read_3da failed");

    // Sanity: material count should match the source (multitex split was
    // folded back into the original material slot).
    TEST_EXPECT(mid.material_count == src.material_count);

    // The multitex-split material should have a DETAIL slot folded back
    // in.  The DIFFUSE name is sourced from `green_texture` ("hull.pic").
    {
        const TdpMaterial *m = &mid.materials[1];
        const TdpMaterialTexture *d0 = nullptr;
        const TdpMaterialTexture *d1 = nullptr;
        for (uint32_t i = 0; i < m->texture_count; ++i) {
            if (m->textures[i].slot == TDP_TEX_SLOT_DIFFUSE)
                d0 = &m->textures[i];
            if (m->textures[i].slot == TDP_TEX_SLOT_DETAIL)
                d1 = &m->textures[i];
        }
        TEST_EXPECT(d0 != nullptr);
        TEST_EXPECT(d1 != nullptr);
        TEST_EXPECT(std::strcmp(d0->name, "hull.pic") == 0);
        TEST_EXPECT(std::strcmp(d1->name, "hull_d.pic") == 0);
    }

    // Sanity: each material's display name (description) preserved.
    for (size_t i = 0; i < src.material_count; ++i) {
        TEST_EXPECT(std::strcmp(src.materials[i].name,
                                 mid.materials[i].name) == 0);
    }

    // Classification fields that survive the BHD shader_type lookup are
    // preserved.  has_detail / has_overlay / is_skinned / is_glass / is_flag
    // / is_mirror_only / luminance / uv_animated do NOT survive a 3DA
    // round-trip when the 3DI3 model's classification doesn't pick a row in
    // kBhdShaderTable that carries them — e.g. (family=FF, has_detail=1)
    // has no matching row, so the writer collapses it to shader_type 0
    // (BHD_SHADER_NONE) and `has_detail` is dropped from the classification.
    // The split-emit is still triggered by `has_detail_tex` checking the
    // model's textures[] directly, so byte-equality across two writes still
    // holds — we just check the survivable fields here.
    for (size_t i = 0; i < src.material_count; ++i) {
        const ThreediMaterialClass *a = &src.materials[i].classification;
        const ThreediMaterialClass *b = &mid.materials[i].classification;
        // render_attributes-derived bits round-trip cleanly:
        TEST_EXPECT(a->blend_mode == b->blend_mode);
        TEST_EXPECT(a->two_sided  == b->two_sided);
        TEST_EXPECT(a->alpha_test == b->alpha_test);
    }

    // -----------------------------------------------------------------------
    // Second write: produce a fresh .3da from the round-tripped project.
    // -----------------------------------------------------------------------
    char path2[4096];
    std::snprintf(path2, sizeof(path2), "%s%ctdp_3da_read_test_b.3da",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    rc = tdp_write_3da(path2, &mid);
    TEST_EXPECT(rc == 0 && "tdp_write_3da [second write] failed");

    // -----------------------------------------------------------------------
    // Compare bodies (skip line 1's timestamp).
    // -----------------------------------------------------------------------
    std::string body_a = drop_first_line(read_file(path1));
    std::string body_b = drop_first_line(read_file(path2));
    TEST_EXPECT(!body_a.empty() && "first .3da is empty");
    TEST_EXPECT(!body_b.empty() && "second .3da is empty");

    if (body_a != body_b) {
        // Surface a small diff hint to make failures debuggable.
        size_t mn = body_a.size() < body_b.size() ? body_a.size() : body_b.size();
        size_t first_diff = 0;
        while (first_diff < mn && body_a[first_diff] == body_b[first_diff])
            ++first_diff;
        std::fprintf(stderr,
                     "byte-mismatch at offset %zu (a.size=%zu, b.size=%zu)\n",
                     first_diff, body_a.size(), body_b.size());
        size_t lo = first_diff > 200 ? first_diff - 200 : 0;
        size_t hi_a = first_diff + 200 < body_a.size() ? first_diff + 200 : body_a.size();
        size_t hi_b = first_diff + 200 < body_b.size() ? first_diff + 200 : body_b.size();
        std::fprintf(stderr, "--- A (%zu..%zu) ---\n%.*s\n",
                     lo, hi_a, (int)(hi_a - lo), body_a.c_str() + lo);
        std::fprintf(stderr, "--- B (%zu..%zu) ---\n%.*s\n",
                     lo, hi_b, (int)(hi_b - lo), body_b.c_str() + lo);
    }
    TEST_EXPECT(body_a == body_b);

    tdp_free(&src);
    tdp_free(&mid);
    std::remove(path1);
    std::remove(path2);

    std::printf("TDP 3DA read round-trip test PASSED\n");
    return 0;
}
