#include <stdio.h>
#include <string.h>

#include <formats/threedi/threedi_panm.h>

using namespace opennova::threedi;

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

static int test_raw_114_to_117_are_waves_with_ctrl_reference_params(void) {
    static const char *expected[] = {
        "WAVE_SINE_RAW_114",
        "WAVE_TRIANGLE_RAW_115",
        "WAVE_SAW_RAW_116",
        "WAVE_INVERSE_SAW_RAW_117",
    };
    ThreediControlRegister reg;
    ThreediCtrl ctrl;
    memset(&reg, 0, sizeof(reg));
    strncpy(reg.name, "STRUCTURAL_CTRL_REF", sizeof(reg.name) - 1);
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.count = 1;
    ctrl.record_size = 24;
    ctrl.registers = &reg;

    for (uint8_t code = 114; code <= 117; ++code) {
        ThreediTransform transform;
        ThreediTransformDecoded decoded;
        memset(&transform, 0, sizeof(transform));
        memset(&decoded, 0, sizeof(decoded));
        transform.control = code;
        transform.control_param = 0;
        if (threedi_decode_transform(
                    &transform, 0, &ctrl, &decoded) != 0) {
            fprintf(stderr, "decode_transform failed for raw code %u\n", code);
            return 0;
        }
        if (decoded.ctrl_reg_name == NULL ||
                strcmp(decoded.ctrl_reg_name, "STRUCTURAL_CTRL_REF") != 0 ||
                decoded.control_name == NULL ||
                strcmp(decoded.control_name, expected[code - 114]) != 0 ||
                threedi_generator_reads_register(THREEDI_GENERATOR_CONSUMER_PANM, code) ||
                !threedi_generator_names_register(code)) {
            fprintf(stderr,
                    "raw PANM code %u lost the structural/reference split\n",
                    code);
            return 0;
        }
    }
    return 1;
}

// Every style above 0x70 names a register for every consumer (the load swaps
// it); which of them read the register's value is the consumer's rule: UV all
// of them, RGB and a light 0x71 and 0x72, alpha and PANM 0x71 alone.
static int test_generator_register_rules(void) {
    static const int kStyles[] = {0, 24, 50, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x80, 0xFF};
    static const struct {
        ThreediGeneratorConsumer consumer;
        const char *name;
        int reads[13]; // by kStyles
    } kRows[] = {
        {THREEDI_GENERATOR_CONSUMER_UV, "uv", {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}},
        {THREEDI_GENERATOR_CONSUMER_RGB, "rgb", {0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0}},
        {THREEDI_GENERATOR_CONSUMER_LIGHT, "light", {0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0}},
        {THREEDI_GENERATOR_CONSUMER_ALPHA, "alpha", {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}},
        {THREEDI_GENERATOR_CONSUMER_PANM, "panm", {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}},
    };
    int ok = 1;
    for (size_t s = 0; s < sizeof(kStyles) / sizeof(kStyles[0]); ++s) {
        const int style = kStyles[s];
        if (threedi_generator_names_register(style) != (style > 0x70)) {
            fprintf(stderr, "style 0x%02X names a register only above 0x70\n", style);
            ok = 0;
        }
        for (const auto &row : kRows) {
            if (threedi_generator_reads_register(row.consumer, style) != (row.reads[s] != 0)) {
                fprintf(stderr, "%s style 0x%02X: reads the register %s\n", row.name, style,
                        row.reads[s] ? "(expected)" : "(not expected)");
                ok = 0;
            }
        }
    }
    return ok;
}

// A flipbook reads a register only with frames on the register clock (type 1).
static int test_flipbook_register_rule(void) {
    static const struct {
        uint8_t frames, type;
        bool reads;
    } kRows[] = {{0, 0, false}, {0, 1, false}, {1, 0, false}, {4, 0, false}, {1, 1, true}, {4, 1, true}};
    int ok = 1;
    for (const auto &row : kRows) {
        ThreediTexAnim animation;
        memset(&animation, 0, sizeof(animation));
        animation.num_frames = row.frames;
        animation.animation_type = row.type;
        animation.cycle_frame_time = 5;
        if (threedi_flipbook_reads_register(animation) != row.reads) {
            fprintf(stderr, "flipbook frames %u type %u: reads a register %s\n", row.frames, row.type,
                    row.reads ? "(expected)" : "(not expected)");
            ok = 0;
        }
    }
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= test_decode_transform_with_reg();
    ok &= test_decode_panm_wave_no_reg();
    ok &= test_raw_114_to_117_are_waves_with_ctrl_reference_params();
    ok &= test_generator_register_rules();
    ok &= test_flipbook_register_rule();
    return ok ? 0 : 1;
}
