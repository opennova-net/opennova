// PANM/CTRL decoding and pretty-print helpers for 3DI models.
//
// These helpers do not alter parsed data; they expose control-function names,
// resolve control registers, and convert packed transform values into
// human-friendly units (degrees for rotations; /256 for others).
//
// All functions are pure and allocation-free; buffers are caller-owned.

#ifndef THREEDI_PANM_H
#define THREEDI_PANM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "threedi/threedi_3di3.h"

typedef enum ThreediTranslateAxis {
    THREEDI_TRANS_NONE = 0,
    THREEDI_TRANS_X = 1,
    THREEDI_TRANS_Y = 2,
    THREEDI_TRANS_Z = 3
} ThreediTranslateAxis;

typedef enum ThreediPanmTarget {
    THREEDI_PANM_ROT_X,
    THREEDI_PANM_ROT_Y,
    THREEDI_PANM_ROT_Z,
    THREEDI_PANM_SCALE_X,
    THREEDI_PANM_SCALE_Y,
    THREEDI_PANM_SCALE_Z,
    THREEDI_PANM_TRANS_X,
    THREEDI_PANM_TRANS_Y,
    THREEDI_PANM_TRANS_Z
} ThreediPanmTarget;

// PANM flag helpers ----------------------------------------------------------
static inline uint8_t threedi_panm_scale_type(uint32_t flags) {
    return (uint8_t)(flags & 0xFFu);
}

static inline uint8_t threedi_panm_rotation_type(uint32_t flags) {
    return (uint8_t)((flags >> 8) & 0xFFu);
}

static inline int threedi_panm_rotation_reversed(uint32_t flags) {
    return ((flags >> 16) & 0xFFu) != 0;
}

static inline uint8_t threedi_panm_translate_type(uint32_t flags) {
    return (uint8_t)((flags >> 24) & 0xFFu);
}

static inline uint32_t threedi_panm_pack_flags(uint8_t scale_type,
                                               uint8_t rotation_type,
                                               uint8_t rotation_reversed,
                                               uint8_t translate_type) {
    return (uint32_t)scale_type |
           ((uint32_t)rotation_type << 8) |
           ((uint32_t)rotation_reversed << 16) |
           ((uint32_t)translate_type << 24);
}

static inline int threedi_panm_track_present(uint32_t flags, ThreediPanmTarget target) {
    switch (target) {
        case THREEDI_PANM_ROT_X:
        case THREEDI_PANM_ROT_Y:
        case THREEDI_PANM_ROT_Z:
            return threedi_panm_rotation_type(flags) == 2;
        case THREEDI_PANM_SCALE_X: {
            uint8_t scale = threedi_panm_scale_type(flags);
            return scale == 1 || scale == 2;
        }
        case THREEDI_PANM_SCALE_Y:
        case THREEDI_PANM_SCALE_Z:
            return threedi_panm_scale_type(flags) == 2;
        case THREEDI_PANM_TRANS_X:
            return threedi_panm_translate_type(flags) == THREEDI_TRANS_X;
        case THREEDI_PANM_TRANS_Y:
            return threedi_panm_translate_type(flags) == THREEDI_TRANS_Y;
        case THREEDI_PANM_TRANS_Z:
            return threedi_panm_translate_type(flags) == THREEDI_TRANS_Z;
        default:
            return 0;
    }
}

typedef struct ThreediControlFuncInfo {
    const char *name;     // Static string for the control function (or NULL).
    int is_register_func; // 1 if this control uses a control register (0x71..0x75).
} ThreediControlFuncInfo;

// Lookup control function metadata. Returns NULL if the code is unknown.
THREEDI_EXPORT const ThreediControlFuncInfo *threedi_control_func_info(uint8_t code);

// Resolve a control register name by index. Returns NULL if out of range or missing.
THREEDI_EXPORT const char *threedi_ctrl_reg_name(const ThreediCtrl *ctrl, uint8_t idx);

typedef struct ThreediTransformDecoded {
    uint8_t control;
    const char *control_name;   // NULL if unknown.
    uint8_t control_param;
    const char *ctrl_reg_name;  // Non-NULL only for register-based controls.
    float phase;                // control_param / 256.0f
    float rate;                 // rate / 256.0f
    float start;                // degrees for rotations; /256 for others.
    float end;                  // degrees for rotations; /256 for others.
    int is_rotation;
} ThreediTransformDecoded;

// Decode a packed transform into human units and resolved names.
// Returns 0 on success, -1 on invalid args.
THREEDI_EXPORT int threedi_decode_transform(const ThreediTransform *t,
                                            int is_rotation,
                                            const ThreediCtrl *ctrl,
                                            ThreediTransformDecoded *out);

typedef struct ThreediPanmDecoded {
    const ThreediPartAnimation *raw;
    ThreediTransformDecoded rotation_x;
    ThreediTransformDecoded rotation_y;
    ThreediTransformDecoded rotation_z;
    ThreediTransformDecoded scale_x;
    ThreediTransformDecoded scale_y;
    ThreediTransformDecoded scale_z;
    ThreediTransformDecoded translation;
} ThreediPanmDecoded;

// Decode a PANM entry into human units and resolved names.
// Returns 0 on success, -1 on invalid args.
THREEDI_EXPORT int threedi_decode_panm(const ThreediPartAnimation *p,
                                       const ThreediCtrl *ctrl,
                                       ThreediPanmDecoded *out);

// Formatters for debugging/CLI output. Buffers are null-terminated.
THREEDI_EXPORT const char *threedi_translate_axis_label(uint8_t translate_type); // "none"/"X"/"Y"/"Z"/"?"
THREEDI_EXPORT void threedi_format_transform(const ThreediTransformDecoded *t, char *buf, size_t buf_sz);
THREEDI_EXPORT void threedi_format_panm(const ThreediPartAnimation *p,
                                        const ThreediCtrl *ctrl,
                                        char *buf,
                                        size_t buf_sz);

// Waveform table (matches sub_4350B0 in the original tool).
// The table has 11 contiguous 256-byte bands (0..2815). It is built deterministically
// with the MSVC rand LCG seeded to 1 and contains the precomputed waves used by
// PANM_SampleTrack.
#define THREEDI_PANM_WAVE_TABLE_SIZE 2816
THREEDI_EXPORT const uint8_t *threedi_panm_wave_table(void);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_PANM_H
