#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "threedi/threedi_panm_runtime.h"

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
    ok &= expect_eq("table_rand0", table[1536], 0x6F);

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
    // operation in retail. It follows wave_lookup (low nibble 2: sine) and
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
    ok &= expect_eq("rand_band", threedi_panm_sample_track_raw(&t_rand, 0, NULL), 0x6F00);

    // Spinner mode reinterprets four bytes beginning at rotation_y.control as
    // a float coefficient; its conventional control high nibble is therefore
    // zero for 1.0f. At 250 ms, coefficient 1 turns exactly a quarter cycle.
    ThreediPartAnimation spinner;
    ThreediVec3 pivot = {0.0f, 0.0f, 0.0f};
    ThreediMatrix4x4 input, at_zero, at_quarter;
    float coefficient = 1.0f;
    memset(&spinner, 0, sizeof(spinner));
    spinner.flags = 1u << 8;
    spinner.parent_subobject = 0xff;
    memcpy(&spinner.rotation_y.control, &coefficient, sizeof(coefficient));
    threedi_mat4_identity(&input);
    ok &= expect_eq("spinner_t0_rc", threedi_panm_build_node_matrices(
        &spinner, 1, &pivot, NULL, &input, NULL, 0, NULL, &at_zero), 0);
    ok &= expect_eq("spinner_t250_rc", threedi_panm_build_node_matrices(
        &spinner, 1, &pivot, NULL, &input, NULL, 250, NULL, &at_quarter), 0);
    ok &= expect_near("spinner_t0_m0", at_zero.m[0], 1.0f);
    ok &= expect_near("spinner_t0_m1", at_zero.m[1], 0.0f);
    ok &= expect_near("spinner_t250_m0", at_quarter.m[0], 0.0f);
    ok &= expect_near("spinner_t250_m1", at_quarter.m[1], 1.0f);
    ok &= expect_near("spinner_t250_m4", at_quarter.m[4], -1.0f);
    ok &= expect_near("spinner_t250_m5", at_quarter.m[5], 0.0f);

    return ok ? 0 : 1;
}
