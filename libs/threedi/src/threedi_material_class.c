// Format-agnostic material classification — implementation.
//
// See libs/threedi/include/threedi/threedi_material_class.h.

#include "threedi/threedi_material_class.h"
#include "threedi/threedi_bhd_shader.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

// 3DI3 binary `material_flags` byte values (from threedi_3di3.h).  We
// re-declare them here to keep this file C-only and not require pulling in
// the whole 3di3 header.
#define MAT_FLAG_ALPHA_TEST   0x01u
#define MAT_FLAG_ALPHA_INVERT 0x02u
#define MAT_FLAG_TWO_SIDED    0x04u

// JO shader_tag prefix detection.
static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Scan JO tag for a substring (used for SK / DOT3 / PHONG / FLAG / GLASS /
// MDT / MIRR / ENV markers).
static int contains(const char *s, const char *needle) {
    return strstr(s, needle) != NULL;
}

// Strip suffix from JO tag if present.  Returns 1 if stripped, 0 otherwise.
// Modifies `tag` in place.
static int strip_suffix(char *tag, const char *suffix) {
    size_t tag_len = strlen(tag);
    size_t sfx_len = strlen(suffix);
    if (tag_len < sfx_len) return 0;
    if (strcmp(tag + tag_len - sfx_len, suffix) != 0) return 0;
    tag[tag_len - sfx_len] = '\0';
    return 1;
}

// ---------------------------------------------------------------------------
// classify_from_jo_shader_tag
// ---------------------------------------------------------------------------

void classify_from_jo_shader_tag(const char *shader_tag,
                                  uint32_t rattrib,
                                  uint8_t material_flags,
                                  uint8_t emissive_type,
                                  uint8_t is_glass_byte,
                                  ThreediMaterialClass *out) {
    memset(out, 0, sizeof(*out));

    // Make a working copy of the tag so we can strip suffixes.
    char tag[64];
    if (!shader_tag) shader_tag = "";
    size_t taglen = strlen(shader_tag);
    if (taglen >= sizeof(tag)) taglen = sizeof(tag) - 1;
    memcpy(tag, shader_tag, taglen);
    tag[taglen] = '\0';

    // Strip #UV first (it always comes last).
    if (strip_suffix(tag, "#UV")) out->uv_animated = 1;
    // Then _LUM (before blend mode? no — _LUM follows blend per JO catalog:
    // FF_ST_AB_LUM, FF_MT_AD_LUM_UV → FF_MT_AD_LUM after #UV strip).  So
    // strip _LUM after #UV but before blend suffixes.
    if (strip_suffix(tag, "_LUM")) out->luminance = 1;
    // Blend suffix: _AB or _AD (mutually exclusive in catalog).
    if (strip_suffix(tag, "_AB")) {
        out->blend_mode = THREEDI_CLASS_BLEND_ALPHA;
    } else if (strip_suffix(tag, "_AD")) {
        out->blend_mode = THREEDI_CLASS_BLEND_ADDITIVE;
    } else {
        out->blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
    }

    // After suffix stripping, `tag` is the base shader tag.  Classify by
    // prefix + token containment.  Family detection mirrors
    // worktree-objects:libs/renderer/src/material_classify.cpp.
    if (strcmp(tag, "FFP_GLASS") == 0) {
        out->family    = THREEDI_FAMILY_GLASS;
        out->is_glass  = 1;
    } else if (strcmp(tag, "VS_FLAG") == 0 || contains(tag, "FLAG")) {
        out->family = THREEDI_FAMILY_FLAG;
        out->is_flag = 1;
    } else if (strcmp(tag, "VS_BMTXMIRRT") == 0) {
        out->family       = THREEDI_FAMILY_GLASS;
        out->is_glass     = 1;
        out->bump_mode    = THREEDI_BUMP_ALPHA_SOURCE;
        out->specular_mode= THREEDI_SPEC_MIRROR_ONLY;
        out->has_overlay  = 1;
    } else if (strcmp(tag, "VS_BUMPMIRRT") == 0) {
        out->family       = THREEDI_FAMILY_GLASS;
        out->is_glass     = 1;
        out->bump_mode    = THREEDI_BUMP_ALPHA_SOURCE;
        out->specular_mode= THREEDI_SPEC_MIRROR_ONLY;
    } else if (strcmp(tag, "VS_ENVPHONGT") == 0) {
        out->family    = THREEDI_FAMILY_ENV;
        out->is_glass  = 1;
        out->bump_mode = THREEDI_BUMP_ALPHA_SOURCE;
        out->specular_mode = THREEDI_SPEC_ENV_LIT;
    } else if (strcmp(tag, "VS_SKGLASS") == 0) {
        out->family     = THREEDI_FAMILY_GLASS;
        out->is_glass   = 1;
        out->is_skinned = 1;
    } else if (contains(tag, "PHONG")) {
        out->family    = THREEDI_FAMILY_PHONG;
        out->bump_mode = contains(tag, "MDT") ? THREEDI_BUMP_MDT
                                              : THREEDI_BUMP_ALPHA_SOURCE;
        // VS_PHONGO has no bump, just object-space.
        if (strcmp(tag, "VS_PHONGO") == 0) {
            out->bump_mode = THREEDI_BUMP_NONE;
        }
        out->specular_mode = THREEDI_SPEC_FIXED_PHONG;
        out->is_skinned = starts_with(tag, "VS_SK");
    } else if (contains(tag, "DOT3") || contains(tag, "BUMPDIFF")) {
        out->family    = THREEDI_FAMILY_DOT3;
        out->bump_mode = THREEDI_BUMP_ALPHA_SOURCE;
        out->specular_mode = THREEDI_SPEC_NONE;
        out->is_skinned = starts_with(tag, "VS_SK");
    } else if (strcmp(tag, "VS_SKBASIC") == 0) {
        out->family     = THREEDI_FAMILY_FF;
        out->is_skinned = 1;
    } else if (starts_with(tag, "FF_")) {
        out->family = THREEDI_FAMILY_FF;
    }

    // Detail flag: `2` suffix on DOT3/PHONG (e.g. VS_DOT3DIFF2,
    // VS_SKBUMPDIFFOBJ2) OR FF_MT prefix (multi-texture; after blend-suffix
    // strip the tag may have shrunk to bare "FF_MT").
    if (starts_with(tag, "FF_MT")) {
        out->has_detail = 1;
    } else {
        // VS_*2 indicates detail variant.
        size_t tl = strlen(tag);
        if (tl > 0 && tag[tl - 1] == '2') {
            out->has_detail = 1;
        }
    }

    // Emissive / glass overrides from per-material binary flags.
    if (emissive_type == 2) out->luminance = 1;
    if (is_glass_byte) out->is_glass = 1;
    if (out->is_glass && out->blend_mode == THREEDI_CLASS_BLEND_OPAQUE) {
        // Glass/mirror always blends.
        out->blend_mode = THREEDI_CLASS_BLEND_ALPHA;
    }

    // Render attributes / material_flags bits.
    out->two_sided        = (rattrib & 0x1u) || (material_flags & MAT_FLAG_TWO_SIDED);
    out->alpha_test       = (rattrib & 0x400u) || (material_flags & MAT_FLAG_ALPHA_TEST);
    out->alpha_test_invert= (rattrib & 0x1000u) || (material_flags & MAT_FLAG_ALPHA_INVERT);
}

// ---------------------------------------------------------------------------
// classify_from_bhd_shader_type
// ---------------------------------------------------------------------------

void classify_from_bhd_shader_type(uint32_t shader_type,
                                    uint32_t render_attributes,
                                    uint8_t use_alpha_pcx,
                                    uint32_t blending_mode,
                                    uint32_t alpha_type,
                                    ThreediMaterialClass *out) {
    memset(out, 0, sizeof(*out));

    BhdShaderInfo info = bhd_shader_lookup(shader_type);
    if (info.name) {
        out->family         = info.family;
        out->bump_mode      = info.bump_mode;
        out->specular_mode  = info.specular_mode;
        out->has_detail     = info.has_detail;
        out->has_overlay    = info.has_overlay;
        out->is_skinned     = info.is_skinned;
        out->is_glass       = info.is_glass;
    } else {
        // Out of range — treat as no-op fixed-function.
        out->family = THREEDI_FAMILY_FF;
    }

    // Render attributes bitfield: 0x0001=two-sided, 0x0100=animated,
    // 0x0400=alpha_test, 0x1000=alpha_test_invert.
    out->two_sided         = (render_attributes & 0x0001u) ? 1 : 0;
    out->alpha_test        = (render_attributes & 0x0400u) ? 1 : 0;
    out->alpha_test_invert = (render_attributes & 0x1000u) ? 1 : 0;

    // BHD `use_alpha_pcx` is a redundant flag: it's set when the engine
    // should pull alpha from a sibling .pcx file.  Treat it as a hint that
    // alpha-test is in use.
    if (use_alpha_pcx) out->alpha_test = 1;

    // Blend mode: `blending_mode` 1 + `alpha_type` 2 → alpha-blend;
    // `blending_mode` 1 + `alpha_type` 0 → additive (BHD encoding).
    if (blending_mode != 0) {
        out->blend_mode = (alpha_type == 2)
            ? THREEDI_CLASS_BLEND_ALPHA
            : THREEDI_CLASS_BLEND_ADDITIVE;
    } else {
        out->blend_mode = THREEDI_CLASS_BLEND_OPAQUE;
    }
}

// ---------------------------------------------------------------------------
// synthesize_jo_shader_tag
// ---------------------------------------------------------------------------
//
// Construct the JO shader tag algorithmically from the classification
// fields.  The output must match an entry in JO's gMaterialInfoTable
// (45 rows in worktree-objects:libs/object/include/object/types.h); our
// test harness verifies that with a parity sweep over every classify ↔
// synthesize round-trip.

static void cat(char *dst, size_t dst_size, const char *s) {
    size_t l = strlen(dst);
    size_t r = strlen(s);
    if (l + r + 1 > dst_size) return;
    memcpy(dst + l, s, r + 1);
}

void synthesize_jo_shader_tag(const ThreediMaterialClass *cls,
                               char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    // Skinned glass has its own tag.
    if (cls->family == THREEDI_FAMILY_GLASS && cls->is_skinned) {
        cat(out, out_size, "VS_SKGLASS");
        return;
    }

    // Glass family — choose between FFP_GLASS / mirror variants.
    if (cls->family == THREEDI_FAMILY_GLASS) {
        if (cls->specular_mode == THREEDI_SPEC_MIRROR_ONLY) {
            // BHD shader_type 5 (chrome, no overlay) → VS_BUMPMIRRT;
            // BHD shader_type 6 / 14 (overlay) → VS_BMTXMIRRT.
            cat(out, out_size,
                cls->has_overlay ? "VS_BMTXMIRRT" : "VS_BUMPMIRRT");
            return;
        }
        cat(out, out_size, "FFP_GLASS");
        return;
    }

    // Env family (BHD shader_type 4 → JO VS_ENVPHONGT).
    if (cls->family == THREEDI_FAMILY_ENV) {
        cat(out, out_size, "VS_ENVPHONGT");
        return;
    }

    if (cls->family == THREEDI_FAMILY_FLAG) {
        cat(out, out_size, "VS_FLAG");
        return;
    }

    // Non-glass paths.  Skinned variants form their own family.
    if (cls->is_skinned) {
        if (cls->bump_mode == THREEDI_BUMP_NONE) {
            cat(out, out_size, "VS_SKBASIC");
            if (cls->uv_animated) cat(out, out_size, "#UV");
            return;
        }
        // Bumped skinned: VS_SKBUMPDIFFOBJ[2] for object-space; we don't
        // currently emit tangent-space skinned variants.
        cat(out, out_size, "VS_SKBUMPDIFFOBJ");
        if (cls->has_detail) cat(out, out_size, "2");
        return;
    }

    // PHONG family.
    if (cls->family == THREEDI_FAMILY_PHONG) {
        if (cls->bump_mode == THREEDI_BUMP_MDT) {
            cat(out, out_size, "VS_PHONGT_MDT");
            return;
        }
        cat(out, out_size, "VS_PHONGT");
        if (cls->uv_animated) cat(out, out_size, "#UV");
        return;
    }

    // DOT3 family.
    if (cls->family == THREEDI_FAMILY_DOT3) {
        if (cls->bump_mode == THREEDI_BUMP_MDT) {
            cat(out, out_size, "VS_PHONGT_MDT");
            return;
        }
        cat(out, out_size, "VS_DOT3DIFF");
        if (cls->has_detail) cat(out, out_size, "2");
        if (cls->uv_animated) cat(out, out_size, "#UV");
        return;
    }

    // FF family — apply detail / blend / luminance / uv suffixes.
    cat(out, out_size, cls->has_detail ? "FF_MT" : "FF_ST");
    switch (cls->blend_mode) {
        case THREEDI_CLASS_BLEND_OPAQUE:   cat(out, out_size, "_OP"); break;
        case THREEDI_CLASS_BLEND_ALPHA:    cat(out, out_size, "_AB"); break;
        case THREEDI_CLASS_BLEND_ADDITIVE: cat(out, out_size, "_AD"); break;
    }
    if (cls->luminance)   cat(out, out_size, "_LUM");
    if (cls->uv_animated) cat(out, out_size, "#UV");
}

// ---------------------------------------------------------------------------
// synthesize_bhd_shader_type
// ---------------------------------------------------------------------------

static int bhd_features_match(const BhdShaderInfo *row,
                               const ThreediMaterialClass *cls) {
    return row->family        == cls->family
        && row->bump_mode     == cls->bump_mode
        && row->specular_mode == cls->specular_mode
        && row->has_detail    == cls->has_detail
        && row->has_overlay   == cls->has_overlay
        && row->is_skinned    == cls->is_skinned
        && row->is_glass      == cls->is_glass;
}

uint32_t synthesize_bhd_shader_type(const ThreediMaterialClass *cls) {
    const BhdShaderInfo *table = bhd_shader_table();
    uint32_t count = bhd_shader_table_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (bhd_features_match(&table[i], cls)) {
            return i;
        }
    }
    // No exact match — fall back to BHD_SHADER_NONE.
    return 0;
}

// ---------------------------------------------------------------------------
// synthesize_render_attributes / synthesize_jo_rattrib
// ---------------------------------------------------------------------------

uint32_t synthesize_render_attributes(const ThreediMaterialClass *cls,
                                       int has_animation) {
    uint32_t r = 0;
    if (cls->two_sided)         r |= 0x0001u;
    if (has_animation)          r |= 0x0100u;
    if (cls->alpha_test)        r |= 0x0400u;
    if (cls->alpha_test_invert) r |= 0x1000u;
    return r;
}

uint32_t synthesize_jo_rattrib(const ThreediMaterialClass *cls,
                                int has_animation) {
    // BHD render_attributes and JO rattrib share the same low-bit
    // semantics for the four flags we model.  If JO carries additional
    // bits in higher positions, those would be added here.
    return synthesize_render_attributes(cls, has_animation);
}
