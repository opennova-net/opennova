// PANM/CTRL decoding and pretty-print helpers for 3DI models.
//
// These helpers do not alter parsed data; they expose control-function names,
// resolve control registers, and convert packed transform values into
// human-friendly units (degrees for rotations; /256 for others).
//
// All functions are pure and allocation-free; buffers are caller-owned.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::threedi {

typedef enum ThreediTranslateAxis {
    THREEDI_TRANS_NONE = 0,
    THREEDI_TRANS_X = 1,
    THREEDI_TRANS_Y = 2,
    THREEDI_TRANS_Z = 3
} ThreediTranslateAxis;

// The authorable PANM control styles by name (the raw generator-style byte;
// see threedi_control_func_info for the full catalog). 114..117 are raw
// waveform lookups selected by their low nibble — structurally valid but not
// authorable as modes.
typedef enum ThreediPanmStyle {
    THREEDI_PANM_STYLE_NONE = 0,
    THREEDI_PANM_STYLE_SLIDE = 16,
    THREEDI_PANM_STYLE_SLIDE_INVERSE = 17,
    THREEDI_PANM_STYLE_SET = 24,
    THREEDI_PANM_STYLE_ROTATE_CW = 32,
    THREEDI_PANM_STYLE_ROTATE_CCW = 33,
    THREEDI_PANM_STYLE_SINE_WAVE = 50,
    THREEDI_PANM_STYLE_SAW_WAVE = 52,
    THREEDI_PANM_STYLE_INVERSE_SAW_WAVE = 53,
    THREEDI_PANM_STYLE_CONTROL_REGISTER = 113,
    THREEDI_PANM_STYLE_WAVE_LOOKUP_FIRST = 114,
    THREEDI_PANM_STYLE_WAVE_LOOKUP_LAST = 117
} ThreediPanmStyle;

// Packed PANM track units [orig: PANM_SampleTrack @ 0x5B2270]: rotations are
// signed counts of 1/16384 turn (360/16384 degrees per count); scale,
// translation, phase and rate are signed 8.8 fixed point (1/256 per count).
inline constexpr int THREEDI_PANM_ROTATION_COUNTS_PER_TURN = 16384;
inline constexpr int THREEDI_PANM_VALUE_ONE = 256;

static inline float threedi_panm_rotation_deg_from_raw(int32_t raw) {
    return (float)raw * (360.0f / (float)THREEDI_PANM_ROTATION_COUNTS_PER_TURN);
}

static inline float threedi_panm_value_from_raw(int32_t raw) {
    return (float)raw / (float)THREEDI_PANM_VALUE_ONE;
}

// Encode counterparts (authoring): human units -> raw track counts. Rounding
// to nearest; range clamping stays with the caller (tracks are int16).
static inline double threedi_panm_rotation_raw_from_deg(double degrees) {
    return degrees * ((double)THREEDI_PANM_ROTATION_COUNTS_PER_TURN / 360.0);
}

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

// The MTRX row a part animation's frame byte selects, or 0 when it selects
// none: the loader sign-extends the byte and the pose reads the table only for
// a selector above zero, so 0 and 0x80..0xFF name no frame (and row 0 is never
// read). [orig: GPM_LoadRenderModel @ 0x5B5698 (movsx) / @ 0x5B569C (store);
//  the frame gate `<= 0` Model_TransformBoneMatrices @ 0x58E3FE]
static inline int threedi_panm_frame_selector(uint8_t matrix_index) {
    const int selector = static_cast<int8_t>(matrix_index);
    return selector > 0 ? selector : 0;
}

// The MTRX row a part animation turns through, or 0 when it reads none: its
// frame byte's selector (threedi_panm_frame_selector), and only the spinner
// and the Euler tracks (rotation types 1 and 2) turn through the row.
// [orig: the row's uses Model_TransformBoneMatrices @ 0x58E648 / @ 0x58E764
//  (spinner), @ 0x58E8AA / @ 0x58EAA7 (Euler)]
static inline int threedi_panm_frame_row(const ThreediPartAnimation &pa) {
    const uint8_t rotation = threedi_panm_rotation_type(pa.flags);
    return rotation == 1 || rotation == 2 ? threedi_panm_frame_selector(pa.matrix_index) : 0;
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

// A PANM row's seven tracks in on-disk order, and the names the `.o3d` scene
// text and the editor give them.
inline constexpr int THREEDI_PANM_TRACK_COUNT = 7;
inline const char *threedi_panm_track_label(int track) {
    static const char *const kNames[THREEDI_PANM_TRACK_COUNT] = {
            "rotx", "roty", "rotz", "scalex", "scaley", "scalez", "trans"};
    return track >= 0 && track < THREEDI_PANM_TRACK_COUNT ? kNames[track] : "?";
}
inline int threedi_panm_track_index(const std::string &label) {
    for (int i = 0; i < THREEDI_PANM_TRACK_COUNT; ++i)
        if (label == threedi_panm_track_label(i)) return i;
    return -1;
}
inline std::array<ThreediTransform *, THREEDI_PANM_TRACK_COUNT> threedi_panm_tracks(ThreediPartAnimation &pa) {
    return {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y, &pa.scale_z,
            &pa.translation};
}
inline std::array<const ThreediTransform *, THREEDI_PANM_TRACK_COUNT> threedi_panm_tracks(
        const ThreediPartAnimation &pa) {
    return {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y, &pa.scale_z,
            &pa.translation};
}

// Whether the load copies a row's track (by its index in threedi_panm_tracks) into the
// record the pose samples: the rotations for rotation type 2, the first scale for scale
// type 1 or 2 and the other two for type 2, the translation for translate types 1 to 3.
// Only a copied track has the register it names swapped for its global one.
// [orig: GPM_LoadRenderModel @ 0x5B54C7..0x5B5687 (the copies);
//  ThreediGp_LoadFromFile @ 0x5B5E08..0x5B5EF6 (the swaps)]
inline bool threedi_panm_track_loaded(const ThreediPartAnimation &pa, int track) {
    if (track == THREEDI_PANM_TRACK_COUNT - 1)
        return threedi_panm_track_present(pa.flags, THREEDI_PANM_TRANS_X) ||
               threedi_panm_track_present(pa.flags, THREEDI_PANM_TRANS_Y) ||
               threedi_panm_track_present(pa.flags, THREEDI_PANM_TRANS_Z);
    // The first six tracks are the targets ROT_X..SCALE_Z, in order.
    return track >= 0 && track < THREEDI_PANM_TRACK_COUNT - 1 &&
           threedi_panm_track_present(pa.flags, static_cast<ThreediPanmTarget>(track)) != 0;
}

typedef struct ThreediControlFuncInfo {
    const char *name;     // Static string for the control function (or NULL).
} ThreediControlFuncInfo;

// Lookup control function metadata. Returns NULL if the code is unknown.
const ThreediControlFuncInfo *threedi_control_func_info(uint8_t code);

// PANM-specific dispatch metadata. Codes 114..117 are raw waveform lookups
// selected by their low nibble, not additional register operations.
const char *threedi_panm_control_name(uint8_t code);

// Structural loader metadata, distinct from runtime dispatch. Every generator
// style above THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD (0x70), in a material,
// a PANM track or a light alike, carries a model-local CTRL index in its
// parameter byte, and the load rewrites that index to the global register its
// table entry names [orig: loader fixup ThreediGp_LoadCtrlRegisters @ 0x5B4640;
// ThreediGp_LoadFromFile @ 0x5B5C7A..0x5B5DA2 (materials), @ 0x5B5E0B..0x5B5EF6
// (PANM fixups), @ 0x5B5F4D..0x5B5F62 (lights)]. Which of those styles
// then read the register's value is the consumer's rule
// (threedi_generator_reads_register).
inline constexpr int THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD = 0x70;
bool threedi_generator_names_register(int style);

// A material generator's parameter byte (the alpha, RGB, second RGB, U and V
// generators each hold one, after the style byte), which the style splits:
// up to style 0x70 it is the phase in 1/256 of a cycle, above it the
// model-local CTRL index (threedi_generator_names_register). The parsed
// generator keeps the split as `phase` and `reg`, the unused half -1 / 0.
// threedi_generator_param_byte packs it as the writer stores it (the phase
// times 256, rounded half away from zero and clamped to 0..255; the register's
// low byte); threedi_generator_split_param_byte splits it back as the reader
// does (a phase style's byte / 256 with reg -1; a register style's byte as the
// register with phase 0).
uint8_t threedi_generator_param_byte(int style, float phase, int32_t reg);
void threedi_generator_split_param_byte(int style, uint8_t byte, float *phase, int32_t *reg);

// --- Generator-style catalog (all consumers) --------------------------------
//
// The canonical style-byte list (the kControlEntries table): every code the
// original tools emit, in canonical order. The byte is family (high nibble) +
// waveform (low nibble). One catalog for every consumer; the DISPATCH of the
// 0x71..0x75 control-register range differs per consumer (below).

// Raw codes whose 0x7X interpretation varies by retail consumer.
inline constexpr int THREEDI_STYLE_CONTROL_SET = 0x71;
inline constexpr int THREEDI_STYLE_CONTROL_ADD = 0x72;
inline constexpr int THREEDI_STYLE_CONTROL_SKEW = 0x73;
inline constexpr int THREEDI_STYLE_CONTROL_MULTIPLY = 0x74;
inline constexpr int THREEDI_STYLE_CONTROL_ROTATE = 0x75;

// The retail consumers of the generator-style byte. Their 0x71..0x75 dispatch
// differs: UV reads the CTRL value for the whole range, RGB/light for
// SET/ADD, alpha and PANM for SET only — everything else in the range falls
// back to the waveform selected by the low nibble.
// [orig: Material_ComputeUVTransformMatrix @ 0x5B1990; RgbGen_EvaluateColor
//  @ 0x5B23D0; AlphaGen_EvaluateValue @ 0x5B2320; PANM_SampleTrack @ 0x5B2270]
typedef enum ThreediGeneratorConsumer {
    THREEDI_GENERATOR_CONSUMER_UV = 0,
    THREEDI_GENERATOR_CONSUMER_RGB = 1,
    THREEDI_GENERATOR_CONSUMER_ALPHA = 2,
    THREEDI_GENERATOR_CONSUMER_LIGHT = 3,
    THREEDI_GENERATOR_CONSUMER_PANM = 4
} ThreediGeneratorConsumer;

// Whether the consumer reads the value of the register a style names; every
// other style above 0x70 takes the parameter byte as its waveform's phase. UV
// reads it for every style above 0x70, RGB and a light (a light's colour is
// an RgbGen) for 0x71 and 0x72, alpha and PANM for 0x71 alone.
// [orig: Material_ComputeUVTransformMatrix @ 0x5B1AB9 / @ 0x5B1AED (U),
//  @ 0x5B1D0C / @ 0x5B1D48 (V); RgbGen_EvaluateColor @ 0x5B244B, @ 0x5B245C;
//  Light_GetPointLightParams, its RgbGen_EvaluateColor call @ 0x5A9225;
//  AlphaGen_EvaluateValue @ 0x5B2343; PANM_SampleTrack @ 0x5B22A1]
bool threedi_generator_reads_register(ThreediGeneratorConsumer consumer, int style);

// Whether a material's flipbook reads a register: one with frames, on the
// register clock (type 1). The load swaps its time word for the global
// register under that gate alone [orig: ThreediGp_LoadFromFile
// @ 0x5B5D6E..0x5B5D99, the frames and clock gate and the word's swap], and
// the draw reads the register under the same one [orig:
// Material_ApplyShaderParameters @ 0x58DBB2..0x58DBC2 (the frames),
// @ 0x58DBF0..0x58DC13 (the clock and the read)].
bool threedi_flipbook_reads_register(const ThreediTexAnim &animation);

// Resolve a control register name by index. Returns NULL if out of range or missing.
const char *threedi_ctrl_reg_name(const ThreediCtrl *ctrl, uint8_t idx);

typedef struct ThreediTransformDecoded {
    uint8_t control;
    const char *control_name;   // NULL if unknown.
    uint8_t control_param;
    const char *ctrl_reg_name;  // CTRL name when style structurally references one.
    float phase;                // control_param / 256.0f
    float rate;                 // rate / 256.0f
    float start;                // degrees for rotations; /256 for others.
    float end;                  // degrees for rotations; /256 for others.
    int is_rotation;
} ThreediTransformDecoded;

// Decode a packed transform into human units and resolved names.
// Returns 0 on success, -1 on invalid args.
int threedi_decode_transform(const ThreediTransform *t,
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
int threedi_decode_panm(const ThreediPartAnimation *p,
                        const ThreediCtrl *ctrl,
                        ThreediPanmDecoded *out);

// Formatters for debugging/CLI output. Buffers are null-terminated.
const char *threedi_translate_axis_label(uint8_t translate_type); // "none"/"X"/"Y"/"Z"/"?"
void threedi_format_transform(const ThreediTransformDecoded *t, char *buf, size_t buf_sz);
void threedi_format_panm(const ThreediPartAnimation *p,
                         const ThreediCtrl *ctrl,
                         char *buf,
                         size_t buf_sz);

// Waveform table (matches sub_4350B0 in the original tool).
// The table has 11 contiguous 256-byte bands (0..2815). It is built deterministically
// with the MSVC rand LCG seeded to 1 and contains the precomputed waves used by
// PANM_SampleTrack.
inline constexpr int THREEDI_PANM_WAVE_TABLE_SIZE = 2816;
const uint8_t *threedi_panm_wave_table(void);

} // namespace opennova::threedi
