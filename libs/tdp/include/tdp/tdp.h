// 3DP project file parser/writer - pure C API.
// Flat structs suitable for FFI (ctypes, etc.).
//
// Materials in TdpProject are TdpMaterial records shared by the 3DP/3DA
// project readers, writers, and object exporter.

#ifndef TDP_H
#define TDP_H

#include <stddef.h>
#include <stdint.h>

#include "tdp/tdp_material.h"

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define TDP_EXPORT __declspec(dllexport)
#  else
#    define TDP_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define TDP_EXPORT __attribute__((visibility("default")))
#  else
#    define TDP_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TDP_MAX_LODS 8

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

    // Optional: control register table populated by readers when project
    // generators reference ctrl-reg names. Allocated by parser; freed by
    // tdp_free.
    TdpControlRegister *ctrl_regs;
    size_t ctrl_reg_count;

    // 3DA `username` field — preserved through tdp_read_3da/tdp_write_3da
    // round-trips.  Empty string falls back to writer default ("opennova").
    char username[64];
} TdpProject;

// Initialize a TdpProject to zero state.
TDP_EXPORT void tdp_init(TdpProject *proj);

// Free all dynamic allocations inside a TdpProject.
TDP_EXPORT void tdp_free(TdpProject *proj);

// Parse a .3dp project file. Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_parse(const char *path, TdpProject *out);

// Write a .3dp project file. Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_write(const char *path, const TdpProject *proj);

// Write a .3da project file (legacy ModSuperOED format). Returns 0 on success, -1 on error.
TDP_EXPORT int tdp_write_3da(const char *path, const TdpProject *proj);

// Read a .3da project file (legacy ModSuperOED format). Populates the TDP
// material array such that a subsequent tdp_write_3da on `out` produces
// byte-identical output (modulo the timestamp comment).  Mirrors
// df4oed.exe::sub_425730 token-driven dispatch.  Returns 0 on success, -1
// on error.
TDP_EXPORT int tdp_read_3da(const char *path, TdpProject *out);

// Allocate (or reallocate) arrays so that Python/FFI callers can build
// projects without manual malloc.  Each frees the old pointer first.
TDP_EXPORT void tdp_alloc_materials(TdpProject *proj, size_t count);
TDP_EXPORT void tdp_alloc_ctrl_regs(TdpProject *proj, size_t count);
TDP_EXPORT void tdp_alloc_part_anims(TdpLod *lod, size_t count);
TDP_EXPORT void tdp_alloc_lights(TdpLod *lod, size_t count);


#ifdef __cplusplus
}
#endif

#endif // TDP_H
