// 3DP/3DA project material data.
//
// These structs are the public TDP material representation used by the
// project readers, writers, and object exporter. They are not a shared
// model abstraction.

#ifndef TDP_MATERIAL_H
#define TDP_MATERIAL_H

#include <stdint.h>

#include "threedi/threedi_material_class.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TdpBlendMode {
    TDP_BLEND_OPAQUE = 0,
    TDP_BLEND_ALPHA  = 1,
    TDP_BLEND_ADD    = 2,
} TdpBlendMode;

#define TDP_MATERIAL_FLAG_ALPHA_TEST    0x01u
#define TDP_MATERIAL_FLAG_ALPHA_INVERT  0x02u
#define TDP_MATERIAL_FLAG_TWO_SIDED     0x04u
#define TDP_MATERIAL_FLAG_EMISSIVE      0x08u

#define TDP_MAX_MATERIAL_TEXTURES 24
#define TDP_TEX_SLOT_DIFFUSE  1
#define TDP_TEX_SLOT_DETAIL   2
#define TDP_TEX_SLOT_NORMAL   3
#define TDP_TEX_SLOT_NORMAL_B 4

#define TDP_MAX_ANIM_FRAMES 8

typedef struct TdpMaterialTexture {
    char name[17];
    uint8_t slot;
    uint8_t type;
    uint8_t flags;
    uint8_t frame;
} TdpMaterialTexture;

typedef struct TdpUvParams {
    uint8_t style;
    float phase;
    int32_t reg;
    float gen_rate;
    float start;
    float end;
} TdpUvParams;

typedef struct TdpAlphaGen {
    uint8_t style;
    float phase;
    int32_t reg;
    float rate;
    int16_t start;
    int16_t end;
} TdpAlphaGen;

typedef struct TdpRgbGen {
    uint8_t style;
    float phase;
    int32_t reg;
    float rate;
    float start_color[4];
    float end_color[4];
} TdpRgbGen;

typedef struct TdpTexAnim {
    uint8_t num_frames;
    uint8_t animation_type;
    int16_t cycle_frame_time;
} TdpTexAnim;

typedef struct TdpAnimTexture {
    char path[32];
    int32_t enabled;
} TdpAnimTexture;

typedef struct TdpMaterial {
    int32_t index;
    char name[80];
    char shader_name[33];
    uint32_t texture_count;
    TdpMaterialTexture textures[TDP_MAX_MATERIAL_TEXTURES];
    uint32_t flags;
    uint8_t material_flags;
    uint8_t alpha_test_value_byte;
    uint8_t material_pad[2];
    float alpha_threshold;
    TdpBlendMode blend_mode;

    ThreediMaterialClass classification;

    TdpUvParams u_params;
    TdpUvParams v_params;
    TdpAlphaGen alpha_gen;
    TdpRgbGen rgb_gen;
    TdpRgbGen rgb_gen2;
    TdpTexAnim animation;

    float reflect_color[4];
    float reflect_color2[4];
    int is_glass;
    int32_t glass_reflect_hi;
    int32_t glass_reflect_mid;
    int32_t glass_reflect_lo;
    uint32_t reflect_type;
    uint32_t reflect_alpha;

    uint32_t specular_intensity;
    uint32_t specular_sharpness;
    uint32_t luminosity;
    uint32_t emissive_color;
    uint32_t transparency;
    uint8_t color_green[3];
    uint8_t color_alpha[3];
    uint8_t emissive_type;
    uint8_t emissive_type2;
    uint8_t glass_type2;
    uint8_t material_pad2;

    float u_offset;
    float v_offset;
    float u_tiling;
    float v_tiling;
    float uv1_u_offset;
    float uv1_v_offset;
    float uv1_u_tiling;
    float uv1_v_tiling;

    char anim_ctrlreg[64];
    TdpAnimTexture anim_diffuse[2][TDP_MAX_ANIM_FRAMES];
    TdpAnimTexture anim_normal[2][TDP_MAX_ANIM_FRAMES];

    uint32_t anim_sequence;

    uint8_t surface_type;
    uint32_t surface_extra_bits;
    uint32_t pattrib;

    int32_t geofx;
    float geofx_value;

    uint32_t color_type;
    uint32_t actionplane_type;
    uint32_t projector_type;
    uint32_t projector_no_receive;
    uint32_t projector_yaw;
    uint32_t projector_pitch;
} TdpMaterial;

typedef struct TdpControlRegister {
    char name[25];
} TdpControlRegister;

#ifdef __cplusplus
}
#endif

#endif // TDP_MATERIAL_H
