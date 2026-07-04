// 3DP project file parser/writer — pure C API.
// Flat structs suitable for FFI (ctypes, etc.).

#ifndef TDP_H
#define TDP_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define TDP_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

#define TDP_MAX_LODS        8
#define TDP_MAX_ANIM_FRAMES 8

// AxisFunc: parameters for a part animation axis function.
// Text format ordering: func_id  start  end  rate  phase  [ctrl_reg]
// (i.e. func_id  param2  param3  param0  param1  [ctrl_reg])
typedef struct TdpAxisFunc {
    int32_t func_id;
    float param0;      // rate
    float param1;      // phase / control_param
    float param2;      // start
    float param3;      // end
    char ctrl_reg[64];
} TdpAxisFunc;

typedef struct TdpPartAnim {
    int32_t rotate_type;
    int32_t scale_type;
    int32_t trans_type;
    int32_t transform_as;
    float yaw_rate;
    float pitch_rate;
    float roll_rate;
    TdpAxisFunc yaw;
    TdpAxisFunc pitch;
    TdpAxisFunc roll;
    int32_t reverse_rotate;
    TdpAxisFunc scale;
    TdpAxisFunc scale_x;
    TdpAxisFunc scale_y;
    TdpAxisFunc scale_z;
    TdpAxisFunc trans_x;
    TdpAxisFunc trans_y;
    TdpAxisFunc trans_z;
} TdpPartAnim;

typedef struct TdpAnimFrame {
    char path[32];
    int32_t enabled;
} TdpAnimFrame;

typedef struct TdpMaterial {
    char name[80];
    char shader_tag[32];
    int32_t rattrib;
    int32_t pattrib;
    int32_t ptype;
    int32_t geofx;
    float geofx_value;
    int32_t alphatestvalue;
    char diffuse_tex[2][32];
    int32_t diffuse_flags[2];
    char normal_tex[2][32];
    int32_t normal_flags[2];
    int32_t anim_frames;
    int32_t anim_type;
    int32_t anim_frametime;
    char anim_ctrlreg[64];
    TdpAnimFrame anim_diffuse[2][TDP_MAX_ANIM_FRAMES];
    TdpAnimFrame anim_normal[2][TDP_MAX_ANIM_FRAMES];
    int32_t reflect_rgb[3];
    int32_t rgbgen_style;
    float rgbgen_rate;
    float rgbgen_phase;
    int32_t rgbgen_srgb[3];
    int32_t rgbgen_ergb[3];
    char rgbgen_ctrlreg[64];
    int32_t alphagen_style;
    float alphagen_rate;
    float alphagen_phase;
    float alphagen_start;
    float alphagen_end;
    char alphagen_ctrlreg[64];
    int32_t mapfunc_u_style;
    float mapfunc_u_rate;
    float mapfunc_u_phase;
    float mapfunc_u_start;
    float mapfunc_u_end;
    char mapfunc_u_ctrlreg[64];
    int32_t mapfunc_v_style;
    float mapfunc_v_rate;
    float mapfunc_v_phase;
    float mapfunc_v_start;
    float mapfunc_v_end;
    char mapfunc_v_ctrlreg[64];
} TdpMaterial;

typedef struct TdpLight {
    char name[32];
    int32_t colorgen_style;
    float colorgen_rate;
    float colorgen_phase;
    int32_t colorgen_start[3];
    int32_t colorgen_end[3];
    char colorgen_ctrlreg[64];
    int32_t disable_corona;
    int32_t disable_lightterrain;
    int32_t disable_lightobjects;
} TdpLight;

typedef struct TdpLod {
    char scene_file[64];
    int32_t attributes;
    char render_function[32];
    float threshold;
    int32_t part_anim_enabled;
    TdpPartAnim *part_anims;
    size_t part_anim_count;
    TdpLight *lights;
    size_t light_count;
} TdpLod;

typedef struct TdpProject {
    int32_t version;
    int32_t poly_collision_lod;
    TdpMaterial *materials;
    size_t material_count;
    TdpLod lods[TDP_MAX_LODS];
} TdpProject;

// Initialize a TdpProject to zero state.
TDP_EXPORT void tdp_init(TdpProject *proj);

// Free all dynamic allocations inside a TdpProject.
TDP_EXPORT void tdp_free(TdpProject *proj);

// Parse a .3dp project file. Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_parse(const char *path, TdpProject *out);

// Write a .3dp project file. Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_write(const char *path, const TdpProject *proj);

// Allocate (or reallocate) arrays so that Python/FFI callers can build
// projects without manual malloc.  Each frees the old pointer first.
TDP_EXPORT void tdp_alloc_materials(TdpProject *proj, size_t count);
TDP_EXPORT void tdp_alloc_part_anims(TdpLod *lod, size_t count);
TDP_EXPORT void tdp_alloc_lights(TdpLod *lod, size_t count);

// Forward-declare the IR struct so we don't require threedi header.
struct ThreediModelIR;

// Populate a TdpProject from a ThreediModelIR.
// Caller must call tdp_free() on `out` when done.
// Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_from_ir(const struct ThreediModelIR *ir, TdpProject *out);

#ifdef __cplusplus
}
#endif

#endif // TDP_H
