// Test for tdp_write_3da: verify 3DA v3 output format from a known TdpProject.

#include "tdp/tdp.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

int main() {
    // Build a project in memory
    TdpProject proj;
    tdp_init(&proj);
    proj.version = 1;
    proj.poly_collision_lod = 2;

    // 3 materials: single-texture, multi-texture, empty-texture
    proj.material_count = 3;
    proj.materials = static_cast<TdpMaterial *>(std::calloc(3, sizeof(TdpMaterial)));

    // Material 0: single texture, has alpha test, rgbgen, reflect
    {
        TdpMaterial *m = &proj.materials[0];
        copy_str(m->name, sizeof(m->name), "Material_0_FF_ST_OP");
        copy_str(m->shader_tag, sizeof(m->shader_tag), "FF_ST_OP");
        m->ptype = 4;
        m->rattrib = 0x101;
        m->alphatestvalue = 128;
        copy_str(m->diffuse_tex[0], sizeof(m->diffuse_tex[0]), "body.pic");
        m->reflect_rgb[0] = 200;
        m->reflect_rgb[1] = 100;
        m->reflect_rgb[2] = 50;
        m->rgbgen_style = 2;
        m->rgbgen_rate = 1.5f;
        m->rgbgen_srgb[0] = 255;
        m->rgbgen_srgb[1] = 128;
        m->rgbgen_srgb[2] = 64;
        m->rgbgen_ergb[0] = 10;
        m->rgbgen_ergb[1] = 20;
        m->rgbgen_ergb[2] = 30;
        m->mapfunc_u_style = 3;
        m->mapfunc_u_rate = 2.0f;
        m->alphagen_style = 1;
        m->alphagen_rate = 0.5f;
    }

    // Material 1: multi-texture (has detail texture) → should produce 2 3DA entries
    {
        TdpMaterial *m = &proj.materials[1];
        copy_str(m->name, sizeof(m->name), "Material_1_FF_DT_OP");
        copy_str(m->diffuse_tex[0], sizeof(m->diffuse_tex[0]), "hull.pic");
        copy_str(m->diffuse_tex[1], sizeof(m->diffuse_tex[1]), "hull_d.pic");
    }

    // Material 2: no texture (empty diffuse_tex)
    {
        TdpMaterial *m = &proj.materials[2];
        copy_str(m->name, sizeof(m->name), "Material_2_FF_NONE");
        m->ptype = 9;
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
    assert(rc == 0 && "tdp_write_3da failed");

    // Read back and verify structure
    std::string output = read_file(tmp_path);
    assert(!output.empty() && "3DA output file is empty");

    // --- Comment line ---
    assert(contains(output, "/ 3DI metafile"));

    // --- General information: v3 format ---
    assert(contains(output, "begin general_information"));
    assert(contains(output, "3da_version 3"));
    assert(contains(output, "3di_version 5"));
    assert(contains(output, "username opennova"));
    assert(contains(output, "attributes: 1"));
    assert(contains(output, "render_function gnrc"));
    assert(contains(output, "threshold"));
    // 3 source materials → mat0(1 entry) + mat1(2 entries) + mat2(1 entry) = 4
    assert(contains(output, "num_materials 4"));
    assert(contains(output, "scale_factor 1.000000"));
    assert(contains(output, "diffuse_set  TRUE"));
    assert(contains(output, "pm_enable 0"));
    assert(contains(output, "pm_polythresh 0"));
    assert(contains(output, "part_anim_enable 1"));
    assert(contains(output, "fakeskin_z 0"));
    assert(contains(output, "polycollision 2"));
    assert(contains(output, "end general_information"));

    // --- Material splitting: 4 3DA material entries ---
    assert(contains(output, "begin material 0"));
    assert(contains(output, "begin material 1"));
    assert(contains(output, "begin material 2")); // detail slot of mat1
    assert(contains(output, "begin material 3")); // mat2
    assert(!contains(output, "{") && "3DA should not contain braces");

    // --- v3 material fields ---
    assert(contains(output, "multitexture_flags"));
    assert(contains(output, "color_type 2"));
    assert(contains(output, "blending_mode 1"));
    assert(contains(output, "alpha_texture"));
    assert(contains(output, "reflect_type 0"));
    assert(contains(output, "reflect_alpha 0"));
    assert(contains(output, "actionplane_type 0"));
    assert(contains(output, "projector_type 0"));
    assert(contains(output, "projector_no_receive 0"));
    assert(contains(output, "projector_yaw 0"));
    assert(contains(output, "projector_pitch 0"));
    assert(contains(output, "shader_type 0"));

    // --- Removed v1-only fields ---
    assert(!contains(output, "render_attributes"));
    assert(!contains(output, "physical_attributes"));
    assert(!contains(output, "alpha_type"));
    assert(!contains(output, "alpha_test"));

    // --- Material name = texture filename, description = material name ---
    // Material 0: name="body.pic", description="Material_0_FF_ST_OP"
    assert(contains(output, "name \"body.pic\""));
    assert(contains(output, "description \"Material_0_FF_ST_OP\""));

    // Material 1 primary slot: name="hull.pic"
    assert(contains(output, "name \"hull.pic\""));
    assert(contains(output, "description \"Material_1_FF_DT_OP\""));

    // Material 1 detail slot: name="hull_d.pic", multitexture_flags 2
    assert(contains(output, "name \"hull_d.pic\""));
    assert(contains(output, "multitexture_flags 2"));
    // Count multitexture_flags 1 (should be 3: mat0, mat1-primary, mat2)
    assert(count_occurrences(output, "multitexture_flags 1") == 3);
    // Count multitexture_flags 2 (should be 1: mat1-detail)
    assert(count_occurrences(output, "multitexture_flags 2") == 1);

    // --- green_texture values ---
    assert(contains(output, "green_texture \"body.pic\""));
    assert(contains(output, "green_texture \"hull.pic\""));
    assert(contains(output, "green_texture \"hull_d.pic\""));

    // --- use_alpha_pcx ---
    // mat0 has alphatestvalue=128 → 1; mat1 and mat2 have 0 → 0
    assert(contains(output, "use_alpha_pcx 1"));
    assert(contains(output, "use_alpha_pcx 0"));

    // --- Per-component RGB fields (NOT combined triplets) ---
    assert(contains(output, "rgbgen_sr"));
    assert(contains(output, "rgbgen_sg"));
    assert(contains(output, "rgbgen_sb"));
    assert(contains(output, "rgbgen_er"));
    assert(contains(output, "rgbgen_eg"));
    assert(contains(output, "rgbgen_eb"));
    assert(contains(output, "reflect_r"));
    assert(contains(output, "reflect_g"));
    assert(contains(output, "reflect_b"));
    assert(!contains(output, "rgbgen_srgb"));
    assert(!contains(output, "rgbgen_ergb"));
    assert(!contains(output, "reflect_rgb"));

    // --- Should NOT have 3DP-only field names ---
    assert(!contains(output, "rattrib"));
    assert(!contains(output, "pattrib"));
    assert(!contains(output, "alphatestvalue"));
    assert(!contains(output, "diffusetex"));

    // --- Descriptive end tags ---
    assert(contains(output, "end general_information"));
    assert(contains(output, "end material"));
    assert(contains(output, "end part_animation"));
    // 4 material blocks → 4 "end material" tags
    assert(count_occurrences(output, "end material") == 4);
    // 2 part_animation blocks → 2 "end part_animation" tags
    assert(count_occurrences(output, "end part_animation") == 2);

    // --- Part animation fields ---
    assert(contains(output, "begin part_animation 0"));
    assert(contains(output, "begin part_animation 1"));
    assert(contains(output, "rotate_type"));
    assert(contains(output, "transform_as"));
    assert(contains(output, "yaw_func"));

    // 3DA does NOT contain translation fields
    assert(!contains(output, "trans_type"));
    assert(!contains(output, "transx_func"));
    assert(!contains(output, "transy_func"));
    assert(!contains(output, "transz_func"));

    // --- Light fields ---
    assert(contains(output, "begin light 0"));
    assert(contains(output, "colorgen_style"));
    assert(contains(output, "colorgen_start"));

    // 3DA does NOT contain disable_* fields
    assert(!contains(output, "disable_corona"));
    assert(!contains(output, "disable_lightterrain"));
    assert(!contains(output, "disable_lightobjects"));

    // Cleanup
    tdp_free(&proj);
    std::remove(tmp_path);

    std::printf("TDP 3DA write test PASSED\n");
    return 0;
}
