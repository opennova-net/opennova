// TDP roundtrip test: build a TdpProject, write to file, parse back, verify.

#include "tdp/tdp.h"
#include "threedi/threedi_ir.h"
#include "threedi/threedi_panm.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_paths.h"

static void copy_str(char *dst, size_t n, const char *src) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static bool float_eq(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) < eps;
}

int main() {
    // Build a project in memory
    TdpProject src;
    tdp_init(&src);
    src.version = 1;
    src.poly_collision_lod = 0;

    // 2 materials
    src.material_count = 2;
    src.materials = static_cast<TdpMaterial *>(std::calloc(2, sizeof(TdpMaterial)));

    // Material 0: basic
    {
        TdpMaterial *m = &src.materials[0];
        copy_str(m->name, sizeof(m->name), "Material_0_FF_ST_OP");
        copy_str(m->shader_tag, sizeof(m->shader_tag), "FF_ST_OP");
        m->ptype = 9;
        m->alphatestvalue = 128;
        copy_str(m->diffuse_tex[0], sizeof(m->diffuse_tex[0]), "body.pic");
        copy_str(m->diffuse_tex[1], sizeof(m->diffuse_tex[1]), "detail.pic");
        copy_str(m->normal_tex[0], sizeof(m->normal_tex[0]), "body_n.pic");
        m->diffuse_flags[0] = 0;
        m->diffuse_flags[1] = 1;
        m->reflect_rgb[0] = 200;
        m->reflect_rgb[1] = 100;
        m->reflect_rgb[2] = 50;
        m->rgbgen_style = 2;
        m->rgbgen_rate = 1.5f;
        m->rgbgen_phase = 0.25f;
        m->rgbgen_srgb[0] = 255;
        m->rgbgen_ergb[2] = 128;
        m->alphagen_style = 1;
        m->alphagen_rate = 0.5f;
        m->alphagen_phase = 0.1f;
        m->alphagen_start = 5.7f;  // fractional — writer truncates to int
        m->alphagen_end = 10.3f;   // fractional — writer truncates to int
        m->mapfunc_u_style = 3;
        m->mapfunc_u_rate = 2.0f;
        m->mapfunc_v_style = 1;
        m->mapfunc_v_end = 0.5f;
    }

    // Material 1: minimal with empty normaltex (fresh export default)
    {
        TdpMaterial *m = &src.materials[1];
        copy_str(m->name, sizeof(m->name), "Material_1_FF_DT_OP");
        copy_str(m->shader_tag, sizeof(m->shader_tag), "FF_DT_OP");
        m->ptype = 9;
        // normal_tex left empty (calloc zero)
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
    for (int i = 0; i < 3; ++i) {
        lod->part_anims[i].transform_as = i;
    }
    // Give part 1 some animation data
    {
        TdpPartAnim *pa = &lod->part_anims[1];
        pa->rotate_type = 2;
        pa->yaw.func_id = 1;
        pa->yaw.param0 = 0.5f;   // rate
        pa->yaw.param1 = 0.0f;   // phase
        pa->yaw.param2 = 10.0f;  // start
        pa->yaw.param3 = 350.0f; // end
    }

    // 1 light
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
    assert(rc == 0 && "tdp_write failed");

    // Parse back
    TdpProject dst;
    rc = tdp_parse(tmp_path, &dst);
    assert(rc == 0 && "tdp_parse failed");

    // Verify header
    assert(dst.version == src.version);
    assert(dst.poly_collision_lod == src.poly_collision_lod);

    // Verify materials
    assert(dst.material_count == src.material_count);
    for (size_t i = 0; i < src.material_count; ++i) {
        const TdpMaterial *s = &src.materials[i];
        const TdpMaterial *d = &dst.materials[i];
        assert(std::strcmp(s->name, d->name) == 0);
        assert(std::strcmp(s->shader_tag, d->shader_tag) == 0);
        assert(s->ptype == d->ptype);
        assert(s->alphatestvalue == d->alphatestvalue);
        assert(std::strcmp(s->diffuse_tex[0], d->diffuse_tex[0]) == 0);
        assert(std::strcmp(s->diffuse_tex[1], d->diffuse_tex[1]) == 0);
        assert(std::strcmp(s->normal_tex[0], d->normal_tex[0]) == 0);
        assert(s->diffuse_flags[1] == d->diffuse_flags[1]);
        assert(s->reflect_rgb[0] == d->reflect_rgb[0]);
        assert(s->reflect_rgb[1] == d->reflect_rgb[1]);
        assert(s->reflect_rgb[2] == d->reflect_rgb[2]);
        assert(s->rgbgen_style == d->rgbgen_style);
        assert(float_eq(s->rgbgen_rate, d->rgbgen_rate));
        assert(float_eq(s->rgbgen_phase, d->rgbgen_phase));
        assert(s->rgbgen_srgb[0] == d->rgbgen_srgb[0]);
        assert(s->rgbgen_ergb[2] == d->rgbgen_ergb[2]);
        assert(s->alphagen_style == d->alphagen_style);
        assert(float_eq(s->alphagen_rate, d->alphagen_rate));
        // alphagen_start/end are written as %i (truncated to int), so roundtrip loses fractional part
        assert(float_eq(d->alphagen_start, static_cast<float>(static_cast<int>(s->alphagen_start))));
        assert(float_eq(d->alphagen_end, static_cast<float>(static_cast<int>(s->alphagen_end))));
        // normaltex sentinel
        assert(std::strcmp(s->normal_tex[0], d->normal_tex[0]) == 0);
        assert(std::strcmp(s->normal_tex[1], d->normal_tex[1]) == 0);
        assert(s->mapfunc_u_style == d->mapfunc_u_style);
        assert(float_eq(s->mapfunc_u_rate, d->mapfunc_u_rate));
        assert(s->mapfunc_v_style == d->mapfunc_v_style);
        assert(float_eq(s->mapfunc_v_end, d->mapfunc_v_end));
    }

    // Verify LOD 0
    const TdpLod *sl = &src.lods[0];
    const TdpLod *dl = &dst.lods[0];
    assert(std::strcmp(sl->scene_file, dl->scene_file) == 0);
    assert(sl->attributes == dl->attributes);
    assert(std::strcmp(sl->render_function, dl->render_function) == 0);
    assert(sl->part_anim_enabled == dl->part_anim_enabled);
    assert(sl->part_anim_count == dl->part_anim_count);

    // Verify part anims
    for (size_t p = 0; p < sl->part_anim_count; ++p) {
        const TdpPartAnim *sp = &sl->part_anims[p];
        const TdpPartAnim *dp = &dl->part_anims[p];
        assert(sp->transform_as == dp->transform_as);
        assert(sp->rotate_type == dp->rotate_type);
        assert(sp->yaw.func_id == dp->yaw.func_id);
        assert(float_eq(sp->yaw.param0, dp->yaw.param0));
        assert(float_eq(sp->yaw.param2, dp->yaw.param2));
        assert(float_eq(sp->yaw.param3, dp->yaw.param3));
    }

    // Verify lights
    assert(sl->light_count == dl->light_count);
    assert(std::strcmp(sl->lights[0].name, dl->lights[0].name) == 0);
    assert(sl->lights[0].colorgen_style == dl->lights[0].colorgen_style);
    assert(sl->lights[0].colorgen_start[0] == dl->lights[0].colorgen_start[0]);
    assert(sl->lights[0].colorgen_start[1] == dl->lights[0].colorgen_start[1]);
    assert(sl->lights[0].colorgen_end[0] == dl->lights[0].colorgen_end[0]);

    // Cleanup
    tdp_free(&src);
    tdp_free(&dst);
    std::remove(tmp_path);

    std::printf("TDP roundtrip test PASSED\n");

    // =======================================================================
    // PANM flag extraction + routing via tdp_from_ir
    // =======================================================================
    {
        ThreediModelIR ir;
        std::memset(&ir, 0, sizeof(ir));

        // Minimal LOD with 1 part so panm_count uses part_animation_count
        ThreediIRLod lod0;
        std::memset(&lod0, 0, sizeof(lod0));
        lod0.part_count = 1;
        ir.lods = &lod0;
        ir.lod_count = 1;

        // Build 2 part animations with different flag combos
        ThreediIRPartAnimation panms[2];
        std::memset(panms, 0, sizeof(panms));

        // PANM 0: rotate_type=2, scale_type=1, trans_type=1 (X), reverse_rotate=1
        panms[0].flags = threedi_panm_pack_flags(1, 2, 1, 1);
        panms[0].part_index = 0;
        panms[0].rotation_y.control = 3;
        panms[0].rotation_y.rate = 512;      // 2.0 after /256
        panms[0].rotation_y.start = 4551;    // ~10 deg after /(16384/360)
        panms[0].rotation_y.end = 45511;     // ~1000 deg (clamped by s16 in real data)
        panms[0].scale_x.control = 1;
        panms[0].scale_x.rate = 256;         // 1.0 after /256
        panms[0].translation.control = 5;
        panms[0].translation.rate = 128;     // 0.5 after /256

        // PANM 1: rotate_type=0 (no rotation), scale_type=2 (per-axis), trans_type=3 (Z)
        panms[1].flags = threedi_panm_pack_flags(2, 0, 0, 3);
        panms[1].part_index = 1;
        panms[1].scale_x.control = 2;
        panms[1].scale_y.control = 4;
        panms[1].scale_z.control = 6;
        panms[1].translation.control = 7;
        panms[1].translation.rate = 256;

        ir.part_animations = panms;
        ir.part_animation_count = 2;

        TdpProject proj;
        int rc2 = tdp_from_ir(&ir, &proj);
        assert(rc2 == 0 && "tdp_from_ir failed");

        // Verify flag extraction for PANM 0
        {
            const TdpPartAnim *pa = &proj.lods[0].part_anims[0];
            assert(pa->rotate_type == 2);
            assert(pa->scale_type == 1);
            assert(pa->trans_type == 1);
            assert(pa->reverse_rotate == 1);

            // Rotation should be populated (rotate_type == 2)
            // rotation_y maps to pitch (rotation_x→yaw, rotation_y→pitch, rotation_z→roll)
            assert(pa->pitch.func_id == 3);

            // Uniform scale: scale_func populated, per-axis NOT
            assert(pa->scale.func_id == 1);
            assert(pa->scale_x.func_id == 0);
            assert(pa->scale_y.func_id == 0);
            assert(pa->scale_z.func_id == 0);

            // Translation routed to X only
            assert(pa->trans_x.func_id == 5);
            assert(pa->trans_y.func_id == 0);
            assert(pa->trans_z.func_id == 0);
        }

        // Verify flag extraction for PANM 1
        {
            const TdpPartAnim *pa = &proj.lods[0].part_anims[1];
            assert(pa->rotate_type == 0);
            assert(pa->scale_type == 2);
            assert(pa->trans_type == 3);
            assert(pa->reverse_rotate == 0);

            // No rotation (rotate_type == 0)
            assert(pa->yaw.func_id == 0);
            assert(pa->pitch.func_id == 0);
            assert(pa->roll.func_id == 0);

            // Per-axis scale
            assert(pa->scale.func_id == 0);
            assert(pa->scale_x.func_id == 2);
            assert(pa->scale_y.func_id == 4);
            assert(pa->scale_z.func_id == 6);

            // Translation routed to Z only
            assert(pa->trans_x.func_id == 0);
            assert(pa->trans_y.func_id == 0);
            assert(pa->trans_z.func_id == 7);
        }

        // Verify normaltex is empty (not "0")
        for (size_t i = 0; i < proj.material_count; ++i) {
            assert(proj.materials[i].normal_tex[0][0] == '\0');
            assert(proj.materials[i].normal_tex[1][0] == '\0');
        }

        tdp_free(&proj);
        // Don't free ir — stack-allocated with no owned allocations

        std::printf("TDP PANM flag/routing test PASSED\n");
    }

    // =======================================================================
    // Control register resolution via tdp_from_ir
    // =======================================================================
    {
        ThreediModelIR ir;
        std::memset(&ir, 0, sizeof(ir));

        // Minimal LOD with 1 part
        ThreediIRLod lod0;
        std::memset(&lod0, 0, sizeof(lod0));
        lod0.part_count = 1;
        ir.lods = &lod0;
        ir.lod_count = 1;

        // One control register
        ThreediIRControlRegister cregs[1];
        std::memset(cregs, 0, sizeof(cregs));
        copy_str(cregs[0].name, sizeof(cregs[0].name), "VEHICLE_GUNYAW");
        ir.control_registers = cregs;
        ir.control_register_count = 1;

        // Part animation: rotation yaw with register-based control (code 113 = 0x71)
        // rotation_x maps to yaw (rotation_x→yaw, rotation_y→pitch, rotation_z→roll)
        ThreediIRPartAnimation panm;
        std::memset(&panm, 0, sizeof(panm));
        panm.flags = threedi_panm_pack_flags(0, 2, 0, 0); // rotate_type=2
        panm.part_index = 0;
        panm.rotation_x.control = 113;         // register-based func
        panm.rotation_x.control_param = 0;     // index into control_registers
        panm.rotation_x.start = 0;
        panm.rotation_x.end = static_cast<int16_t>(360.0f * (16384.0f / 360.0f)); // 360 deg

        ir.part_animations = &panm;
        ir.part_animation_count = 1;

        TdpProject proj;
        int rc3 = tdp_from_ir(&ir, &proj);
        assert(rc3 == 0 && "tdp_from_ir failed");

        const TdpPartAnim *pa = &proj.lods[0].part_anims[0];
        assert(pa->yaw.func_id == 113);
        assert(std::strcmp(pa->yaw.ctrl_reg, "VEHICLE_GUNYAW") == 0);
        assert(pa->yaw.param1 == 0.0f);

        // Write and parse back to verify roundtrip
        char tmp2[4096];
        std::snprintf(tmp2, sizeof(tmp2), "%s%ctdp_ctrlreg_test.3dp",
                      test_paths_temp_dir(), TEST_PATHS_SEP);
        int wrc = tdp_write(tmp2, &proj);
        assert(wrc == 0 && "tdp_write failed");

        TdpProject parsed;
        int prc = tdp_parse(tmp2, &parsed);
        assert(prc == 0 && "tdp_parse failed");

        const TdpPartAnim *pa2 = &parsed.lods[0].part_anims[0];
        assert(pa2->yaw.func_id == 113);
        assert(std::strcmp(pa2->yaw.ctrl_reg, "VEHICLE_GUNYAW") == 0);
        assert(pa2->yaw.param1 == 0.0f);

        tdp_free(&proj);
        tdp_free(&parsed);
        std::remove(tmp2);

        std::printf("TDP control register test PASSED\n");
    }

    // =======================================================================
    // Material control register resolution + rattrib via tdp_from_ir
    // =======================================================================
    {
        ThreediModelIR ir;
        std::memset(&ir, 0, sizeof(ir));

        // Minimal LOD
        ThreediIRLod lod0;
        std::memset(&lod0, 0, sizeof(lod0));
        lod0.part_count = 1;
        ir.lods = &lod0;
        ir.lod_count = 1;

        // Two control registers
        ThreediIRControlRegister cregs[2];
        std::memset(cregs, 0, sizeof(cregs));
        copy_str(cregs[0].name, sizeof(cregs[0].name), "VEHICLE_WHEELS00");
        copy_str(cregs[1].name, sizeof(cregs[1].name), "ANIM_CTRL_01");
        ir.control_registers = cregs;
        ir.control_register_count = 2;

        // One material with register-driven UV scroll and animation
        ThreediIRMaterial mat;
        std::memset(&mat, 0, sizeof(mat));
        mat.index = 0;
        copy_str(mat.shader_name, sizeof(mat.shader_name), "FF_ST_OP");

        // mapfunc_u: style 114 (> 0x70), reg index 0 -> "VEHICLE_WHEELS00"
        mat.u_params.style = 114;
        mat.u_params.reg = 0;
        mat.u_params.gen_rate = 1.0f;

        // mapfunc_v: style 50 (< 0x70), reg should NOT be resolved
        mat.v_params.style = 50;
        mat.v_params.reg = 0;

        // rgbgen: style 113 (> 0x70), reg index 1 -> "ANIM_CTRL_01"
        mat.rgb_gen.style = 113;
        mat.rgb_gen.reg = 1;

        // alphagen: style 115 (> 0x70), reg index 0 -> "VEHICLE_WHEELS00"
        mat.alpha_gen.style = 115;
        mat.alpha_gen.reg = 0;

        // animation: type 1 (ctrl reg driven), cycle_frame_time = reg index 1
        mat.animation.animation_type = 1;
        mat.animation.cycle_frame_time = 1;
        mat.animation.num_frames = 4;

        // Two-sided flag
        mat.flags = THREEDI_IR_MATERIAL_FLAG_TWO_SIDED;

        ir.materials = &mat;
        ir.material_count = 1;

        TdpProject proj;
        int rc4 = tdp_from_ir(&ir, &proj);
        assert(rc4 == 0 && "tdp_from_ir failed");
        assert(proj.material_count == 1);

        const TdpMaterial *dm = &proj.materials[0];

        // mapfunc_u_ctrlreg should be resolved (style > 0x70)
        assert(std::strcmp(dm->mapfunc_u_ctrlreg, "VEHICLE_WHEELS00") == 0);

        // mapfunc_v_ctrlreg should be empty (style <= 0x70)
        assert(dm->mapfunc_v_ctrlreg[0] == '\0');

        // rgbgen_ctrlreg should be resolved
        assert(std::strcmp(dm->rgbgen_ctrlreg, "ANIM_CTRL_01") == 0);

        // alphagen_ctrlreg should be resolved
        assert(std::strcmp(dm->alphagen_ctrlreg, "VEHICLE_WHEELS00") == 0);

        // anim_ctrlreg should be resolved (animation_type == 1)
        assert(std::strcmp(dm->anim_ctrlreg, "ANIM_CTRL_01") == 0);

        // rattrib: bit 0 = two-sided, bit 8 = animated (num_frames=4)
        assert(dm->rattrib == 0x101);

        // Write and parse back to verify roundtrip
        char tmp3[4096];
        std::snprintf(tmp3, sizeof(tmp3), "%s%ctdp_mat_ctrlreg_test.3dp",
                      test_paths_temp_dir(), TEST_PATHS_SEP);
        int wrc3 = tdp_write(tmp3, &proj);
        assert(wrc3 == 0 && "tdp_write failed");

        TdpProject parsed3;
        int prc3 = tdp_parse(tmp3, &parsed3);
        assert(prc3 == 0 && "tdp_parse failed");

        const TdpMaterial *pm = &parsed3.materials[0];
        assert(std::strcmp(pm->mapfunc_u_ctrlreg, "VEHICLE_WHEELS00") == 0);
        assert(pm->mapfunc_v_ctrlreg[0] == '\0');
        assert(std::strcmp(pm->rgbgen_ctrlreg, "ANIM_CTRL_01") == 0);
        assert(std::strcmp(pm->alphagen_ctrlreg, "VEHICLE_WHEELS00") == 0);
        assert(std::strcmp(pm->anim_ctrlreg, "ANIM_CTRL_01") == 0);
        // bit 0 = two-sided, bit 8 = animated (num_frames > 0)
        assert(pm->rattrib == 0x101);

        tdp_free(&proj);
        tdp_free(&parsed3);
        std::remove(tmp3);

        std::printf("TDP material ctrlreg + rattrib test PASSED\n");
    }

    // =======================================================================
    // Surface type → ptype mapping via tdp_from_ir
    // =======================================================================
    {
        ThreediModelIR ir;
        std::memset(&ir, 0, sizeof(ir));

        // Minimal LOD with 1 part
        ThreediIRLod lod0;
        std::memset(&lod0, 0, sizeof(lod0));
        lod0.part_count = 1;
        ir.lods = &lod0;
        ir.lod_count = 1;

        // Three materials with different surface_type values
        ThreediIRMaterial mats[3];
        std::memset(mats, 0, sizeof(mats));
        copy_str(mats[0].shader_name, sizeof(mats[0].shader_name), "FF_ST_OP");
        copy_str(mats[1].shader_name, sizeof(mats[1].shader_name), "FF_ST_OP");
        copy_str(mats[2].shader_name, sizeof(mats[2].shader_name), "FF_ST_OP");

        mats[0].surface_type = 0x12; // Cement → ptype 4
        mats[1].surface_type = 0x01; // Mud → ptype 9
        mats[2].surface_type = 0x10; // Sand → ptype 5

        ir.materials = mats;
        ir.material_count = 3;

        TdpProject proj;
        int rc5 = tdp_from_ir(&ir, &proj);
        assert(rc5 == 0 && "tdp_from_ir failed");
        assert(proj.material_count == 3);
        assert(proj.materials[0].ptype == 4);  // Cement
        assert(proj.materials[1].ptype == 9);  // Mud
        assert(proj.materials[2].ptype == 5);  // Sand

        // Also verify all defined surface_type mappings
        struct { uint8_t st; int ptype; } mappings[] = {
            {0x0E, 0}, {0x0D, 1}, {0x0C, 2}, {0x11, 3}, {0x12, 4},
            {0x10, 5}, {0x0F, 6}, {0x07, 7}, {0x13, 8}, {0x01, 9},
        };
        for (auto &m : mappings) {
            ThreediIRMaterial test_mat;
            std::memset(&test_mat, 0, sizeof(test_mat));
            copy_str(test_mat.shader_name, sizeof(test_mat.shader_name), "FF_ST_OP");
            test_mat.surface_type = m.st;

            ThreediModelIR ir2;
            std::memset(&ir2, 0, sizeof(ir2));
            ir2.lods = &lod0;
            ir2.lod_count = 1;
            ir2.materials = &test_mat;
            ir2.material_count = 1;

            TdpProject p2;
            int r = tdp_from_ir(&ir2, &p2);
            assert(r == 0);
            assert(p2.materials[0].ptype == m.ptype);
            tdp_free(&p2);
        }

        // Unknown surface_type defaults to ptype 9
        {
            ThreediIRMaterial unk_mat;
            std::memset(&unk_mat, 0, sizeof(unk_mat));
            copy_str(unk_mat.shader_name, sizeof(unk_mat.shader_name), "FF_ST_OP");
            unk_mat.surface_type = 0xFF; // unknown

            ThreediModelIR ir3;
            std::memset(&ir3, 0, sizeof(ir3));
            ir3.lods = &lod0;
            ir3.lod_count = 1;
            ir3.materials = &unk_mat;
            ir3.material_count = 1;

            TdpProject p3;
            int r = tdp_from_ir(&ir3, &p3);
            assert(r == 0);
            assert(p3.materials[0].ptype == 9);
            tdp_free(&p3);
        }

        // Default surface_type (0, from calloc) should also give ptype 9
        {
            ThreediIRMaterial zero_mat;
            std::memset(&zero_mat, 0, sizeof(zero_mat));
            copy_str(zero_mat.shader_name, sizeof(zero_mat.shader_name), "FF_ST_OP");
            // surface_type = 0 (not in table)

            ThreediModelIR ir4;
            std::memset(&ir4, 0, sizeof(ir4));
            ir4.lods = &lod0;
            ir4.lod_count = 1;
            ir4.materials = &zero_mat;
            ir4.material_count = 1;

            TdpProject p4;
            int r = tdp_from_ir(&ir4, &p4);
            assert(r == 0);
            assert(p4.materials[0].ptype == 9);
            tdp_free(&p4);
        }

        tdp_free(&proj);
        std::printf("TDP surface_type -> ptype mapping test PASSED\n");
    }

    // =======================================================================
    // Normal texture slots + animated frames via tdp_from_ir
    // =======================================================================
    {
        ThreediModelIR ir;
        std::memset(&ir, 0, sizeof(ir));

        ThreediIRLod lod0;
        std::memset(&lod0, 0, sizeof(lod0));
        lod0.part_count = 1;
        ir.lods = &lod0;
        ir.lod_count = 1;

        // Material with all 4 static texture slots + animated frames
        ThreediIRMaterial mat;
        std::memset(&mat, 0, sizeof(mat));
        copy_str(mat.shader_name, sizeof(mat.shader_name), "FF_DT_OP");

        // Slot 1: DIFFUSE static, clamped
        copy_str(mat.textures[0].name, sizeof(mat.textures[0].name), "body.pic");
        mat.textures[0].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
        mat.textures[0].flags = 0x02; // clamped bit set, not animated
        mat.textures[0].frame = 0;

        // Slot 2: DETAIL static, not clamped
        copy_str(mat.textures[1].name, sizeof(mat.textures[1].name), "detail.pic");
        mat.textures[1].slot = THREEDI_IR_TEX_SLOT_DETAIL;
        mat.textures[1].flags = 0x00;
        mat.textures[1].frame = 0;

        // Slot 3: NORMAL static, clamped
        copy_str(mat.textures[2].name, sizeof(mat.textures[2].name), "body_n.pic");
        mat.textures[2].slot = THREEDI_IR_TEX_SLOT_NORMAL;
        mat.textures[2].flags = 0x02;
        mat.textures[2].frame = 0;

        // Slot 4: NORMAL_B static, not clamped
        copy_str(mat.textures[3].name, sizeof(mat.textures[3].name), "detail_n.pic");
        mat.textures[3].slot = THREEDI_IR_TEX_SLOT_NORMAL_B;
        mat.textures[3].flags = 0x00;
        mat.textures[3].frame = 0;

        // Animated diffuse slot 1, frames 0 and 1 (flags bit 0 = animated)
        copy_str(mat.textures[4].name, sizeof(mat.textures[4].name), "anim0.pic");
        mat.textures[4].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
        mat.textures[4].flags = 0x01; // animated, not clamped
        mat.textures[4].frame = 0;

        copy_str(mat.textures[5].name, sizeof(mat.textures[5].name), "anim1.pic");
        mat.textures[5].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
        mat.textures[5].flags = 0x03; // animated + clamped
        mat.textures[5].frame = 1;

        // Animated normal slot 3, frame 0
        copy_str(mat.textures[6].name, sizeof(mat.textures[6].name), "anim_n0.pic");
        mat.textures[6].slot = THREEDI_IR_TEX_SLOT_NORMAL;
        mat.textures[6].flags = 0x01; // animated, not clamped
        mat.textures[6].frame = 0;

        mat.texture_count = 7;

        ir.materials = &mat;
        ir.material_count = 1;

        TdpProject proj;
        int rc6 = tdp_from_ir(&ir, &proj);
        assert(rc6 == 0 && "tdp_from_ir failed");
        assert(proj.material_count == 1);

        const TdpMaterial *dm = &proj.materials[0];

        // Static textures
        assert(std::strcmp(dm->diffuse_tex[0], "body.pic") == 0);
        assert(dm->diffuse_flags[0] == 1); // clamped
        assert(std::strcmp(dm->diffuse_tex[1], "detail.pic") == 0);
        assert(dm->diffuse_flags[1] == 0);
        assert(std::strcmp(dm->normal_tex[0], "body_n.pic") == 0);
        assert(dm->normal_flags[0] == 1); // clamped
        assert(std::strcmp(dm->normal_tex[1], "detail_n.pic") == 0);
        assert(dm->normal_flags[1] == 0);

        // Animated diffuse[0] frames
        assert(std::strcmp(dm->anim_diffuse[0][0].path, "anim0.pic") == 0);
        assert(dm->anim_diffuse[0][0].enabled == 0); // not clamped
        assert(std::strcmp(dm->anim_diffuse[0][1].path, "anim1.pic") == 0);
        assert(dm->anim_diffuse[0][1].enabled == 1); // clamped

        // Animated normal[0] frame 0
        assert(std::strcmp(dm->anim_normal[0][0].path, "anim_n0.pic") == 0);
        assert(dm->anim_normal[0][0].enabled == 0);

        // Write and parse back to verify full roundtrip
        char tmp4[4096];
        std::snprintf(tmp4, sizeof(tmp4), "%s%ctdp_texslot_test.3dp",
                      test_paths_temp_dir(), TEST_PATHS_SEP);
        int wrc4 = tdp_write(tmp4, &proj);
        assert(wrc4 == 0 && "tdp_write failed");

        TdpProject parsed4;
        int prc4 = tdp_parse(tmp4, &parsed4);
        assert(prc4 == 0 && "tdp_parse failed");

        const TdpMaterial *pm4 = &parsed4.materials[0];
        assert(std::strcmp(pm4->normal_tex[0], "body_n.pic") == 0);
        assert(pm4->normal_flags[0] == 1);
        assert(std::strcmp(pm4->normal_tex[1], "detail_n.pic") == 0);
        assert(pm4->normal_flags[1] == 0);
        assert(std::strcmp(pm4->anim_diffuse[0][0].path, "anim0.pic") == 0);
        assert(pm4->anim_diffuse[0][0].enabled == 0);
        assert(std::strcmp(pm4->anim_diffuse[0][1].path, "anim1.pic") == 0);
        assert(pm4->anim_diffuse[0][1].enabled == 1);
        assert(std::strcmp(pm4->anim_normal[0][0].path, "anim_n0.pic") == 0);

        tdp_free(&proj);
        tdp_free(&parsed4);
        std::remove(tmp4);

        std::printf("TDP normal texture + animated frame test PASSED\n");
    }

    return 0;
}
