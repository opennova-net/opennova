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
    uint16_t ctrl[512];
    const uint8_t *table;
    ThreediTransform t_set, t_ctrl, t_sin, t_sin_small, t_rand;
    int32_t delta_sin;

    memset(ctrl, 0, sizeof(ctrl));
    ctrl[0] = 200; // ctrl register 0

    table = threedi_panm_wave_table();
    ok &= expect_eq("table_sin0", table[256], 0x7F);
    ok &= expect_eq("table_rand0", table[1536], 0x6F);

    // SET (control=24): returns start<<8
    memset(&t_set, 0, sizeof(t_set));
    t_set.control = 24; t_set.start = 10; t_set.end = 20;
    ok &= expect_eq("set", threedi_panm_sample_track_raw(&t_set, 0, NULL), 10 << 8);

    // Control-register wave (control=113): uses ctrl[2*param]
    memset(&t_ctrl, 0, sizeof(t_ctrl));
    t_ctrl.control = 113; t_ctrl.end = 10; // delta = 10
    // wave_fp8 = ctrl[0]=200; sample = (200*10)>>8 = 7
    ok &= expect_eq("ctrl", threedi_panm_sample_track_raw(&t_ctrl, 0, ctrl), 7);

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
