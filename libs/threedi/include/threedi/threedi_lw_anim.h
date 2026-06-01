// Land Warrior animation formats: SAF1 (loose single clip), KSA (baked bundle),
// ACA (slot->saf text manifest), ANM (name->slot text manifest).
//
// All four are consumed by Dflw.exe's LWAnim_* loaders. The on-disk SAF1 clip is
// converted at load into a contiguous run of 88-byte "runtime frames"; KSA stores
// those 88-byte frames pre-baked. This module reproduces that conversion byte-exactly
// and provides a pose sampler that turns a runtime frame + LW geometry sub-objects
// into per-bone local transforms for the Blender BAD-style consumer.
//
// IDA refs (Dflw.exe, base 0x400000):
//   LWAnim_LoadSAF1      @ 0x44A0B0
//   LWAnim_LoadKsa       @ 0x449F50
//   LWAnim_LoadAca       @ 0x44AC50 / LWAnim_ParseAcaLine @ 0x44AB30
//   LWAnim_LoadAnm       @ 0x44AA60 / LWAnim_ParseAnmLine @ 0x44A6D0
//   LWAnim_PoseSkeleton  @ 0x4A0C00  (pose recipe: notes/3di-lw/lw-saf-pose.md)

#ifndef THREEDI_LW_ANIM_H
#define THREEDI_LW_ANIM_H

#include <stddef.h>
#include <stdint.h>

#include "threedi/threedi_lw.h" // ThreediLwSubObject (rest offsets, parent)

// Export macro (mirrors threedi_ir.h THREEDI_EXPORT; named distinctly to avoid a
// redefinition clash if both headers are included in one TU). dllexport only when
// building the shared FFI target (OPENNOVA_SHARED_EXPORTS), empty for static/tests.
#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_LW_ANIM_EXPORT __declspec(dllexport)
#  else
#    define THREEDI_LW_ANIM_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_LW_ANIM_EXPORT __attribute__((visibility("default")))
#  else
#    define THREEDI_LW_ANIM_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ----- on-disk constants (IDA-verified) ----------------------------------- */

#define THREEDI_LW_SAF1_MAGIC         0x31464153u /* 'SAF1' LE = 53 41 46 31      */
#define THREEDI_LW_SAF1_HEADER_SIZE   0x10        /* 16-byte file header          */
#define THREEDI_LW_SAF1_FRAME_HDR     0x34        /* 52-byte per-frame header     */
#define THREEDI_LW_SAF1_PART_REC      4           /* on-disk part record size     */

#define THREEDI_LW_KSA_HEADER_SIZE    0x64        /* 100-byte fixed header        */
#define THREEDI_LW_KSA_VERSION        1u
#define THREEDI_LW_KSA_SLOT_REC       28          /* 7 dwords per slot record     */
#define THREEDI_LW_KSA_MAX_SLOTS      256         /* in-memory cap (idx < 0x100)  */

#define THREEDI_LW_RUNTIME_FRAME_SIZE 88          /* 0x58 runtime frame stride    */
#define THREEDI_LW_MAX_PARTS          15          /* part loop capped at 15       */
#define THREEDI_LW_ROOT_I16_COUNT     9           /* 9 i16 root values            */

/* ----- runtime 88-byte frame ---------------------------------------------- */
/* Layout (matches LWAnim_LoadSAF1 output and KSA on-disk frames):
 *   +0x00..+0x3B : 15 part records, 4 bytes each {b0,b1,b2,pad}
 *   +0x3C..+0x4D : 9 little-endian signed i16 root values
 *   +0x4E..+0x57 : 10 pad bytes (zero) */
typedef struct ThreediLwAnimPartRec {
    uint8_t b0;  /* Rz Euler byte. SAF on disk: (disk_b0 + 0x80) & 0xFF.   */
    uint8_t b1;  /* Ry Euler byte. Copied verbatim.                        */
    uint8_t b2;  /* Rx Euler byte. Copied verbatim.                        */
    uint8_t pad; /* Unused by pose builder; zeroed for determinism.        */
} ThreediLwAnimPartRec;

typedef struct ThreediLwAnimFrame {
    ThreediLwAnimPartRec parts[THREEDI_LW_MAX_PARTS]; /* 60 bytes */
    int16_t root[THREEDI_LW_ROOT_I16_COUNT];          /* 18 bytes (+0x3C..+0x4D) */
    uint8_t zero_pad[10];                             /* 10 bytes (+0x4E..+0x57) */
} ThreediLwAnimFrame;

/* ----- SAF1 single clip --------------------------------------------------- */
typedef struct ThreediLwSaf {
    uint32_t version;     /* header dword[1], loaded but unused by engine */
    uint32_t frame_count; /* header dword[2] @ file off 0x08              */
    ThreediLwAnimFrame *frames; /* frame_count entries, calloc-owned      */
} ThreediLwSaf;

/* ----- KSA baked bundle --------------------------------------------------- */
/* One slot = one animation; its frames are pre-baked 88-byte runtime frames. */
typedef struct ThreediLwKsaSlot {
    uint32_t frame_count; /* rec+0x00 (the only field the KSA loader reads) */
    uint32_t stored_slot; /* rec+0x08 baked authoring slot id (loader ignores) */
    uint32_t loop_frame;  /* rec+0x0C baked loop-target frame (loader ignores) */
    uint32_t frame_base;  /* index into ThreediLwKsa.frames for this slot's first frame */
    ThreediLwAnimFrame *frames; /* aliases ksa->frames + frame_base; NOT separately owned */
} ThreediLwKsaSlot;

typedef struct ThreediLwKsa {
    uint32_t version;     /* header @0x04, must == 1                 */
    uint32_t blob_size;   /* header @0x2C: bytes after the 100B header */
    uint32_t slot_count;  /* header @0x34                            */
    uint32_t total_frames;/* sum of slot frame_counts                */
    ThreediLwKsaSlot *slots;    /* slot_count entries, calloc-owned   */
    ThreediLwAnimFrame *frames; /* total_frames entries (flat), calloc-owned */
} ThreediLwKsa;

/* ----- ACA text manifest:  'slot <slot_id> <filename> [loop_frame]' -------- */
typedef struct ThreediLwAcaEntry {
    uint32_t slot_id;      /* token 1 (atoi)             */
    char     saf_name[64]; /* token 2 (the .saf filename) */
    int32_t  loop_frame;   /* token 3 if present, else -1 */
    int      has_loop;     /* 1 if a 4th token was present */
} ThreediLwAcaEntry;

typedef struct ThreediLwAca {
    uint32_t entry_count;
    ThreediLwAcaEntry *entries; /* calloc-owned */
} ThreediLwAca;

/* ----- ANM text manifest:  '<name> <slot> [velocity] [override]' ----------- */
typedef struct ThreediLwAnmEntry {
    char     name[64];     /* token 0: animation name        */
    uint32_t slot;         /* token 1 (atoi)                 */
    float    velocity;     /* token 2 if present, else 0.0f  */
    int32_t  override_val; /* token 3 if present, else -1    */
    int      has_velocity; /* 1 if token 2 present           */
    int      has_override; /* 1 if token 3 present           */
} ThreediLwAnmEntry;

typedef struct ThreediLwAnm {
    uint32_t entry_count;
    ThreediLwAnmEntry *entries; /* calloc-owned */
} ThreediLwAnm;

/* ----- pose sampler output ------------------------------------------------ */
/* Per-bone LOCAL transform (parent.world^-1 * bone.world), ready for the
 * Blender BAD-style world->rest-local pass. Row-major 3x4 [R|t]: local[r][0..2]
 * = rotation rows, local[r][3] = translation (in pos_scale units). */
typedef struct ThreediLwBonePose {
    float local[3][4];
} ThreediLwBonePose;

typedef struct ThreediLwAnimPose {
    uint32_t bone_count;      /* == subobject_count of the geometry LOD */
    ThreediLwBonePose *bones; /* bone_count entries, calloc-owned        */
} ThreediLwAnimPose;

/* ----- detect helpers (cheap magic probes) -------------------------------- */
THREEDI_LW_ANIM_EXPORT int threedi_lw_anim_detect_saf(const uint8_t *data, size_t len);
THREEDI_LW_ANIM_EXPORT int threedi_lw_anim_detect_ksa(const uint8_t *data, size_t len);

/* ----- SAF1 --------------------------------------------------------------- */
THREEDI_LW_ANIM_EXPORT void threedi_lw_saf_init(ThreediLwSaf *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_saf_parse(const uint8_t *data, size_t len, ThreediLwSaf *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_saf_read(const char *path, ThreediLwSaf *out);
THREEDI_LW_ANIM_EXPORT void threedi_lw_saf_free(ThreediLwSaf *out);

/* ----- KSA ---------------------------------------------------------------- */
THREEDI_LW_ANIM_EXPORT void threedi_lw_ksa_init(ThreediLwKsa *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_ksa_parse(const uint8_t *data, size_t len, ThreediLwKsa *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_ksa_read(const char *path, ThreediLwKsa *out);
THREEDI_LW_ANIM_EXPORT void threedi_lw_ksa_free(ThreediLwKsa *out);

/* ----- ACA (text) --------------------------------------------------------- */
THREEDI_LW_ANIM_EXPORT void threedi_lw_aca_init(ThreediLwAca *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_aca_parse(const uint8_t *data, size_t len, ThreediLwAca *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_aca_read(const char *path, ThreediLwAca *out);
THREEDI_LW_ANIM_EXPORT void threedi_lw_aca_free(ThreediLwAca *out);

/* ----- ANM (text) --------------------------------------------------------- */
THREEDI_LW_ANIM_EXPORT void threedi_lw_anm_init(ThreediLwAnm *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_anm_parse(const uint8_t *data, size_t len, ThreediLwAnm *out);
THREEDI_LW_ANIM_EXPORT int  threedi_lw_anm_read(const char *path, ThreediLwAnm *out);
THREEDI_LW_ANIM_EXPORT void threedi_lw_anm_free(ThreediLwAnm *out);

/* ----- skeleton rest data (reads a model's LOD sub-objects for the sampler) --
 * Allocates *out_subs (out_count entries, free via threedi_lw_anim_free_skeleton),
 * and reports the LOD flags (bit 0 = skinned). Returns 0 on success, -1 on error.
 * Internally uses threedi_lw_read so callers need not export it. */
THREEDI_LW_ANIM_EXPORT int  threedi_lw_anim_read_skeleton(const char *path, uint32_t lod,
                                                          ThreediLwSubObject **out_subs,
                                                          uint32_t *out_count, uint32_t *out_flags);
THREEDI_LW_ANIM_EXPORT void threedi_lw_anim_free_skeleton(ThreediLwSubObject *subs);

/* ----- pose sampler ------------------------------------------------------- */
THREEDI_LW_ANIM_EXPORT void threedi_lw_anim_pose_init(ThreediLwAnimPose *out);
THREEDI_LW_ANIM_EXPORT void threedi_lw_anim_pose_free(ThreediLwAnimPose *out);

/* Sample one runtime frame into per-bone LOCAL transforms.
 *   frame         : the 88-byte runtime frame (from SAF/KSA).
 *   subobjects    : geometry sub-object array (parent + rest offset source).
 *   subobject_cnt : number of sub-objects/bones (<= 15).
 *   pos_scale     : fixed-point->float scale for rest offsets / root translation
 *                   (pass 1.0f to keep raw units; the consumer scales later).
 *   out           : filled with subobject_cnt bone poses (calloc-owned).
 * Returns 0 on success, -1 on error. Implements LWAnim_PoseSkeleton @ 0x4A0C00
 * with gameplay modifiers (heading / arm-aim clamp) SKIPPED. */
THREEDI_LW_ANIM_EXPORT int threedi_lw_anim_sample(const ThreediLwAnimFrame *frame,
                           const ThreediLwSubObject *subobjects,
                           uint32_t subobject_cnt,
                           float pos_scale,
                           ThreediLwAnimPose *out);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_LW_ANIM_H
