#include <formats/threedi/threedi_panm.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace opennova::threedi {

// Table of known control functions from the witnessed 3DI catalog.
typedef struct ControlEntry {
    uint8_t code;
    ThreediControlFuncInfo info;
} ControlEntry;

static const ControlEntry kControlEntries[] = {
    {0, { "NONE" }},
    {16, { "SLIDE" }},
    {17, { "SLIDE_INVERSE" }},
    {24, { "SET" }},
    {32, { "ROTATE_CW" }},
    {33, { "ROTATE_CCW" }},
    {49, { "SET_WAVE_SQUARE" }},
    {50, { "SET_WAVE_SINE" }},
    {51, { "SET_WAVE_TRIANGLE" }},
    {52, { "SET_WAVE_SAW" }},
    {53, { "SET_WAVE_INVERSE_SAW" }},
    {54, { "SET_WAVE_RANDOM" }},
    {55, { "SET_WAVE_SMOOTH_RANDOM" }},
    {56, { "SET_WAVE_HALF_SINE" }},
    {57, { "SET_WAVE_PULSE" }},
    {58, { "SET_WAVE_VIBRATE" }},
    {63, { "SET_WAVE_HEARTBEAT" }},
    {65, { "ADD_WAVE_SQUARE" }},
    {66, { "ADD_WAVE_SINE" }},
    {67, { "ADD_WAVE_TRIANGLE" }},
    {68, { "ADD_WAVE_SAW" }},
    {69, { "ADD_WAVE_INVERSE_SAW" }},
    {70, { "ADD_WAVE_RANDOM" }},
    {71, { "ADD_WAVE_SMOOTH_RANDOM" }},
    {72, { "ADD_WAVE_HALF_SINE" }},
    {73, { "ADD_WAVE_PULSE" }},
    {74, { "ADD_WAVE_VIBRATE" }},
    {79, { "ADD_WAVE_HEARTBEAT" }},
    {81, { "SKEW_WAVE_SQUARE" }},
    {82, { "SKEW_WAVE_SINE" }},
    {83, { "SKEW_WAVE_TRIANGLE" }},
    {84, { "SKEW_WAVE_SAW" }},
    {85, { "SKEW_WAVE_INVERSE_SAW" }},
    {86, { "SKEW_WAVE_RANDOM" }},
    {87, { "SKEW_WAVE_SMOOTH_RANDOM" }},
    {88, { "SKEW_WAVE_HALF_SINE" }},
    {89, { "SKEW_WAVE_PULSE" }},
    {90, { "SKEW_WAVE_VIBRATE" }},
    {95, { "SKEW_WAVE_HEARTBEAT" }},
    {97, { "MULT_WAVE_SQUARE" }},
    {98, { "MULT_WAVE_SINE" }},
    {99, { "MULT_WAVE_TRIANGLE" }},
    {100, { "MULT_WAVE_SAW" }},
    {101, { "MULT_WAVE_INVERSE_SAW" }},
    {102, { "MULT_WAVE_RANDOM" }},
    {103, { "MULT_WAVE_SMOOTH_RANDOM" }},
    {104, { "MULT_WAVE_HALF_SINE" }},
    {105, { "MULT_WAVE_PULSE" }},
    {106, { "MULT_WAVE_VIBRATE" }},
    {111, { "MULT_WAVE_HEARTBEAT" }},
    {113, { "SET_CONTROL_REGISTER" }},
    {114, { "ADD_CONTROL_REGISTER" }},
    {115, { "SKEW_CONTROL_REGISTER" }},
    {116, { "MULT_CONTROL_REGISTER" }},
    {117, { "ROTATE_CONTROL_REGISTER" }},
};

const ThreediControlFuncInfo *threedi_control_func_info(uint8_t code) {
    for (size_t i = 0; i < sizeof(kControlEntries) / sizeof(kControlEntries[0]); ++i) {
        if (kControlEntries[i].code == code) {
            return &kControlEntries[i].info;
        }
    }
    return NULL;
}

const char *threedi_panm_control_name(uint8_t code) {
    switch (code) {
        case 114: return "WAVE_SINE_RAW_114";
        case 115: return "WAVE_TRIANGLE_RAW_115";
        case 116: return "WAVE_SAW_RAW_116";
        case 117: return "WAVE_INVERSE_SAW_RAW_117";
        default: {
            const ThreediControlFuncInfo *info =
                    threedi_control_func_info(code);
            return info ? info->name : NULL;
        }
    }
}

bool threedi_generator_names_register(int style) {
    // [orig: ThreediGp_LoadFromFile PANM fixups @ 0x5B5E0B..0x5B5EF6]
    return style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD;
}

uint8_t threedi_generator_param_byte(int style, float phase, int32_t reg) {
    if (threedi_generator_names_register(style)) {
        return (uint8_t)reg;
    }
    // The phase in 1/256, rounded half away from zero and clamped to a byte
    // (the writer's byte rule for every scaled byte it stores).
    const double scaled = (double)phase * 256.0;
    long v = (long)(scaled >= 0.0 ? (int64_t)(scaled + 0.5) : (int64_t)(scaled - 0.5));
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

void threedi_generator_split_param_byte(int style, uint8_t byte, float *phase, int32_t *reg) {
    if (!threedi_generator_names_register(style)) {
        *phase = (float)byte / 256.0f;
        *reg = -1;
    } else {
        *reg = byte;
        *phase = 0.0f;
    }
}

bool threedi_generator_reads_register(ThreediGeneratorConsumer consumer, int style) {
    // [orig: Material_ComputeUVTransformMatrix @ 0x5B1990; RgbGen_EvaluateColor
    //  @ 0x5B23D0; AlphaGen_EvaluateValue @ 0x5B2320; PANM_SampleTrack @ 0x5B2270]
    switch (consumer) {
        case THREEDI_GENERATOR_CONSUMER_UV:
            return threedi_generator_names_register(style);
        case THREEDI_GENERATOR_CONSUMER_RGB:
        case THREEDI_GENERATOR_CONSUMER_LIGHT:
            return style == THREEDI_STYLE_CONTROL_SET || style == THREEDI_STYLE_CONTROL_ADD;
        case THREEDI_GENERATOR_CONSUMER_ALPHA:
        case THREEDI_GENERATOR_CONSUMER_PANM:
            return style == THREEDI_STYLE_CONTROL_SET;
    }
    return false;
}

bool threedi_flipbook_reads_register(const ThreediTexAnim &animation) {
    // [orig: ThreediGp_LoadFromFile @ 0x5B5D6E..0x5B5D99;
    //  Material_ApplyShaderParameters @ 0x58DBB2..0x58DBC2, @ 0x58DBF0..0x58DC13]
    return animation.num_frames != 0 && animation.animation_type == 1;
}

const char *threedi_ctrl_reg_name(const ThreediCtrl *ctrl, uint8_t idx) {
    if (!ctrl || !ctrl->registers || idx >= ctrl->count) {
        return NULL;
    }
    return ctrl->registers[idx].name;
}

const char *threedi_translate_axis_label(uint8_t translate_type) {
    switch (translate_type) {
        case THREEDI_TRANS_X: return "X";
        case THREEDI_TRANS_Y: return "Y";
        case THREEDI_TRANS_Z: return "Z";
        case THREEDI_TRANS_NONE: return "none";
        default: return "?";
    }
}

static void zero_decode(ThreediTransformDecoded *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

int threedi_decode_transform(const ThreediTransform *t,
                             int is_rotation,
                             const ThreediCtrl *ctrl,
                             ThreediTransformDecoded *out) {
    if (!t || !out) {
        return -1;
    }
    zero_decode(out);
    out->control = t->control;
    out->control_param = t->control_param;
    out->is_rotation = is_rotation ? 1 : 0;

    out->control_name = threedi_panm_control_name(t->control);
    if (threedi_generator_names_register(t->control)) {
        out->ctrl_reg_name = threedi_ctrl_reg_name(ctrl, t->control_param);
    }

    out->phase = threedi_panm_value_from_raw(t->control_param);
    out->rate = threedi_panm_value_from_raw(t->rate);

    if (out->is_rotation) {
        out->start = threedi_panm_rotation_deg_from_raw(t->start);
        out->end = threedi_panm_rotation_deg_from_raw(t->end);
    } else {
        out->start = threedi_panm_value_from_raw(t->start);
        out->end = threedi_panm_value_from_raw(t->end);
    }
    return 0;
}

static void decode_if_present(const ThreediTransform *src,
                              int is_rotation,
                              const ThreediCtrl *ctrl,
                              int has_flag,
                              ThreediTransformDecoded *out) {
    if (!out) return;
    zero_decode(out);
    if (has_flag && src) {
        (void)threedi_decode_transform(src, is_rotation, ctrl, out);
    }
}

int threedi_decode_panm(const ThreediPartAnimation *p,
                        const ThreediCtrl *ctrl,
                        ThreediPanmDecoded *out) {
    if (!p || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->raw = p;

    const uint32_t flags = p->flags;
    const uint8_t scale_type = threedi_panm_scale_type(flags);
    const uint8_t rot_type = threedi_panm_rotation_type(flags);
    const uint8_t trans_type = threedi_panm_translate_type(flags);

    decode_if_present(&p->rotation_x, 1, ctrl, rot_type == 2, &out->rotation_x);
    decode_if_present(&p->rotation_y, 1, ctrl, rot_type == 2, &out->rotation_y);
    decode_if_present(&p->rotation_z, 1, ctrl, rot_type == 2, &out->rotation_z);
    decode_if_present(&p->scale_x, 0, ctrl, scale_type == 1 || scale_type == 2, &out->scale_x);
    decode_if_present(&p->scale_y, 0, ctrl, scale_type == 2, &out->scale_y);
    decode_if_present(&p->scale_z, 0, ctrl, scale_type == 2, &out->scale_z);
    decode_if_present(&p->translation, 0, ctrl, trans_type > THREEDI_TRANS_NONE, &out->translation);
    return 0;
}

void threedi_format_transform(const ThreediTransformDecoded *t, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!t) return;

    const char *name = t->control_name ? t->control_name : "UNKNOWN";
    const int is_none = (t->control == 0);

    if (is_none) {
        snprintf(buf, buf_sz, "NONE");
        return;
    }

    if (t->ctrl_reg_name) {
        snprintf(buf, buf_sz,
                 "%s reg=%s(%u) phase=%.3f rate=%.3f start=%.3f%s end=%.3f%s",
                 name,
                 t->ctrl_reg_name,
                 (unsigned)t->control_param,
                 t->phase,
                 t->rate,
                 t->start,
                 t->is_rotation ? "°" : "",
                 t->end,
                 t->is_rotation ? "°" : "");
    } else {
        snprintf(buf, buf_sz,
                 "%s phase=%.3f rate=%.3f start=%.3f%s end=%.3f%s",
                 name,
                 t->phase,
                 t->rate,
                 t->start,
                 t->is_rotation ? "°" : "",
                 t->end,
                 t->is_rotation ? "°" : "");
    }
}

void threedi_format_panm(const ThreediPartAnimation *p,
                         const ThreediCtrl *ctrl,
                         char *buf,
                         size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!p) return;

    char tmp[256];
    ThreediPanmDecoded dec;
    memset(&dec, 0, sizeof(dec));
    if (threedi_decode_panm(p, ctrl, &dec) != 0) {
        return;
    }

    // Header
    const uint32_t flags = p->flags;
    const uint8_t scale_type = threedi_panm_scale_type(flags);
    const uint8_t rot_type = threedi_panm_rotation_type(flags);
    const int rot_rev = threedi_panm_rotation_reversed(flags);
    const uint8_t trans_type = threedi_panm_translate_type(flags);
    const char *trans_label = threedi_translate_axis_label(trans_type);
    int written = snprintf(buf, buf_sz,
                           "parent=%u subobj=%u matrix=%u rot_type=%u%s scale_type=%u trans=%s",
                           p->parent_subobject,
                           p->subobject_index,
                           p->matrix_index,
                           rot_type,
                           rot_rev ? " (rot_rev=1)" : "",
                           scale_type,
                           trans_label);
    if (written < 0 || (size_t)written >= buf_sz) return;

    size_t cursor = (size_t)written;
    // Append active transforms
    if (threedi_panm_track_present(flags, THREEDI_PANM_ROT_X) && dec.rotation_x.control != 0) {
        threedi_format_transform(&dec.rotation_x, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  rot_x: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (threedi_panm_track_present(flags, THREEDI_PANM_ROT_Y) && dec.rotation_y.control != 0) {
        threedi_format_transform(&dec.rotation_y, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  rot_y: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (threedi_panm_track_present(flags, THREEDI_PANM_ROT_Z) && dec.rotation_z.control != 0) {
        threedi_format_transform(&dec.rotation_z, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  rot_z: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (threedi_panm_track_present(flags, THREEDI_PANM_SCALE_X) && dec.scale_x.control != 0) {
        threedi_format_transform(&dec.scale_x, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  scale_x: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (threedi_panm_track_present(flags, THREEDI_PANM_SCALE_Y) && dec.scale_y.control != 0) {
        threedi_format_transform(&dec.scale_y, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  scale_y: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (threedi_panm_track_present(flags, THREEDI_PANM_SCALE_Z) && dec.scale_z.control != 0) {
        threedi_format_transform(&dec.scale_z, tmp, sizeof(tmp));
        int n = snprintf(buf + cursor, buf_sz - cursor, "\n  scale_z: %s", tmp);
        if (n > 0) cursor += (size_t)n;
    }
    if (trans_type != THREEDI_TRANS_NONE && dec.translation.control != 0) {
        threedi_format_transform(&dec.translation, tmp, sizeof(tmp));
        const char *label = "translation";
        if (trans_type == THREEDI_TRANS_X) label = "translation(X)";
        else if (trans_type == THREEDI_TRANS_Y) label = "translation(Y)";
        else if (trans_type == THREEDI_TRANS_Z) label = "translation(Z)";
        (void)snprintf(buf + cursor, buf_sz - cursor, "\n  %s: %s", label, tmp);
    }
}

} // namespace opennova::threedi
