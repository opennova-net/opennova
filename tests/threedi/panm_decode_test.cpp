#include <stdio.h>
#include <string.h>

#include "threedi/threedi_panm.h"

static int expect_eq(const char *label, const char *got, const char *expected) {
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "%s mismatch:\n got: %s\n exp: %s\n", label, got, expected);
        return 0;
    }
    return 1;
}

static int test_decode_transform_with_reg(void) {
    ThreediControlRegister regs[2];
    ThreediCtrl ctrl;
    ThreediTransform t;
    ThreediTransformDecoded dec;
    char buf[256];

    memset(regs, 0, sizeof(regs));
    strncpy(regs[0].name, "REG0", sizeof(regs[0].name) - 1);
    strncpy(regs[1].name, "REG1", sizeof(regs[1].name) - 1);

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.count = 2;
    ctrl.record_size = 24;
    ctrl.registers = regs;

    memset(&t, 0, sizeof(t));
    t.control = 113;        // SET_CONTROL_REGISTER
    t.control_param = 1;    // REG1
    t.rate = 0;
    t.start = 0;
    t.end = 16384;          // full rotation

    memset(&dec, 0, sizeof(dec));
    if (threedi_decode_transform(&t, 1, &ctrl, &dec) != 0) {
        fprintf(stderr, "decode_transform failed\n");
        return 0;
    }

    threedi_format_transform(&dec, buf, sizeof(buf));
    return expect_eq("transform_with_reg", buf,
        "SET_CONTROL_REGISTER reg=REG1(1) phase=0.004 rate=0.000 start=0.000\xc2\xb0 end=360.000\xc2\xb0");
}

static int test_decode_panm_wave_no_reg(void) {
    ThreediPartAnimation p;
    ThreediPanmDecoded dec;
    char buf[512];

    memset(&p, 0, sizeof(p));
    p.flags = threedi_panm_pack_flags(0, 2, 0, THREEDI_TRANS_Y);
    p.parent_subobject = 0;
    p.subobject_index = 1;
    p.matrix_index = 0;

    p.rotation_z.control = 50;   // SET_WAVE_SINE
    p.rotation_z.control_param = 0;
    p.rotation_z.rate = 128;     // 0.5
    p.rotation_z.start = -136;   // approx -3.0 deg
    p.rotation_z.end = 8192;     // 180 deg

    p.translation.control = 50;  // SET_WAVE_SINE
    p.translation.control_param = 0;
    p.translation.rate = 128;    // 0.5
    p.translation.start = 256;   // 1.0
    p.translation.end = -256;    // -1.0

    memset(&dec, 0, sizeof(dec));
    if (threedi_decode_panm(&p, NULL, &dec) != 0) {
        fprintf(stderr, "decode_panm failed\n");
        return 0;
    }

    threedi_format_panm(&p, NULL, buf, sizeof(buf));
    return expect_eq("panm_wave", buf,
        "parent=0 subobj=1 matrix=0 rot_type=2 scale_type=0 trans=Y\n"
        "  rot_z: SET_WAVE_SINE phase=0.000 rate=0.500 start=-2.988\xc2\xb0 end=180.000\xc2\xb0\n"
        "  translation(Y): SET_WAVE_SINE phase=0.000 rate=0.500 start=1.000 end=-1.000");
}

int main(void) {
    int ok = 1;
    ok &= test_decode_transform_with_reg();
    ok &= test_decode_panm_wave_no_reg();
    return ok ? 0 : 1;
}
