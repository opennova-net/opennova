// BHD GP shader_type lookup table.
//
// Each row maps one shader_type integer to its UI label + the full feature
// set that the material classification carries.  IDA citations point to
// df4oed.exe description-array entries used by sub_40A7C0::SetDlgItemTextA(
// .., &aNoAdvancedShad[256*shader_type]).

#include "threedi/threedi_bhd_shader.h"

#include <stddef.h>

static const BhdShaderInfo kBhdShaderTable[] = {
    // shader_type 0 (df4oed @ 0x4b25b8 "No advanced shading"):
    //   No bump, no specular.  Pure fixed-function.
    [BHD_SHADER_NONE] = {
        .name           = "No advanced shading",
        .base_jo_tag    = "FF_ST_OP",
        .family         = THREEDI_FAMILY_FF,
        .bump_mode      = THREEDI_BUMP_NONE,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 1 (df4oed @ 0x4b26b8):
    //   Diffuse-only bump, alpha-source.  JO equivalent: VS_DOT3DIFF.
    [BHD_SHADER_DIFFUSE_BUMP] = {
        .name           = "Diffuse Bump Map",
        .base_jo_tag    = "VS_DOT3DIFF",
        .family         = THREEDI_FAMILY_DOT3,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 2 (df4oed @ 0x4b27b8):
    //   Phong specular, fixed exponent, alpha-source bump.  JO: VS_PHONGT.
    [BHD_SHADER_PHONG_BUMP] = {
        .name           = "Phong Bump Map",
        .base_jo_tag    = "VS_PHONGT",
        .family         = THREEDI_FAMILY_PHONG,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_FIXED_PHONG,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 3 (df4oed @ 0x4b28b8):
    //   White-only specular (no material color), bumped diffuse.
    [BHD_SHADER_DIFFSPEC_BUMP] = {
        .name           = "Diffuse+Specular Bump",
        .base_jo_tag    = "VS_PHONGT",
        .family         = THREEDI_FAMILY_PHONG,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_WHITE_ONLY,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 4 (df4oed @ 0x4b29b8):
    //   Env-mapped specular via gloss map, bumped diffuse.  JO:
    //   VS_ENVPHONGT.
    [BHD_SHADER_DIFFSPEC_ENV_BUMP] = {
        .name           = "Diff/Spec/Glossmap Bump",
        .base_jo_tag    = "VS_ENVPHONGT",
        .family         = THREEDI_FAMILY_ENV,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_ENV_LIT,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 1,
        .is_mirror_only = 0,
    },
    // shader_type 5 (df4oed @ 0x4b2ab8):
    //   Chrome-style mirror, no diffuse specular.  JO: VS_BUMPMIRRT.
    [BHD_SHADER_CHROME_BUMP] = {
        .name           = "Chrome Bump",
        .base_jo_tag    = "VS_BUMPMIRRT",
        .family         = THREEDI_FAMILY_GLASS,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_MIRROR_ONLY,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 1,
        .is_mirror_only = 1,
    },
    // shader_type 6 (df4oed @ 0x4b2bb8):
    //   Env+overlay (Stage2 base), with diffuse specular env.
    [BHD_SHADER_ENV_OVERLAY_BUMP] = {
        .name           = "Env+Overlay Bump",
        .base_jo_tag    = "VS_BMTXMIRRT",
        .family         = THREEDI_FAMILY_GLASS,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_ENV_LIT,
        .has_detail     = 0,
        .has_overlay    = 1,
        .is_skinned     = 0,
        .is_glass       = 1,
        .is_mirror_only = 0,
    },
    // shader_type 7 (df4oed @ 0x4b2cb8):
    //   Phong-lit but no bumpmap and no specular (per UI label).
    //   Functionally a UI duplicate of shader_type 0 with different label.
    [BHD_SHADER_PHONG_NO_BUMP] = {
        .name           = "Phong (no bump)",
        .base_jo_tag    = "FF_ST_OP",
        .family         = THREEDI_FAMILY_FF,
        .bump_mode      = THREEDI_BUMP_NONE,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 8 (df4oed @ 0x4b2db8):
    //   MDT-bumped diffuse, no specular.  JO: VS_PHONGT_MDT (lossy — JO
    //   conflates the four MDT variants, but the material class differentiates).
    [BHD_SHADER_MDT_DIFFUSE_BUMP] = {
        .name           = "MDT Diffuse Bump Map",
        .base_jo_tag    = "VS_PHONGT_MDT",
        .family         = THREEDI_FAMILY_DOT3,
        .bump_mode      = THREEDI_BUMP_MDT,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 9 (df4oed @ 0x4b2fb8):
    //   MDT-bumped + phong fixed-exponent specular.
    [BHD_SHADER_MDT_PHONG_BUMP] = {
        .name           = "MDT Phong Bump Map",
        .base_jo_tag    = "VS_PHONGT_MDT",
        .family         = THREEDI_FAMILY_PHONG,
        .bump_mode      = THREEDI_BUMP_MDT,
        .specular_mode  = THREEDI_SPEC_FIXED_PHONG,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 10 (df4oed @ 0x4b30b8):
    //   MDT-bumped + variable specular V2 algorithm.
    [BHD_SHADER_MDT_PHONG_BUMP_V2] = {
        .name           = "MDT Phong V2",
        .base_jo_tag    = "VS_PHONGT_MDT",
        .family         = THREEDI_FAMILY_PHONG,
        .bump_mode      = THREEDI_BUMP_MDT,
        .specular_mode  = THREEDI_SPEC_VARIABLE_V2,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 11 (df4oed @ 0x4b31b8):
    //   V2 + render-target output (BHD-only).
    [BHD_SHADER_MDT_PHONG_BUMP_V2_RT] = {
        .name           = "MDT Phong V2 + RT",
        .base_jo_tag    = "VS_PHONGT_MDT",
        .family         = THREEDI_FAMILY_PHONG,
        .bump_mode      = THREEDI_BUMP_MDT,
        .specular_mode  = THREEDI_SPEC_VARIABLE_V2_RT,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 12 (df4oed @ 0x4b34b8):
    //   Detail-stage variant of shader_type 1.  JO: VS_DOT3DIFF2.
    [BHD_SHADER_DIFFUSE_BUMP_DETAIL] = {
        .name           = "Diffuse Bump + Detail",
        .base_jo_tag    = "VS_DOT3DIFF2",
        .family         = THREEDI_FAMILY_DOT3,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 1,
        .has_overlay    = 0,
        .is_skinned     = 0,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 13 (df4oed @ 0x4b35b8):
    //   Skin (skinned-mesh) bumped diffuse variant.
    [BHD_SHADER_SKIN_DIFFUSE_BUMP] = {
        .name           = "Skin Diffuse Bump",
        .base_jo_tag    = "VS_SKBUMPDIFFOBJ",
        .family         = THREEDI_FAMILY_DOT3,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_NONE,
        .has_detail     = 0,
        .has_overlay    = 0,
        .is_skinned     = 1,
        .is_glass       = 0,
        .is_mirror_only = 0,
    },
    // shader_type 14 (df4oed @ 0x4b39b8):
    //   Mirror reflection with overlay (no diffuse specular).
    [BHD_SHADER_REFLECTIVE_ENV_OVERLAY] = {
        .name           = "Reflective Env + Overlay",
        .base_jo_tag    = "VS_BMTXMIRRT",
        .family         = THREEDI_FAMILY_GLASS,
        .bump_mode      = THREEDI_BUMP_ALPHA_SOURCE,
        .specular_mode  = THREEDI_SPEC_MIRROR_ONLY,
        .has_detail     = 0,
        .has_overlay    = 1,
        .is_skinned     = 0,
        .is_glass       = 1,
        .is_mirror_only = 1,
    },
};

#define BHD_SHADER_TABLE_COUNT \
    (sizeof(kBhdShaderTable) / sizeof(kBhdShaderTable[0]))

BhdShaderInfo bhd_shader_lookup(uint32_t shader_type) {
    if (shader_type >= BHD_SHADER_TABLE_COUNT) {
        BhdShaderInfo none = {0};
        none.name = NULL;
        return none;
    }
    return kBhdShaderTable[shader_type];
}

uint32_t bhd_shader_table_count(void) {
    return (uint32_t)BHD_SHADER_TABLE_COUNT;
}

const BhdShaderInfo *bhd_shader_table(void) {
    return kBhdShaderTable;
}
