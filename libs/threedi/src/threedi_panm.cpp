#include "threedi/threedi_panm.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Table of known control functions. Names mirror the C# importer docs.
typedef struct ControlEntry {
    uint8_t code;
    ThreediControlFuncInfo info;
} ControlEntry;

static const ControlEntry kControlEntries[] = {
    {0, { "NONE", 0 }},
    {16, { "SLIDE", 0 }},
    {17, { "SLIDE_INVERSE", 0 }},
    {24, { "SET", 0 }},
    {32, { "ROTATE_CW", 0 }},
    {33, { "ROTATE_CCW", 0 }},
    {49, { "SET_WAVE_SQUARE", 0 }},
    {50, { "SET_WAVE_SINE", 0 }},
    {51, { "SET_WAVE_TRIANGLE", 0 }},
    {52, { "SET_WAVE_SAW", 0 }},
    {53, { "SET_WAVE_INVERSE_SAW", 0 }},
    {54, { "SET_WAVE_RANDOM", 0 }},
    {55, { "SET_WAVE_SMOOTH_RANDOM", 0 }},
    {56, { "SET_WAVE_HALF_SINE", 0 }},
    {57, { "SET_WAVE_PULSE", 0 }},
    {58, { "SET_WAVE_VIBRATE", 0 }},
    {63, { "SET_WAVE_HEARTBEAT", 0 }},
    {65, { "ADD_WAVE_SQUARE", 0 }},
    {66, { "ADD_WAVE_SINE", 0 }},
    {67, { "ADD_WAVE_TRIANGLE", 0 }},
    {68, { "ADD_WAVE_SAW", 0 }},
    {69, { "ADD_WAVE_INVERSE_SAW", 0 }},
    {70, { "ADD_WAVE_RANDOM", 0 }},
    {71, { "ADD_WAVE_SMOOTH_RANDOM", 0 }},
    {72, { "ADD_WAVE_HALF_SINE", 0 }},
    {73, { "ADD_WAVE_PULSE", 0 }},
    {74, { "ADD_WAVE_VIBRATE", 0 }},
    {79, { "ADD_WAVE_HEARTBEAT", 0 }},
    {81, { "SKEW_WAVE_SQUARE", 0 }},
    {82, { "SKEW_WAVE_SINE", 0 }},
    {83, { "SKEW_WAVE_TRIANGLE", 0 }},
    {84, { "SKEW_WAVE_SAW", 0 }},
    {85, { "SKEW_WAVE_INVERSE_SAW", 0 }},
    {86, { "SKEW_WAVE_RANDOM", 0 }},
    {87, { "SKEW_WAVE_SMOOTH_RANDOM", 0 }},
    {88, { "SKEW_WAVE_HALF_SINE", 0 }},
    {89, { "SKEW_WAVE_PULSE", 0 }},
    {90, { "SKEW_WAVE_VIBRATE", 0 }},
    {95, { "SKEW_WAVE_HEARTBEAT", 0 }},
    {97, { "MULT_WAVE_SQUARE", 0 }},
    {98, { "MULT_WAVE_SINE", 0 }},
    {99, { "MULT_WAVE_TRIANGLE", 0 }},
    {100, { "MULT_WAVE_SAW", 0 }},
    {101, { "MULT_WAVE_INVERSE_SAW", 0 }},
    {102, { "MULT_WAVE_RANDOM", 0 }},
    {103, { "MULT_WAVE_SMOOTH_RANDOM", 0 }},
    {104, { "MULT_WAVE_HALF_SINE", 0 }},
    {105, { "MULT_WAVE_PULSE", 0 }},
    {106, { "MULT_WAVE_VIBRATE", 0 }},
    {111, { "MULT_WAVE_HEARTBEAT", 0 }},
    {113, { "SET_CONTROL_REGISTER", 1 }},
    {114, { "ADD_CONTROL_REGISTER", 1 }},
    {115, { "SKEW_CONTROL_REGISTER", 1 }},
    {116, { "MULT_CONTROL_REGISTER", 1 }},
    {117, { "ROTATE_CONTROL_REGISTER", 1 }},
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

int threedi_panm_control_uses_register(uint8_t code) {
    // [orig: PANM_SampleTrack @ 0x5B2270]
    return code == 113;
}

int threedi_panm_parameter_is_ctrl_reference(uint8_t code) {
    // The model loader fixes up the parameter field before PANM dispatch. Its
    // structural threshold is broader than the one runtime value-read case.
    // [orig: ThreediGp_LoadFromFile PANM fixups @ 0x5B5E0B..0x5B5EF6]
    return code > 0x70;
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
    if (threedi_panm_parameter_is_ctrl_reference(t->control)) {
        out->ctrl_reg_name = threedi_ctrl_reg_name(ctrl, t->control_param);
    }

    out->phase = (float)t->control_param / 256.0f;
    out->rate = (float)t->rate / 256.0f;

    const float scale = out->is_rotation ? (360.0f / 16384.0f) : (1.0f / 256.0f);
    out->start = (float)t->start * scale;
    out->end = (float)t->end * scale;
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
