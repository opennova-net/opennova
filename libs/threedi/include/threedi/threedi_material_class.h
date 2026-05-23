// Format-agnostic material classification for the material model.
//
// Both BHD (GP/3DA) and JO (3DI3/3DP) carry the same conceptual feature set
// (shader family, bump mode, specular mode, blend, alpha-test, etc.) but
// encode it differently: BHD uses a `shader_type` integer (0..14) plus a
// `render_attributes` bitfield; JO uses a `shader_tag` string from a 45-row
// catalog plus suffix decorations.  This header defines the canonical
// representation and the four classify/synthesize entry points.

#ifndef THREEDI_MATERIAL_CLASS_H
#define THREEDI_MATERIAL_CLASS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shader rendering family — informs vertex/pixel shader selection.
typedef enum ThreediShaderFamily {
    THREEDI_FAMILY_FF    = 0,  // Fixed-function (FF_ST_*, FF_MT_*, BHD shader_type 0/7)
    THREEDI_FAMILY_PHONG = 1,  // VS_PHONGT*, BHD shader_type 2/3/9/10/11
    THREEDI_FAMILY_DOT3  = 2,  // VS_DOT3DIFF*, BHD shader_type 1/8/12
    THREEDI_FAMILY_ENV   = 3,  // VS_ENVPHONGT, BHD shader_type 4
    THREEDI_FAMILY_GLASS = 4,  // FFP_GLASS, VS_BMTXMIRRT/BUMPMIRRT, BHD 5/6/14
    THREEDI_FAMILY_FLAG  = 5,  // VS_FLAG / wind-animated foliage
} ThreediShaderFamily;

// Bump-map sourcing mode.
typedef enum ThreediBumpMode {
    THREEDI_BUMP_NONE         = 0,
    THREEDI_BUMP_ALPHA_SOURCE = 1,  // Stage1.alpha = bump (BHD 1-6,12,14; JO VS_*BUMP*T)
    THREEDI_BUMP_MDT          = 2,  // BHD 8-11; JO VS_PHONGT_MDT
} ThreediBumpMode;

// Specular shading variant.
typedef enum ThreediSpecularMode {
    THREEDI_SPEC_NONE           = 0,
    THREEDI_SPEC_FIXED_PHONG    = 1,  // BHD shader_type 2/9; JO VS_PHONGT
    THREEDI_SPEC_WHITE_ONLY     = 2,  // BHD shader_type 3
    THREEDI_SPEC_VARIABLE       = 3,  // Variable phong V1
    THREEDI_SPEC_VARIABLE_V2    = 4,  // BHD shader_type 10
    THREEDI_SPEC_VARIABLE_V2_RT = 5,  // BHD shader_type 11
    THREEDI_SPEC_ENV_LIT        = 6,  // BHD shader_type 4; JO VS_ENVPHONGT
    THREEDI_SPEC_MIRROR_ONLY    = 7,  // BHD shader_type 5/14
} ThreediSpecularMode;

// Blend mode for the material.
typedef enum ThreediClassBlendMode {
    THREEDI_CLASS_BLEND_OPAQUE   = 0,
    THREEDI_CLASS_BLEND_ALPHA    = 1,  // _AB
    THREEDI_CLASS_BLEND_ADDITIVE = 2,  // _AD
} ThreediClassBlendMode;

// Format-agnostic feature set.  Carries enough discrimination that no two
// functionally-distinct shader entries (in BHD's 15-row catalog or JO's
// 45-row catalog) collide in this representation.  Genuinely-equivalent
// entries (e.g. BHD shader_type 0 vs 7, both pure FF) collapse — that's
// fine, the writer picks the lowest-index match.
typedef struct ThreediMaterialClass {
    uint8_t family;             // ThreediShaderFamily
    uint8_t bump_mode;          // ThreediBumpMode
    uint8_t specular_mode;      // ThreediSpecularMode
    uint8_t has_detail;         // 0/1 — Stage2 / second diffuse texture
    uint8_t has_overlay;        // 0/1 — BHD shader_type 6/14 extra overlay stage
    uint8_t is_skinned;         // 0/1 — VS_SK*, BHD shader_type 13
    uint8_t is_glass;           // 0/1 — FFP_GLASS / VS_*GLASS
    uint8_t is_flag;            // 0/1 — VS_FLAG family
    uint8_t luminance;          // 0/1 — _LUM suffix
    uint8_t uv_animated;        // 0/1 — #UV suffix
    uint8_t blend_mode;         // ThreediClassBlendMode
    uint8_t two_sided;          // 0/1
    uint8_t alpha_test;         // 0/1
    uint8_t alpha_test_invert;  // 0/1
} ThreediMaterialClass;

// ----------------------------------------------------------------------------
// Classify (format -> class)
// ----------------------------------------------------------------------------

// Populate `out` from a JO shader_tag string + 3DI3 binary's per-material
// flags.  Looks up the tag in `kMaterialInfoTable`, scans `_AB`/`_AD`/`_LUM`/
// `#UV`/`2`-detail suffixes, OR's in render_attributes-derived flags.
void classify_from_jo_shader_tag(const char *shader_tag,
                                  uint32_t rattrib,
                                  uint8_t material_flags,
                                  uint8_t emissive_type,
                                  uint8_t is_glass_byte,
                                  ThreediMaterialClass *out);

// Populate `out` from BHD's per-material `shader_type` integer + the
// `render_attributes` bitfield + a few related flags.
void classify_from_bhd_shader_type(uint32_t shader_type,
                                    uint32_t render_attributes,
                                    uint8_t use_alpha_pcx,
                                    uint32_t blending_mode,
                                    uint32_t alpha_type,
                                    ThreediMaterialClass *out);

// ----------------------------------------------------------------------------
// Synthesize (class -> format)
// ----------------------------------------------------------------------------

// Reverse-lookup against `kMaterialInfoTable` + apply suffix decoration.
// `out` must have at least 33 bytes.
void synthesize_jo_shader_tag(const ThreediMaterialClass *cls,
                               char *out, size_t out_size);

// Reverse-lookup against the extended `kBhdShaderTable`.  Returns the
// lowest-index row whose feature columns match `cls`; falls back to 0
// (BHD_SHADER_NONE) if no match.
uint32_t synthesize_bhd_shader_type(const ThreediMaterialClass *cls);

// Build BHD's `render_attributes` bitfield from classification.
//   bit 0x0001 = two_sided
//   bit 0x0100 = animated (set by caller via has_animation arg)
//   bit 0x0400 = alpha_test
//   bit 0x1000 = alpha_test_invert
uint32_t synthesize_render_attributes(const ThreediMaterialClass *cls,
                                       int has_animation);

// Build JO's `rattrib` bitfield from classification.  Same shared bits as
// BHD's render_attributes.
uint32_t synthesize_jo_rattrib(const ThreediMaterialClass *cls,
                                int has_animation);

#ifdef __cplusplus
}
#endif

#endif  // THREEDI_MATERIAL_CLASS_H
