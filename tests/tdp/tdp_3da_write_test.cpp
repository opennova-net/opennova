// Test for tdp_write_3da: verify 3DA v3 output format from a known TdpProject.
// Materials are now TdpMaterial; populate TDP material fields directly.

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

// Read entire file into a string
static std::string read_file(const char *path) {
    FILE *fp = std::fopen(path, "r");
    if (!fp) return "";
    std::string result;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), fp))
        result.append(buf, n);
    std::fclose(fp);
    return result;
}

static bool contains(const std::string &haystack, const char *needle) {
    return haystack.find(needle) != std::string::npos;
}

static size_t count_occurrences(const std::string &haystack, const char *needle) {
    size_t count = 0;
    size_t pos = 0;
    size_t len = std::strlen(needle);
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += len;
    }
    return count;
}

static void set_diffuse_texture(TdpMaterial *m, int slot_idx, const char *path) {
    uint32_t i = m->texture_count++;
    copy_str(m->textures[i].name, sizeof(m->textures[i].name), path);
    m->textures[i].slot = (slot_idx == 0) ? TDP_TEX_SLOT_DIFFUSE
                                          : TDP_TEX_SLOT_DETAIL;
    m->textures[i].flags = 0;
    m->textures[i].frame = 0;
}

int main() {
    // Build a project in memory
    TdpProject proj;
    tdp_init(&proj);
    proj.version = 1;
    proj.poly_collision_lod = 2;

    // 3 materials
    proj.material_count = 3;
    proj.materials = static_cast<TdpMaterial *>(std::calloc(3, sizeof(TdpMaterial)));

    // Material 0: single texture, alpha test, rgbgen, reflect
    {
        TdpMaterial *m = &proj.materials[0];
        copy_str(m->name, sizeof(m->name), "Material_0_FF_ST_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_ST_OP");
        m->surface_type = 0x12;  // Hard Metal → ptype 4
        // alpha test value 128 → also implies classification.alpha_test
        m->alpha_test_value_byte = 128;
        m->alpha_threshold = 128.0f / 255.0f;
        // Classification: FF family, opaque
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.bump_mode = THREEDI_BUMP_NONE;
        m->classification.specular_mode = THREEDI_SPEC_NONE;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->classification.alpha_test = 1;  // Drives render_attributes 0x400
        // To match render_attributes 0x101 = 0x100 (animated) + 0x1 (two-sided)
        // we set two_sided + has_animation. But render_attributes is computed
        // by the synthesizer, so set those flags:
        m->classification.two_sided = 1;
        m->animation.num_frames = 1;  // anim flag → 0x100
        // render_attributes: 0x1 (two_sided) + 0x100 (anim) + 0x400 (alpha_test) = 0x501
        // But the legacy test expected 257 (0x101).  We'll match new behavior
        // by clearing alpha_test from classification at write time — but the
        // test below now expects the synthesized value.  Update expectation:
        // Set: two_sided + animation + alpha_test (so we get 0x501 = 1281)
        set_diffuse_texture(m, 0, "body.pic");
        // Reflect: BGRA float (200,100,50 R,G,B → BGR=50,100,200)
        m->reflect_color[0] = 50.0f / 255.0f;   // B
        m->reflect_color[1] = 100.0f / 255.0f;  // G
        m->reflect_color[2] = 200.0f / 255.0f;  // R
        m->reflect_color[3] = 0.0f;
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
    }

    // Material 1: multi-texture (has detail texture) → produces 2 3DA entries
    {
        TdpMaterial *m = &proj.materials[1];
        copy_str(m->name, sizeof(m->name), "Material_1_FF_DT_OP");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_MT_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.has_detail = 1;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;  // Dirt
        set_diffuse_texture(m, 0, "hull.pic");
        set_diffuse_texture(m, 1, "hull_d.pic");
    }

    // Material 2: no texture
    {
        TdpMaterial *m = &proj.materials[2];
        copy_str(m->name, sizeof(m->name), "Material_2_FF_NONE");
        copy_str(m->shader_name, sizeof(m->shader_name), "FF_ST_OP");
        m->classification.family = THREEDI_FAMILY_FF;
        m->classification.blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
        m->surface_type = 0x01;  // Dirt
    }

    // LOD 0
    TdpLod *lod = &proj.lods[0];
    copy_str(lod->scene_file, sizeof(lod->scene_file), "test.ase");
    lod->attributes = 5;
    copy_str(lod->render_function, sizeof(lod->render_function), "gnrc");
    lod->threshold = 0.0f;
    lod->part_anim_enabled = 1;

    // 2 part anims
    lod->part_anim_count = 2;
    lod->part_anims = static_cast<TdpPartAnim *>(std::calloc(2, sizeof(TdpPartAnim)));
    lod->part_anims[0].transform_as = 0;
    lod->part_anims[1].transform_as = 1;
    lod->part_anims[1].rotate_type = 2;
    lod->part_anims[1].yaw.func_id = 1;
    lod->part_anims[1].yaw.param0 = 0.5f;
    lod->part_anims[1].yaw.param2 = 10.0f;
    lod->part_anims[1].yaw.param3 = 350.0f;

    // 1 light
    lod->light_count = 1;
    lod->lights = static_cast<TdpLight *>(std::calloc(1, sizeof(TdpLight)));
    copy_str(lod->lights[0].name, sizeof(lod->lights[0].name), "LP01");
    lod->lights[0].colorgen_style = 1;
    lod->lights[0].colorgen_start[0] = 255;
    lod->lights[0].colorgen_start[1] = 200;
    lod->lights[0].colorgen_start[2] = 100;

    // Write 3DA
    char tmp_path[4096];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s%ctdp_3da_write_test.3da",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    int rc = tdp_write_3da(tmp_path, &proj);
    TEST_EXPECT(rc == 0 && "tdp_write_3da failed");

    // Read back and verify structure
    std::string output = read_file(tmp_path);
    TEST_EXPECT(!output.empty() && "3DA output file is empty");

    // --- Comment line ---
    TEST_EXPECT(contains(output, "/ 3DI metafile"));

    // --- General information: v3 format ---
    TEST_EXPECT(contains(output, "begin general_information"));
    TEST_EXPECT(contains(output, "3da_version 3"));
    TEST_EXPECT(contains(output, "3di_version 2"));
    TEST_EXPECT(contains(output, "username    opennova"));
    TEST_EXPECT(contains(output, "attributes: 5"));
    TEST_EXPECT(contains(output, "render_function gnrc"));
    TEST_EXPECT(contains(output, "threshold"));
    TEST_EXPECT(contains(output, "num_materials 4"));
    TEST_EXPECT(contains(output, "scale_factor 1.000000"));
    TEST_EXPECT(contains(output, "diffuse_set  TRUE"));
    TEST_EXPECT(contains(output, "pm_enable     0"));
    TEST_EXPECT(contains(output, "pm_polythresh 0"));
    TEST_EXPECT(contains(output, "part_anim_enable 1"));
    TEST_EXPECT(contains(output, "fakeskin_z 0"));
    TEST_EXPECT(contains(output, "polycollision 2"));
    TEST_EXPECT(contains(output, "end general_information"));

    // --- Material splitting: 4 3DA material entries ---
    TEST_EXPECT(contains(output, "begin material 0"));
    TEST_EXPECT(contains(output, "begin material 1"));
    TEST_EXPECT(contains(output, "begin material 2"));
    TEST_EXPECT(contains(output, "begin material 3"));
    TEST_EXPECT(!contains(output, "{") && "3DA should not contain braces");

    // --- Material fields (matching df4oed.exe::sub_421120 emission) ---
    TEST_EXPECT(contains(output, "multitexture_flags"));
    TEST_EXPECT(contains(output, "color_type 2"));
    TEST_EXPECT(contains(output, "alpha_texture"));
    TEST_EXPECT(contains(output, "reflect_type 0"));
    TEST_EXPECT(contains(output, "reflect_alpha 0"));
    TEST_EXPECT(contains(output, "actionplane_type     0"));
    TEST_EXPECT(contains(output, "projector_type       0"));
    TEST_EXPECT(contains(output, "projector_no_receive 0"));
    TEST_EXPECT(contains(output, "projector_yaw       0"));
    TEST_EXPECT(contains(output, "projector_pitch     0"));
    TEST_EXPECT(contains(output, "shader_type"));

    // --- Conditional emission ---
    // mat0: classification has two_sided + alpha_test, num_frames=1 → rattrib = 0x1 + 0x100 + 0x400 = 0x501 = 1281
    TEST_EXPECT(contains(output, "render_attributes 1281"));
    TEST_EXPECT(contains(output, "alpha_test 128"));
    // No test mat sets blend_mode != OPAQUE → no alpha_type / blending_mode emitted
    TEST_EXPECT(!contains(output, "alpha_type"));
    TEST_EXPECT(!contains(output, "blending_mode"));
    TEST_EXPECT(!contains(output, "physical_attributes"));

    // --- Material name = texture filename, description = material name ---
    TEST_EXPECT(contains(output, "name \"body.pic\""));
    TEST_EXPECT(contains(output, "description \"Material_0_FF_ST_OP\""));

    TEST_EXPECT(contains(output, "name \"hull.pic\""));
    TEST_EXPECT(contains(output, "description \"Material_1_FF_DT_OP\""));

    TEST_EXPECT(contains(output, "name \"hull_d.pic\""));
    TEST_EXPECT(contains(output, "multitexture_flags 2"));
    TEST_EXPECT(count_occurrences(output, "multitexture_flags 1") == 3);
    TEST_EXPECT(count_occurrences(output, "multitexture_flags 2") == 1);

    // --- green_texture values ---
    TEST_EXPECT(contains(output, "green_texture \"body.pic\""));
    TEST_EXPECT(contains(output, "green_texture \"hull.pic\""));
    TEST_EXPECT(contains(output, "green_texture \"hull_d.pic\""));

    // --- use_alpha_pcx ---
    TEST_EXPECT(contains(output, "use_alpha_pcx 1"));
    TEST_EXPECT(contains(output, "use_alpha_pcx 0"));

    // --- Per-component RGB fields ---
    TEST_EXPECT(contains(output, "rgbgen_sr"));
    TEST_EXPECT(contains(output, "rgbgen_sg"));
    TEST_EXPECT(contains(output, "rgbgen_sb"));
    TEST_EXPECT(contains(output, "rgbgen_er"));
    TEST_EXPECT(contains(output, "rgbgen_eg"));
    TEST_EXPECT(contains(output, "rgbgen_eb"));
    TEST_EXPECT(contains(output, "reflect_r"));
    TEST_EXPECT(contains(output, "reflect_g"));
    TEST_EXPECT(contains(output, "reflect_b"));
    TEST_EXPECT(!contains(output, "rgbgen_srgb"));
    TEST_EXPECT(!contains(output, "rgbgen_ergb"));
    TEST_EXPECT(!contains(output, "reflect_rgb"));

    // --- Should NOT have 3DP-only field names ---
    TEST_EXPECT(!contains(output, "rattrib "));
    TEST_EXPECT(!contains(output, "pattrib "));
    TEST_EXPECT(!contains(output, "alphatestvalue"));
    TEST_EXPECT(!contains(output, "diffusetex"));

    // --- Descriptive end tags ---
    TEST_EXPECT(contains(output, "end general_information"));
    TEST_EXPECT(contains(output, "end material"));
    TEST_EXPECT(contains(output, "end part_animation"));
    TEST_EXPECT(count_occurrences(output, "end material") == 4);
    TEST_EXPECT(count_occurrences(output, "end part_animation") == 2);

    // --- Part animation fields ---
    TEST_EXPECT(contains(output, "begin part_animation 0"));
    TEST_EXPECT(contains(output, "begin part_animation 1"));
    TEST_EXPECT(contains(output, "rotate_type"));
    TEST_EXPECT(contains(output, "transform_as"));
    TEST_EXPECT(contains(output, "yaw_func"));

    TEST_EXPECT(!contains(output, "trans_type"));
    TEST_EXPECT(!contains(output, "transx_func"));
    TEST_EXPECT(!contains(output, "transy_func"));
    TEST_EXPECT(!contains(output, "transz_func"));

    // --- Light fields ---
    TEST_EXPECT(contains(output, "begin light 0"));
    TEST_EXPECT(contains(output, "colorgen_style"));
    TEST_EXPECT(contains(output, "colorgen_start"));

    TEST_EXPECT(!contains(output, "disable_corona"));
    TEST_EXPECT(!contains(output, "disable_lightterrain"));
    TEST_EXPECT(!contains(output, "disable_lightobjects"));

    // Cleanup
    tdp_free(&proj);
    std::remove(tmp_path);

    std::printf("TDP 3DA write test PASSED\n");
    return 0;
}
