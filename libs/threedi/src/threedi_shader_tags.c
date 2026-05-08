#include "threedi/threedi_shader_tags.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#define E(name, flags) { name, (uint32_t)(flags) }
#define F(x)           THREEDI_MAT_FLAG_##x

const ThreediShaderInfo kThreediShaderTable[] = {
    E("FF_ST_OP",         F(DIFFUSE)),
    E("FF_ST_OP#UV",      F(DIFFUSE) | F(UI_TOGGLE)),
    E("FF_ST_AB",         F(ALPHA) | F(DIFFUSE) | F(SPECIAL)),
    E("FF_ST_AB#UV",      F(ALPHA) | F(DIFFUSE) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_ST_AD",         F(DIFFUSE) | F(SPECIAL)),
    E("FF_ST_AD#UV",      F(DIFFUSE) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_ST_OP_LUM",     F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE)),
    E("FF_ST_OP_LUM#UV",  F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(UI_TOGGLE)),
    E("FF_ST_AB_LUM",     F(LUMINANCE) | F(ALPHA) | F(EMISSIVE) | F(DIFFUSE) | F(SPECIAL)),
    E("FF_ST_AB_LUM#UV",  F(LUMINANCE) | F(ALPHA) | F(EMISSIVE) | F(DIFFUSE) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_ST_AD_LUM",     F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SPECIAL)),
    E("FF_ST_AD_LUM#UV",  F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_MT_OP",         F(DIFFUSE) | F(SECONDARY)),
    E("FF_MT_OP#UV",      F(DIFFUSE) | F(SECONDARY) | F(UI_TOGGLE)),
    E("FF_MT_AB",         F(ALPHA) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL)),
    E("FF_MT_AB#UV",      F(ALPHA) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_MT_AD",         F(DIFFUSE) | F(SECONDARY) | F(SPECIAL)),
    E("FF_MT_AD#UV",      F(DIFFUSE) | F(SECONDARY) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_MT_OP_LUM",     F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY)),
    E("FF_MT_OP_LUM#UV",  F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY) | F(UI_TOGGLE)),
    E("FF_MT_AB_LUM",     F(LUMINANCE) | F(ALPHA) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL)),
    E("FF_MT_AB_LUM#UV",  F(LUMINANCE) | F(ALPHA) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FF_MT_AD_LUM",     F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL)),
    E("FF_MT_AD_LUM#UV",  F(LUMINANCE) | F(EMISSIVE) | F(DIFFUSE) | F(SECONDARY) | F(SPECIAL) | F(UI_TOGGLE)),
    E("FFP_GLASS",        F(GLASS) | F(SMOOTH) | F(SPECIAL)),
    E("VS_DOT3DIFFOBJ",   F(NORMAL_A) | F(DIFFUSE)),
    E("VS_PHONGO",        F(NORMAL_A) | F(DIFFUSE)),
    E("VS_DOT3DIFF",      F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_DOT3DIFF#UV",   F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE) | F(UI_TOGGLE)),
    E("VS_PHONGT",        F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_PHONGT#UV",     F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE) | F(UI_TOGGLE)),
    E("VS_DOT3DIFF2",     F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE) | F(SECONDARY)),
    E("VS_BMTXMIRRT",     F(GLASS) | F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_BUMPMIRRT",     F(GLASS) | F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_ENVPHONGT",     F(GLASS) | F(SMOOTH) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_SKBASIC",       F(FILTER) | F(DIFFUSE)),
    E("VS_SKBASIC#UV",    F(FILTER) | F(DIFFUSE) | F(UI_TOGGLE)),
    E("VS_SKGLASS",       F(GLASS) | F(FILTER) | F(DIFFUSE) | F(SPECIAL)),
    E("VS_SKBUMPDIFFOBJ", F(FILTER) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_SKBUMPPHONGOBJ",F(FILTER) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_SKBUMPDIFFOBJ2",F(FILTER) | F(NORMAL_A) | F(DIFFUSE) | F(SECONDARY)),
    E("VS_SKBUMPDIFFT",   F(GLASS) | F(FILTER) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_SKBUMPPHONGT",  F(GLASS) | F(FILTER) | F(NORMAL_A) | F(DIFFUSE)),
    E("VS_SKBUMPDIFFT2",  F(GLASS) | F(FILTER) | F(NORMAL_A) | F(DIFFUSE) | F(SECONDARY)),
    E("VS_FLAG",          F(DIFFUSE)),
};

const size_t kThreediShaderTableCount =
    sizeof(kThreediShaderTable) / sizeof(kThreediShaderTable[0]);

#undef E
#undef F

static int icmp_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (toupper(ca) != toupper(cb)) return 0;
    }
    return *a == '\0' && *b == '\0';
}

uint32_t threedi_shader_flags(const char *tag) {
    if (!tag || !*tag) return 0u;
    for (size_t i = 0; i < kThreediShaderTableCount; ++i) {
        if (icmp_eq(tag, kThreediShaderTable[i].name)) {
            return kThreediShaderTable[i].flags;
        }
    }
    return 0u;
}

int threedi_shader_known(const char *tag) {
    return threedi_shader_flags(tag) != 0u ? 1 : 0;
}
