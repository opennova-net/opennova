#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <formats/threedi/threedi_panm_runtime.h>

using namespace opennova::threedi;

static int expect_eq(const char *label, int32_t got, int32_t expected) {
    if (got != expected) {
        fprintf(stderr, "%s: got %d expected %d\n", label, got, expected);
        return 0;
    }
    return 1;
}

static int expect_near(const char *label, float got, float expected) {
    if (fabsf(got - expected) > 0.0001f) {
        fprintf(stderr, "%s: got %.8f expected %.8f\n", label, got, expected);
        return 0;
    }
    return 1;
}

int main(void) {
    int ok = 1;
    int32_t ctrl[THREEDI_CTRL_REGISTER_COUNT];
    const uint8_t *table;
    ThreediTransform t_set, t_ctrl, t_ctrl_add, t_sin, t_sin_small, t_rand;
    int32_t delta_sin;

    memset(ctrl, 0, sizeof(ctrl));

    table = threedi_panm_wave_table();
    ok &= expect_eq("table_sin0", table[256], 0x7F);
    ok &= expect_eq("table_rand0", table[1536], 0x83);

    // SET (control=24): returns start<<8
    memset(&t_set, 0, sizeof(t_set));
    t_set.control = 24; t_set.start = 10; t_set.end = 20;
    ok &= expect_eq("set", threedi_panm_sample_track_raw(&t_set, 0, NULL), 10 << 8);

    // Retail PANM code 113 indexes the signed value dwords directly by the
    // already-resolved global ordinal.
    memset(&t_ctrl, 0, sizeof(t_ctrl));
    t_ctrl.control = 113;
    t_ctrl.control_param = 7;
    t_ctrl.rate = 321;
    t_ctrl.start = 30;
    t_ctrl.end = 10; // negative delta
    ctrl[14] = 0x1234;

    ctrl[7] = 0;
    ok &= expect_eq("ctrl_reg7_zero",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 30 << 8);
    ctrl[7] = 0x8000;
    ok &= expect_eq("ctrl_reg7_mid_negative_delta",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 20 << 8);
    ctrl[7] = 0x10000;
    ok &= expect_eq("ctrl_reg7_exact_endpoint",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 10 << 8);
    ctrl[7] = -0x8000;
    ok &= expect_eq("ctrl_reg7_signed_negative",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 40 << 8);
    ctrl[7] = 0x4000;
    ok &= expect_eq("ctrl_reg7_time_zero",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 25 << 8);
    ok &= expect_eq("ctrl_reg7_time_independent",
                    threedi_panm_sample_track_raw(&t_ctrl, 9000, ctrl), 25 << 8);
    ok &= expect_eq("ctrl_reg7_null_table",
                    threedi_panm_sample_track_raw(&t_ctrl, 9000, NULL), 30 << 8);
    t_ctrl.control_param = 0xFF;
    ok &= expect_eq("ctrl_invalid_resolved_ordinal_is_safe",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 30 << 8);
    t_ctrl.control_param = 7;

    // Retail's 32-bit IMUL intentionally wraps before SAR. A widened multiply
    // would produce the positive endpoint instead.
    t_ctrl.start = -32768;
    t_ctrl.end = 32767;
    ctrl[7] = 0x10000;
    ok &= expect_eq("ctrl_reg7_low32_imul_wrap",
                    threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), -8388864);

    // Despite its shared catalog name, PANM code 114 is not a second control
    // operation in retail. It follows CWaveformTable_WaveLookup (low nibble 2: sine) and
    // ignores the supplied control table.
    memset(&t_ctrl_add, 0, sizeof(t_ctrl_add));
    t_ctrl_add.control = 114;
    t_ctrl_add.rate = 1;
    t_ctrl_add.end = 256;
    ctrl[0] = 0xFFFF;
    ok &= expect_eq("ctrl_114_sine_t0",
                    threedi_panm_sample_track_raw(&t_ctrl_add, 0, ctrl), 0x7F00);
    ok &= expect_eq("ctrl_114_sine_t1000",
                    threedi_panm_sample_track_raw(&t_ctrl_add, 1000, ctrl), 0x8200);
    ctrl[0] = 0;
    ok &= expect_eq("ctrl_114_ignores_register",
                    threedi_panm_sample_track_raw(&t_ctrl_add, 0, ctrl), 0x7F00);

    // Sine wave (control=0x12): start=0, end=256 -> delta=256
    memset(&t_sin, 0, sizeof(t_sin));
    t_sin.control = 0x12; t_sin.end = 256;
    delta_sin = (int16_t)t_sin.end - (int16_t)t_sin.start;
    ok &= expect_eq("delta_sin", delta_sin, 256);
    ok &= expect_eq("sin", threedi_panm_sample_track_raw(&t_sin, 0, NULL), 0x7F00);

    // Sine wave with delta=1
    memset(&t_sin_small, 0, sizeof(t_sin_small));
    t_sin_small.control = 0x12; t_sin_small.end = 1;
    ok &= expect_eq("sin_delta1", threedi_panm_sample_track_raw(&t_sin_small, 0, NULL), 0x7F);

    // Random band (control=0x17)
    memset(&t_rand, 0, sizeof(t_rand));
    t_rand.control = 0x17; t_rand.end = 256;
    ok &= expect_eq("rand_band", threedi_panm_sample_track_raw(&t_rand, 0, NULL), 0x8300);

    // A spinner (rotation type 1, its rate 1.0f in the four bytes from
    // rotation_y.control) stands still in JO: the loader keeps no flag for the
    // type, so the builder takes the static copy at every time, and the part
    // after it is built too (the loop goes on) [orig: GPM_LoadRenderModel
    // @ 0x5B54C7..0x5B54F3; Model_TransformBoneMatrices @ 0x58E478 -> @ 0x58F008].
    ThreediPartAnimation spinner[2];
    ThreediVec3 spin_pivots[2] = {{0.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    ThreediMatrix4x4 spin_in[2], at_zero[2], at_quarter[2];
    float coefficient = 1.0f;
    memset(spinner, 0, sizeof(spinner));
    spinner[0].flags = 1u << 8;
    spinner[0].parent_subobject = 0xff;
    memcpy(&spinner[0].rotation_y.control, &coefficient, sizeof(coefficient));
    spinner[1].subobject_index = 1;
    spinner[1].parent_subobject = 0xff;
    spinner[1].flags = 1u; // a uniform scale from its scale_x track
    spinner[1].scale_x.control = 24; // SET: start << 8
    spinner[1].scale_x.start = 512; // 2.0 in 16.16
    for (int i = 0; i < 2; ++i) {
        threedi_mat4_identity(&spin_in[i]);
        memset(&at_zero[i], 0xCD, sizeof(at_zero[i]));
        memset(&at_quarter[i], 0xCD, sizeof(at_quarter[i]));
    }
    ok &= expect_eq("spinner_t0_rc", threedi_panm_build_node_matrices(
        spinner, 2, spin_pivots, NULL, NULL, spin_in, ThreediPanmInputs::kRestFrames, NULL, 0,
        NULL, at_zero), 0);
    ok &= expect_eq("spinner_t250_rc", threedi_panm_build_node_matrices(
        spinner, 2, spin_pivots, NULL, NULL, spin_in, ThreediPanmInputs::kRestFrames, NULL, 250,
        NULL, at_quarter), 0);
    ok &= expect_near("spinner_t0_m0", at_zero[0].m[0], 1.0f);
    ok &= expect_near("spinner_t250_m0", at_quarter[0].m[0], 1.0f);
    ok &= expect_near("spinner_t250_m1", at_quarter[0].m[1], 0.0f);
    ok &= expect_near("spinner_t250_m5", at_quarter[0].m[5], 1.0f);
    ok &= expect_eq("spinner_is_not_live", threedi_panm_animation_is_live(spinner[0]) ? 1 : 0, 0);
    ok &= expect_near("after_spinner_scaled", at_quarter[1].m[5], 2.0f);

    // The PSP flagpole: node 0 names ITSELF as its parent and scales Y about
    // its pivot (11.5 up) by the LFP_CAMPPERCENT register; node 1, the flag,
    // rides it from 15.3 up. Output slot 0 receives its input before node 0
    // is built, so the self-parent carry restores the pivot: full control is
    // the bind pose, zero control drops the flag to the pole's base.
    // [orig: Model_TransformBoneMatrices @0x58e451..0x58e46b]
    ThreediPartAnimation pole[2];
    ThreediVec3 pole_pivots[2] = {{0.0f, 11.5f, 0.0f}, {0.0f, 15.3f, 0.0f}};
    ThreediMatrix4x4 pole_in[2], pole_out[2];
    memset(pole, 0, sizeof(pole));
    pole[0].subobject_index = 0;
    pole[0].parent_subobject = 0;
    pole[0].flags = 2u; // per-axis scale
    pole[0].scale_y.control = 113;
    pole[0].scale_y.control_param = 7;
    pole[0].scale_y.end = 256;
    pole[1].subobject_index = 1;
    pole[1].parent_subobject = 0;
    for (int i = 0; i < 2; ++i) {
        threedi_mat4_identity(&pole_in[i]);
        pole_in[i].m[13] = pole_pivots[i].y; // the cache's inputs carry each part's pivot
        memset(&pole_out[i], 0xCD, sizeof(pole_out[i])); // an unwritten output slot
    }
    memset(ctrl, 0, sizeof(ctrl));
    ctrl[7] = 0x10000;
    ok &= expect_eq("pole_full_rc", threedi_panm_build_node_matrices(
        pole, 2, pole_pivots, NULL, NULL, pole_in, ThreediPanmInputs::kRestFrames, NULL, 0,
        ctrl, pole_out), 0);
    ok &= expect_near("pole_full_sy", pole_out[0].m[5], 1.0f);
    ok &= expect_near("pole_full_ty", pole_out[0].m[13], 0.0f);
    ok &= expect_near("flag_full_ty", pole_out[1].m[13], 0.0f);
    ctrl[7] = 0;
    ok &= expect_eq("pole_down_rc", threedi_panm_build_node_matrices(
        pole, 2, pole_pivots, NULL, NULL, pole_in, ThreediPanmInputs::kRestFrames, NULL, 0,
        ctrl, pole_out), 0);
    ok &= expect_near("pole_down_sy", pole_out[0].m[5], 0.0f);
    ok &= expect_near("pole_down_ty", pole_out[0].m[13], 11.5f);
    ok &= expect_near("flag_down_ty", pole_out[1].m[13], 11.5f - 15.3f);

    // An Euler row in the reversed (Z, X, Y) order whose rotation-z track turns on a
    // register, as shipped rows flagged 0x00010200 carry one (the retired retail rows'
    // models reached it, 2026-10-19): register full, the part turns about Z by the
    // track's end; register zero, it stands. [orig: Model_TransformBoneMatrices
    // @0x58E8AA, the reversed order]
    ThreediPartAnimation zyx;
    ThreediVec3 origin = {0.0f, 0.0f, 0.0f};
    ThreediMatrix4x4 zyx_in, zyx_out;
    memset(&zyx, 0, sizeof(zyx));
    zyx.flags = threedi_panm_pack_flags(0, 2, 1, 0);
    zyx.rotation_z.control = 113;
    zyx.rotation_z.control_param = 3;
    zyx.rotation_z.end = 4096; // a quarter turn
    threedi_mat4_identity(&zyx_in);
    memset(ctrl, 0, sizeof(ctrl));
    ctrl[3] = 0x10000;
    ok &= expect_eq("zyx_rotz_rc", threedi_panm_build_node_matrices(
        &zyx, 1, &origin, NULL, NULL, &zyx_in, ThreediPanmInputs::kRestFrames, NULL, 0,
        ctrl, &zyx_out), 0);
    const float zyx_angle = (float)threedi_panm_sample_track_raw(&zyx.rotation_z, 0, ctrl) * 0.0000014980282f;
    ok &= expect_near("zyx_rotz_m0", zyx_out.m[0], cosf(zyx_angle));
    ok &= expect_near("zyx_rotz_m1", zyx_out.m[1], sinf(zyx_angle));
    ok &= expect_near("zyx_rotz_m10", zyx_out.m[10], 1.0f);
    ok &= expect_eq("zyx_rotz_turns", zyx_angle > 1.0f ? 1 : 0, 1);
    ctrl[3] = 0;
    ok &= expect_eq("zyx_rest_rc", threedi_panm_build_node_matrices(
        &zyx, 1, &origin, NULL, NULL, &zyx_in, ThreediPanmInputs::kRestFrames, NULL, 0,
        ctrl, &zyx_out), 0);
    ok &= expect_near("zyx_rest_m0", zyx_out.m[0], 1.0f);
    ok &= expect_near("zyx_rest_m1", zyx_out.m[1], 0.0f);

    return ok ? 0 : 1;
}
