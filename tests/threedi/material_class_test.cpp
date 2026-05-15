// Tests for ThreediMaterialClass classify/synthesize round-trips.
//
// Covers:
//   - classify_from_bhd_shader_type for each 0..14
//   - classify_from_jo_shader_tag for sample JO catalog entries
//   - synthesize_bhd_shader_type round-trip
//   - synthesize_jo_shader_tag round-trip
//   - synthesize_render_attributes from flag combinations

#include <cstdio>
#include <cstring>

#include "threedi/threedi_material_class.h"
#include "threedi/threedi_bhd_shader.h"

static int failures = 0;

static void expect_true(const char *label, int cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void expect_eq_u32(const char *label, uint32_t got, uint32_t want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s — got %u, want %u\n", label, got, want);
        failures++;
    }
}

static void expect_eq_str(const char *label, const char *got, const char *want) {
    if (std::strcmp(got, want) != 0) {
        std::fprintf(stderr, "FAIL: %s — got '%s', want '%s'\n", label, got, want);
        failures++;
    }
}

// ============================================================================
// classify_from_bhd_shader_type
// ============================================================================

static void test_bhd_classify_shader_type_0_no_advanced() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(0, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 0 family",        cls.family,        THREEDI_FAMILY_FF);
    expect_eq_u32("bhd 0 bump_mode",     cls.bump_mode,     THREEDI_BUMP_NONE);
    expect_eq_u32("bhd 0 specular_mode", cls.specular_mode, THREEDI_SPEC_NONE);
    expect_eq_u32("bhd 0 has_detail",    cls.has_detail,    0);
    expect_eq_u32("bhd 0 is_glass",      cls.is_glass,      0);
}

static void test_bhd_classify_shader_type_2_phong_bump() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(2, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 2 family",        cls.family,        THREEDI_FAMILY_PHONG);
    expect_eq_u32("bhd 2 bump_mode",     cls.bump_mode,     THREEDI_BUMP_ALPHA_SOURCE);
    expect_eq_u32("bhd 2 specular_mode", cls.specular_mode, THREEDI_SPEC_FIXED_PHONG);
}

static void test_bhd_classify_shader_type_5_chrome() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(5, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 5 family",        cls.family,        THREEDI_FAMILY_GLASS);
    expect_eq_u32("bhd 5 bump_mode",     cls.bump_mode,     THREEDI_BUMP_ALPHA_SOURCE);
    expect_eq_u32("bhd 5 specular_mode", cls.specular_mode, THREEDI_SPEC_MIRROR_ONLY);
    expect_eq_u32("bhd 5 is_glass",      cls.is_glass,      1);
    expect_eq_u32("bhd 5 has_overlay",   cls.has_overlay,   0);
}

static void test_bhd_classify_shader_type_6_env_overlay() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(6, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 6 has_overlay",   cls.has_overlay,   1);
    expect_eq_u32("bhd 6 specular_mode", cls.specular_mode, THREEDI_SPEC_ENV_LIT);
}

static void test_bhd_classify_shader_type_9_mdt_phong() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(9, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 9 family",        cls.family,        THREEDI_FAMILY_PHONG);
    expect_eq_u32("bhd 9 bump_mode",     cls.bump_mode,     THREEDI_BUMP_MDT);
    expect_eq_u32("bhd 9 specular_mode", cls.specular_mode, THREEDI_SPEC_FIXED_PHONG);
}

static void test_bhd_classify_shader_type_12_detail() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(12, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 12 has_detail",   cls.has_detail,    1);
    expect_eq_u32("bhd 12 family",       cls.family,        THREEDI_FAMILY_DOT3);
}

static void test_bhd_classify_shader_type_13_skinned() {
    ThreediMaterialClass cls;
    classify_from_bhd_shader_type(13, 0, 0, 0, 0, &cls);
    expect_eq_u32("bhd 13 is_skinned",   cls.is_skinned,    1);
}

static void test_bhd_classify_render_attribute_bits() {
    ThreediMaterialClass cls;
    // Bits: 0x001=two-sided, 0x400=alpha_test, 0x1000=alpha_test_invert.
    classify_from_bhd_shader_type(0, 0x1401, 0, 0, 0, &cls);
    expect_eq_u32("rattr 0x1401 two_sided",         cls.two_sided,         1);
    expect_eq_u32("rattr 0x1401 alpha_test",        cls.alpha_test,        1);
    expect_eq_u32("rattr 0x1401 alpha_test_invert", cls.alpha_test_invert, 1);
}

static void test_bhd_classify_blending_mode() {
    ThreediMaterialClass cls;
    // blending_mode=1, alpha_type=2 → alpha-blend.
    classify_from_bhd_shader_type(0, 0, 0, 1, 2, &cls);
    expect_eq_u32("blend alpha", cls.blend_mode, THREEDI_CLASS_BLEND_ALPHA);
    // blending_mode=1, alpha_type=0 → additive.
    classify_from_bhd_shader_type(0, 0, 0, 1, 0, &cls);
    expect_eq_u32("blend add",   cls.blend_mode, THREEDI_CLASS_BLEND_ADDITIVE);
    // blending_mode=0 → opaque regardless of alpha_type.
    classify_from_bhd_shader_type(0, 0, 0, 0, 2, &cls);
    expect_eq_u32("blend opaque",cls.blend_mode, THREEDI_CLASS_BLEND_OPAQUE);
}

// ============================================================================
// classify_from_jo_shader_tag
// ============================================================================

static void test_jo_classify_ff_st_op() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("FF_ST_OP", 0, 0, 0, 0, &cls);
    expect_eq_u32("FF_ST_OP family",     cls.family,     THREEDI_FAMILY_FF);
    expect_eq_u32("FF_ST_OP blend",      cls.blend_mode, THREEDI_CLASS_BLEND_OPAQUE);
    expect_eq_u32("FF_ST_OP has_detail", cls.has_detail, 0);
}

static void test_jo_classify_ff_mt_ab_lum_uv() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("FF_MT_AB_LUM#UV", 0, 0, 0, 0, &cls);
    expect_eq_u32("FF_MT_AB_LUM#UV family",     cls.family,      THREEDI_FAMILY_FF);
    expect_eq_u32("FF_MT_AB_LUM#UV has_detail", cls.has_detail,  1);
    expect_eq_u32("FF_MT_AB_LUM#UV blend",      cls.blend_mode,  THREEDI_CLASS_BLEND_ALPHA);
    expect_eq_u32("FF_MT_AB_LUM#UV luminance",  cls.luminance,   1);
    expect_eq_u32("FF_MT_AB_LUM#UV uv_animated",cls.uv_animated, 1);
}

static void test_jo_classify_ff_st_ad() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("FF_ST_AD", 0, 0, 0, 0, &cls);
    expect_eq_u32("FF_ST_AD blend", cls.blend_mode, THREEDI_CLASS_BLEND_ADDITIVE);
}

static void test_jo_classify_vs_phongt() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_PHONGT", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_PHONGT family",        cls.family,        THREEDI_FAMILY_PHONG);
    expect_eq_u32("VS_PHONGT bump_mode",     cls.bump_mode,     THREEDI_BUMP_ALPHA_SOURCE);
    expect_eq_u32("VS_PHONGT specular_mode", cls.specular_mode, THREEDI_SPEC_FIXED_PHONG);
}

static void test_jo_classify_vs_phongt_mdt() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_PHONGT_MDT", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_PHONGT_MDT bump_mode", cls.bump_mode, THREEDI_BUMP_MDT);
}

static void test_jo_classify_vs_dot3diff2() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_DOT3DIFF2", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_DOT3DIFF2 family",     cls.family,     THREEDI_FAMILY_DOT3);
    expect_eq_u32("VS_DOT3DIFF2 has_detail", cls.has_detail, 1);
}

static void test_jo_classify_vs_skbasic() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_SKBASIC", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_SKBASIC is_skinned", cls.is_skinned, 1);
    expect_eq_u32("VS_SKBASIC family",     cls.family,     THREEDI_FAMILY_FF);
}

static void test_jo_classify_ffp_glass() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("FFP_GLASS", 0, 0, 0, 0, &cls);
    expect_eq_u32("FFP_GLASS family",   cls.family,     THREEDI_FAMILY_GLASS);
    expect_eq_u32("FFP_GLASS is_glass", cls.is_glass,   1);
    expect_eq_u32("FFP_GLASS blend",    cls.blend_mode, THREEDI_CLASS_BLEND_ALPHA);
}

static void test_jo_classify_vs_bumpmirrt() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_BUMPMIRRT", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_BUMPMIRRT specular", cls.specular_mode, THREEDI_SPEC_MIRROR_ONLY);
    expect_eq_u32("VS_BUMPMIRRT is_glass", cls.is_glass,      1);
}

static void test_jo_classify_vs_envphongt() {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag("VS_ENVPHONGT", 0, 0, 0, 0, &cls);
    expect_eq_u32("VS_ENVPHONGT specular", cls.specular_mode, THREEDI_SPEC_ENV_LIT);
}

// ============================================================================
// synthesize_bhd_shader_type round-trips
// ============================================================================

static void test_bhd_round_trip_all() {
    for (uint32_t i = 0; i < bhd_shader_table_count(); ++i) {
        ThreediMaterialClass cls;
        classify_from_bhd_shader_type(i, 0, 0, 0, 0, &cls);
        uint32_t back = synthesize_bhd_shader_type(&cls);
        // Same-feature pairs collapse: 0/7 both pure FF; 6/14 both
        // glass+overlay+mirror; 8 vs 9 differ (different specular_mode).
        // Our synthesizer returns lowest-index match.
        const BhdShaderInfo *want = &bhd_shader_table()[back];
        const BhdShaderInfo *got  = &bhd_shader_table()[i];
        char label[64];
        std::snprintf(label, sizeof(label), "bhd round-trip %u → %u features", i, back);
        // The returned shader_type's features must match the input's
        // features (collapsing is fine).
        expect_true(label,
            want->family == got->family
            && want->bump_mode == got->bump_mode
            && want->specular_mode == got->specular_mode
            && want->has_detail == got->has_detail
            && want->has_overlay == got->has_overlay
            && want->is_skinned == got->is_skinned
            && want->is_glass == got->is_glass);
    }
}

// ============================================================================
// synthesize_jo_shader_tag round-trips
// ============================================================================

static void check_jo_round_trip(const char *tag) {
    ThreediMaterialClass cls;
    classify_from_jo_shader_tag(tag, 0, 0, 0, 0, &cls);
    char back[64];
    synthesize_jo_shader_tag(&cls, back, sizeof(back));
    char label[128];
    std::snprintf(label, sizeof(label), "jo round-trip '%s' → '%s'", tag, back);
    expect_eq_str(label, back, tag);
}

static void test_jo_round_trip_basic_tags() {
    check_jo_round_trip("FF_ST_OP");
    check_jo_round_trip("FF_ST_AB");
    check_jo_round_trip("FF_ST_AD");
    check_jo_round_trip("FF_ST_OP_LUM");
    check_jo_round_trip("FF_ST_AB_LUM");
    check_jo_round_trip("FF_ST_AD_LUM");
    check_jo_round_trip("FF_MT_OP");
    check_jo_round_trip("FF_MT_AB");
    check_jo_round_trip("FF_MT_AD");
    check_jo_round_trip("FF_MT_OP_LUM");
    check_jo_round_trip("FF_MT_AB_LUM");
    check_jo_round_trip("FF_MT_AD_LUM");
}

static void test_jo_round_trip_uv_variants() {
    check_jo_round_trip("FF_ST_OP#UV");
    check_jo_round_trip("FF_MT_AB_LUM#UV");
    check_jo_round_trip("VS_DOT3DIFF#UV");
    check_jo_round_trip("VS_PHONGT#UV");
    check_jo_round_trip("VS_SKBASIC#UV");
}

static void test_jo_round_trip_vs_tags() {
    check_jo_round_trip("VS_DOT3DIFF");
    check_jo_round_trip("VS_DOT3DIFF2");
    check_jo_round_trip("VS_PHONGT");
    check_jo_round_trip("VS_PHONGT_MDT");
    check_jo_round_trip("VS_SKBASIC");
    check_jo_round_trip("VS_SKBUMPDIFFOBJ");
    check_jo_round_trip("VS_SKBUMPDIFFOBJ2");
    check_jo_round_trip("VS_SKGLASS");
    check_jo_round_trip("VS_FLAG");
    check_jo_round_trip("FFP_GLASS");
    check_jo_round_trip("VS_BUMPMIRRT");
    check_jo_round_trip("VS_BMTXMIRRT");
    check_jo_round_trip("VS_ENVPHONGT");
}

// ============================================================================
// synthesize_render_attributes
// ============================================================================

static void test_synthesize_render_attributes() {
    ThreediMaterialClass cls;
    std::memset(&cls, 0, sizeof(cls));

    cls.two_sided = 1;
    expect_eq_u32("rattr two_sided",
                  synthesize_render_attributes(&cls, 0), 0x0001u);

    cls.two_sided = 0;
    cls.alpha_test = 1;
    expect_eq_u32("rattr alpha_test",
                  synthesize_render_attributes(&cls, 0), 0x0400u);

    cls.alpha_test = 0;
    cls.alpha_test_invert = 1;
    expect_eq_u32("rattr alpha_test_invert",
                  synthesize_render_attributes(&cls, 0), 0x1000u);

    std::memset(&cls, 0, sizeof(cls));
    expect_eq_u32("rattr animated",
                  synthesize_render_attributes(&cls, 1), 0x0100u);
}

// ============================================================================

int main() {
    test_bhd_classify_shader_type_0_no_advanced();
    test_bhd_classify_shader_type_2_phong_bump();
    test_bhd_classify_shader_type_5_chrome();
    test_bhd_classify_shader_type_6_env_overlay();
    test_bhd_classify_shader_type_9_mdt_phong();
    test_bhd_classify_shader_type_12_detail();
    test_bhd_classify_shader_type_13_skinned();
    test_bhd_classify_render_attribute_bits();
    test_bhd_classify_blending_mode();

    test_jo_classify_ff_st_op();
    test_jo_classify_ff_mt_ab_lum_uv();
    test_jo_classify_ff_st_ad();
    test_jo_classify_vs_phongt();
    test_jo_classify_vs_phongt_mdt();
    test_jo_classify_vs_dot3diff2();
    test_jo_classify_vs_skbasic();
    test_jo_classify_ffp_glass();
    test_jo_classify_vs_bumpmirrt();
    test_jo_classify_vs_envphongt();

    test_bhd_round_trip_all();

    test_jo_round_trip_basic_tags();
    test_jo_round_trip_uv_variants();
    test_jo_round_trip_vs_tags();

    test_synthesize_render_attributes();

    if (failures == 0) {
        std::fprintf(stderr, "all material_class tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d material_class test failures\n", failures);
    return 1;
}
