// BHD GP shader_type catalog, RE'd from df4oed.exe (BHD's GP OED).
//
// BHD materials carry a shader_type integer (low byte of GP material's
// shader_flags) that selects from a fixed 15-entry "Advanced Features"
// catalog in df4oed. This header exposes the catalog as a table so
// classify/synthesize code (libs/threedi/src/threedi_material_class.c)
// can map BHD shader_type ↔ ThreediMaterialClass losslessly without
// inline heuristics.
//
// Source of truth:
//   df4oed.exe::sub_40A7C0 @ 0x40a7c0:
//     SetDlgItemTextA(hDlg, 1194, &aNoAdvancedShad[256 * shader_type])

#ifndef THREEDI_BHD_SHADER_H
#define THREEDI_BHD_SHADER_H

#include <stdint.h>

#include "threedi/threedi_material_class.h"

#ifdef __cplusplus
extern "C" {
#endif

// BHD GP shader_type values, indexed exactly as df4oed.exe's
// description-array lookup.
typedef enum BhdShaderType {
    BHD_SHADER_NONE                    = 0,   // No advanced shading
    BHD_SHADER_DIFFUSE_BUMP            = 1,   // Diffuse Bump Map (alpha-source)
    BHD_SHADER_PHONG_BUMP              = 2,   // Phong Bump Map (alpha-source)
    BHD_SHADER_DIFFSPEC_BUMP           = 3,   // Diffuse+Specular Bump (white spec)
    BHD_SHADER_DIFFSPEC_ENV_BUMP       = 4,   // Diff/Spec/Glossmap Bump
    BHD_SHADER_CHROME_BUMP             = 5,   // Chrome Bump (env-only)
    BHD_SHADER_ENV_OVERLAY_BUMP        = 6,   // Diffuse and specular env w/ overlay
    BHD_SHADER_PHONG_NO_BUMP           = 7,   // Phong lighting without bumpmap
    BHD_SHADER_MDT_DIFFUSE_BUMP        = 8,   // MDT Diffuse Bump Map
    BHD_SHADER_MDT_PHONG_BUMP          = 9,   // MDT Phong Bump Map
    BHD_SHADER_MDT_PHONG_BUMP_V2       = 10,  // MDT Phong Variable Specular V2
    BHD_SHADER_MDT_PHONG_BUMP_V2_RT    = 11,  // V2 + Object Render Target
    BHD_SHADER_DIFFUSE_BUMP_DETAIL     = 12,  // Diffuse Bump + Stage2 detail
    BHD_SHADER_SKIN_DIFFUSE_BUMP       = 13,  // Skin variant
    BHD_SHADER_REFLECTIVE_ENV_OVERLAY  = 14,  // Reflective env with overlay
} BhdShaderType;

// Lookup result for a BHD shader_type.  All feature columns are populated
// from df4oed UI labels + IDA-verified shader behavior so that a reader
// can produce a complete `ThreediMaterialClass` without heuristics.
typedef struct {
    const char *name;          // df4oed UI label
    const char *base_jo_tag;   // Closest JO shader_tag (transition-period helper)
    uint8_t family;            // ThreediShaderFamily
    uint8_t bump_mode;         // ThreediBumpMode
    uint8_t specular_mode;     // ThreediSpecularMode
    uint8_t has_detail;        // 0/1
    uint8_t has_overlay;       // 0/1 — shader_type 6/14
    uint8_t is_skinned;        // 0/1 — shader_type 13
    uint8_t is_glass;          // 0/1 — env-mapped/mirror
    uint8_t is_mirror_only;    // 0/1 — chrome (no diffuse spec) — shader_type 5/14
} BhdShaderInfo;

// Look up the table entry for a BHD shader_type integer.  Out-of-range
// values get a zeroed struct (caller treats as BHD_SHADER_NONE behavior).
BhdShaderInfo bhd_shader_lookup(uint32_t shader_type);

// Number of rows in the catalog (= 15).
uint32_t bhd_shader_table_count(void);

// Direct table access (read-only) for synthesis routines that linear-scan
// for a row whose features match an material classification.
const BhdShaderInfo *bhd_shader_table(void);

#ifdef __cplusplus
}
#endif

#endif  // THREEDI_BHD_SHADER_H
