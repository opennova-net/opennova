// Typed 3DI3 format parsing utilities.
// This interprets GHDR/RLOD trees into C structs so we can diff our output
// against ModSuperOED layouts.

#ifndef THREEDI_3DI3_H
#define THREEDI_3DI3_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "threedi/threedi.h"

#if defined(_MSC_VER) && !defined(__cplusplus)
// MSVC's C mode lacks _Static_assert; alias to C++ static_assert.
#define _Static_assert static_assert
#endif

#pragma pack(push, 1)

#define THREEDI_VERTEX_FLAG_TANGENTS 0x14u
#define THREEDI_VERTEX_FLAG_SKINNED  0x40u

// Material flags (material_flags field in ThreediMaterial)
#define THREEDI_MATERIAL_FLAG_ALPHA_TEST   0x01u  // Enable alpha test/scissor
#define THREEDI_MATERIAL_FLAG_ALPHA_INVERT 0x02u  // Invert alpha test (1 - threshold)
#define THREEDI_MATERIAL_FLAG_TWO_SIDED    0x04u  // Disable backface culling

// Texture slot types (slot field in ThreediMaterialTexture)
#define THREEDI_TEX_SLOT_DIFFUSE  1  // Primary diffuse texture
#define THREEDI_TEX_SLOT_DETAIL   2  // Secondary/detail texture
#define THREEDI_TEX_SLOT_NORMAL   3  // Normal map texture

// Texture format types (type field in ThreediMaterialTexture)
#define THREEDI_TEX_TYPE_DIFFUSE     0  // Standard diffuse texture
#define THREEDI_TEX_TYPE_NORMAL_MDT  4  // Normal map from MDT format
#define THREEDI_TEX_TYPE_NORMAL_TGA  5  // Normal map from TGA alpha channel

// Texture flags (flags field in ThreediMaterialTexture)
#define THREEDI_TEX_FLAG_ANIMATED  0x01u  // Part of animation sequence
#define THREEDI_TEX_FLAG_CLAMPED   0x02u  // Use clamp addressing (vs wrap)

/* 3DI3 chunk-header dword: high bit = parent (has children), low 24 bits =
 * payload length. One home; the reader and writer TUs both use these. */
#define THREEDI_3DI3_PARENT_FLAG 0x80000000u
#define THREEDI_3DI3_LENGTH_MASK 0x00FFFFFFu

// Emissive type values (emissive_type field in ThreediMaterial)
#define THREEDI_EMISSIVE_NONE      0  // Not emissive
#define THREEDI_EMISSIVE_FULL      2  // Full emissive (LUM shader variants)

typedef enum ThreediMeshType {
    THREEDI_MESH_INVALID = 0,
    THREEDI_MESH_BASIC = 1,
    THREEDI_MESH_SKINNED = 2
} ThreediMeshType;

typedef struct ThreediHeader {
    int has_header;          // 1 if GHDR was found/parsed.
    char name[17];           // Null-terminated model name (from GHDR).
    ThreediMeshType mesh_type;
    int32_t lod_count_decl;  // Number of LODs reported in GHDR.
    int32_t lod_distance;
} ThreediHeader;

typedef struct ThreediInfo {
    uint8_t *data;
    size_t data_len;
} ThreediInfo;

typedef struct ThreediVertex {
    float position[3];
    float bone_weights[3];   // Only valid if is_skinned is set.
    uint8_t bone_indices[4]; // Only valid if is_skinned is set.
    float normal[3];
    float uv0[2];
    float uv1[2];
    float tangent[3];        // Only valid if has_tangents is set.
    float bitangent[3];      // Only valid if has_tangents is set.
    uint32_t flags;          // Raw flag from the VERT header.
    int has_tangents;
    int is_skinned;
} ThreediVertex;

typedef struct ThreediVertexBuffer {
    uint32_t count;
    uint32_t stride;
    uint32_t flags;
    ThreediVertex *items;
} ThreediVertexBuffer;

typedef struct ThreediIndexBuffer {
    uint32_t count;
    uint16_t *indices;
} ThreediIndexBuffer;

typedef struct ThreediTriangleStrip {
    int32_t material_index;
    int32_t index_offset;
    uint16_t num_indices;
    uint16_t num_triangles;
    int32_t is_strip;
    int32_t start_vertex;
    int32_t num_vertices;
    float min[3];
    float max[3];
    uint8_t bone_table[16];
    int32_t bone_table_length; // 0 if absent.
} ThreediTriangleStrip;

typedef struct ThreediRenderObject {
    int32_t num_strips;
    int32_t num_alpha_strips;
    int32_t parent_index;
    float rel[3];
    float abs[3];
    float bounding_center[3];
    float bounding_radius;
} ThreediRenderObject;

// Material texture slot definition
typedef struct ThreediMaterialTexture {
    char name[17];   // Texture filename (null-terminated)
    uint8_t slot;    // Texture slot: THREEDI_TEX_SLOT_DIFFUSE/DETAIL/NORMAL
    uint8_t type;    // Texture type: THREEDI_TEX_TYPE_DIFFUSE/NORMAL_MDT/NORMAL_TGA
    uint8_t flags;   // Texture flags: THREEDI_TEX_FLAG_ANIMATED/CLAMPED
    uint8_t frame;   // Animation frame index (0 for non-animated)
} ThreediMaterialTexture;

typedef struct ThreediAlphaGen {
    uint8_t style;
    float phase;
    int32_t reg; // -1 if unused
    float rate;
    int16_t start;
    int16_t end;
} ThreediAlphaGen;

typedef struct ThreediRgbGen {
    uint8_t style;
    float phase;
    int32_t reg; // -1 if unused
    float rate;
    float start_color[4]; // RGBA 0..1
    float end_color[4];   // RGBA 0..1
} ThreediRgbGen;

typedef struct ThreediUvParams {
    uint8_t style;
    float phase;
    int32_t reg; // -1 if unused
    float gen_rate;
    float start;
    float end;
} ThreediUvParams;

// Texture animation parameters
typedef struct ThreediTexAnim {
    uint8_t num_frames;       // Number of animation frames (0 = no animation)
    uint8_t animation_type;   // 0 = time-based, 1 = control register driven
    int16_t cycle_frame_time; // Time per frame (type=0) or control register index (type=1)
} ThreediTexAnim;

// Material definition from MTRL chunk
// Shader name determines rendering behavior:
//   FF_ST_* = Fixed-function single texture
//   FF_MT_* = Fixed-function multi-texture
//   VS_DOT3DIFF* = Vertex shader bump-mapped diffuse
//   VS_PHONG* = Vertex shader phong lighting
//   VS_SK* = Skinned mesh variants
//   *_LUM = Emissive/luminous variants
//   *_AB = Alpha-blended variants
//   *_AD = Additive blend variants
//   FFP_GLASS, VS_*GLASS = Glass/reflective materials
typedef struct ThreediMaterial {
    int32_t index;                       // Material index in MTRL table
    char shader_name[33];                // Shader tag (determines render behavior)
    uint32_t texture_count;              // Number of valid texture slots
    ThreediMaterialTexture textures[24]; // Texture slots (unused slots zeroed)
    uint8_t material_flags;              // THREEDI_MATERIAL_FLAG_* bits
    ThreediAlphaGen alpha_gen;           // Alpha animation parameters
    ThreediRgbGen rgb_gen;               // RGB color animation parameters (channel 0)
    ThreediRgbGen rgb_gen2;              // RGB color animation parameters (channel 1, always zero in practice)
    ThreediUvParams u_params;            // U-axis UV animation parameters
    ThreediUvParams v_params;            // V-axis UV animation parameters
    float reflect_color[4];              // Glass reflection color (BGRA, 0..1)
    float reflect_color2[4];             // Second reflect color (always zero in practice)
    uint8_t emissive_type;               // THREEDI_EMISSIVE_NONE or THREEDI_EMISSIVE_FULL
    uint8_t emissive_type2;              // Second emissive type (always 0 in practice)
    uint8_t is_glass;                    // 1 if reflective/env-mapped
    uint8_t glass_type2;                 // Second glass flag (always 0 in practice)
    uint8_t alpha_test_value_byte;       // Alpha test threshold (0-255, divide by 255 for 0..1)
    uint8_t pad[3];                      // Always 0
    ThreediTexAnim animation;
} ThreediMaterial;

typedef struct ThreediLight {
    float offset[3];
    float atten_start;
    float atten_end;
    uint8_t style;
    uint8_t phase;
    uint16_t rate;
    uint8_t color_start[4]; // packed B,G,R,unused
    uint8_t color_end[4];   // packed B,G,R,unused
    uint8_t subobj_index;
    uint8_t flags;
    uint8_t unknown1;
    uint8_t falloff_byte;  // (u8)falloff angle in degrees
    float rotation[4];    // { -rotY, rotZ, rotX, cos(falloff) }
    float view_proj[16];  // 4x4 matrix row-major
} ThreediLight;
#ifdef __cplusplus
static_assert(sizeof(ThreediLight) == 116, "ThreediLight layout mismatch");
#else
_Static_assert(sizeof(ThreediLight) == 116, "ThreediLight layout mismatch");
#endif

typedef struct ThreediUserPoint {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t rot_x;
    int32_t rot_y;
    int32_t rot_z;
    int32_t subobject_index;
    int32_t userpoint_type;
    char name[17];
} ThreediUserPoint;

typedef struct ThreediCollisionModelData {
    float bbox[6];                // {minX, minY, minZ, maxX, maxY, maxZ}
    float radii[3];               // {max_radius, max_radius_xy, max_radius_z}
    int32_t num_vertices;
    int32_t num_normals;
    int32_t num_faces;
    int32_t num_objects;
    int32_t num_transforms;
    int32_t num_bounding_planes;
    int32_t num_bounding_volumes;
} ThreediCollisionModelData;

typedef struct ThreediBoundingPlane {
    int16_t flags;
    float normal[3];
    float radius;
} ThreediBoundingPlane;

typedef struct ThreediBoundingVolume {
    int32_t collidable_type;   // remapped collidable type
    int32_t flags;             // bvolFlags (upper bytes are preserved)
    int32_t min_x_fp16;        // 16.16 fixed
    int32_t min_y_fp16;
    int32_t min_z_fp16;
    int32_t max_x_fp16;
    int32_t max_y_fp16;
    int32_t max_z_fp16;
    int32_t plane_count;
} ThreediBoundingVolume;
#ifdef __cplusplus
static_assert(sizeof(ThreediBoundingVolume) == 36, "ThreediBoundingVolume layout mismatch");
#else
_Static_assert(sizeof(ThreediBoundingVolume) == 36, "ThreediBoundingVolume layout mismatch");
#endif

typedef struct ThreediCollisionVertex {
    float position[3];
} ThreediCollisionVertex;

typedef struct ThreediCollisionNormal {
    float normal[3];
    int16_t dominate_axis;
} ThreediCollisionNormal;

typedef struct ThreediCollisionFace {
    int16_t vert_index[3];   // Local subobject vertex indices.
    int16_t normal_index;    // Index into CNRM for this subobject.
    int32_t plane_dist_fp16; // Plane distance in 16.16 fixed.
    int32_t min_x_fp16;      // Bounding box mins/maxes in 16.16 fixed.
    int32_t min_y_fp16;
    int32_t min_z_fp16;
    int32_t max_x_fp16;
    int32_t max_y_fp16;
    int32_t max_z_fp16;
    uint32_t material_flags; // Bitfield derived from material attributes.
    uint8_t poly_type;       // Surface type enum (matches legacy material surface).
    uint8_t pad[3];          // Unused padding bytes.
} ThreediCollisionFace;

typedef struct ThreediCollisionObject {
    int32_t unk0;
    int32_t num_vertices;
    int32_t num_faces;
    int32_t num_normals;        /* per-object CNRM run count (face normal_index is
                                   local to this run) [orig: the COBJ normal-run
                                   fixup in the collision builder @ 0x5b3bf0] */
    int32_t num_bounding_volumes;
    int32_t parent_subobject_index;
    int32_t unk3;
    int32_t unk4;
    int32_t unk5;
    int32_t offset[3];  // exact authored 16.16 integers
    int32_t min[3];
    int32_t max[3];
    int32_t med[3];
    int32_t radius;
} ThreediCollisionObject;

typedef struct ThreediCollisionTranslation {
    int32_t translation[3]; // exact authored 16.16 integers
} ThreediCollisionTranslation;

#ifdef __cplusplus
static_assert(sizeof(ThreediCollisionObject) == 88, "ThreediCollisionObject layout mismatch");
static_assert(sizeof(ThreediCollisionTranslation) == 12, "ThreediCollisionTranslation layout mismatch");
#else
_Static_assert(sizeof(ThreediCollisionObject) == 88, "ThreediCollisionObject layout mismatch");
_Static_assert(sizeof(ThreediCollisionTranslation) == 12, "ThreediCollisionTranslation layout mismatch");
#endif

typedef struct ThreediCollisionModel {
    ThreediCollisionModelData model_data; // CMDL
    ThreediBoundingPlane *planes;
    size_t plane_count;
    ThreediBoundingVolume *volumes;
    size_t volume_count;
    ThreediCollisionVertex *vertices;
    size_t vertex_count;
    ThreediCollisionNormal *normals;
    size_t normal_count;
    ThreediCollisionFace *faces;
    size_t face_count;
    ThreediCollisionObject *objects;
    size_t object_count;
    ThreediCollisionTranslation *translations;
    size_t translation_count;
} ThreediCollisionModel;

typedef struct ThreediOcclusionVertex {
    float position[3];
} ThreediOcclusionVertex;

typedef struct ThreediOcclusionFace {
    uint32_t raw_indices;
    uint32_t edge_data;
    uint32_t other_edge_data;
} ThreediOcclusionFace;

typedef struct ThreediOcclusionObject {
    uint8_t type;
    uint8_t parent_subobject_index;
    uint8_t connecting_subobject;
    uint8_t unused0;
    float position[3];
    float radius;
    float glow_scale; /* [orig: OOBJ disk +20 -> runtime record +0x2C - the
                         window-glow scale the slot collector consumes
                         @ 0x5c6ea3; zero across the JO 3DI3 corpus] */
    int32_t num_vertices;
    int32_t num_planes;
    int32_t face_count;
} ThreediOcclusionObject;

typedef struct ThreediOcclusionPlane {
    float normal[3];
    float radius;
} ThreediOcclusionPlane;

// Raw PANM track (matches on-disk layout; no implicit flags/presence bits).
typedef struct ThreediTransform {
    uint8_t control;
    uint8_t control_param;
    int16_t rate;
    int16_t start;
    int16_t end;
} ThreediTransform;

// PANM node (matches IDA PanmNode layout; 0x44 bytes on disk).
typedef struct ThreediPartAnimation {
    uint32_t flags;          // Packed flags; see threedi_panm_flags_* helpers.
    uint8_t parent_subobject;
    uint8_t subobject_index; // transform_as
    uint8_t matrix_index;
    uint8_t matrix_offset;
    int32_t bind_matrix_index;
    ThreediTransform rotation_x;
    ThreediTransform rotation_y;
    ThreediTransform rotation_z;
    ThreediTransform scale_x;
    ThreediTransform scale_y;
    ThreediTransform scale_z;
    ThreediTransform translation;
} ThreediPartAnimation;

#ifdef __cplusplus
static_assert(sizeof(ThreediTransform) == 8, "ThreediTransform layout mismatch");
static_assert(sizeof(ThreediPartAnimation) == 0x44, "ThreediPartAnimation layout mismatch");
#else
_Static_assert(sizeof(ThreediTransform) == 8, "ThreediTransform layout mismatch");
_Static_assert(sizeof(ThreediPartAnimation) == 0x44, "ThreediPartAnimation layout mismatch");
#endif

typedef struct ThreediControlRegister {
    char name[25];
} ThreediControlRegister;

typedef struct ThreediCtrl {
    uint32_t count;
    uint32_t record_size;
    ThreediControlRegister *registers;
} ThreediCtrl;

typedef struct ThreediRawTable {
    uint32_t count;
    uint32_t record_size;
    uint8_t *data;
    size_t data_len;
} ThreediRawTable;

typedef struct ThreediVec3 {
    float x, y, z;
} ThreediVec3;

typedef struct ThreediMatrix4x4 {
    float m[16];  // row-major: m[0-3]=row0, m[4-7]=row1, m[8-11]=row2, m[12-15]=row3
} ThreediMatrix4x4;

// Matrix helper functions (row-major, row-vector convention: p' = p * M)
#ifdef __cplusplus
extern "C" {
#endif

static inline void threedi_mat4_identity(ThreediMatrix4x4 *out) {
    memset(out->m, 0, sizeof(out->m));
    out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

static inline void threedi_mat4_copy(ThreediMatrix4x4 *dst, const ThreediMatrix4x4 *src) {
    memcpy(dst->m, src->m, sizeof(dst->m));
}

static inline void threedi_mat4_zero_translation(ThreediMatrix4x4 *m) {
    m->m[12] = m->m[13] = m->m[14] = 0.0f;
}

static inline void threedi_mat4_set_translation(ThreediMatrix4x4 *m, float x, float y, float z) {
    m->m[12] = x;
    m->m[13] = y;
    m->m[14] = z;
}

static inline void threedi_mat4_add_translation(ThreediMatrix4x4 *m, float x, float y, float z) {
    m->m[12] += x;
    m->m[13] += y;
    m->m[14] += z;
}

// out = a * b (affine only)
static inline void threedi_mat4_mul_affine(ThreediMatrix4x4 *out, const ThreediMatrix4x4 *a, const ThreediMatrix4x4 *b) {
    ThreediMatrix4x4 r;
    r.m[0]  = a->m[0] * b->m[0]  + a->m[1] * b->m[4]  + a->m[2] * b->m[8];
    r.m[1]  = a->m[0] * b->m[1]  + a->m[1] * b->m[5]  + a->m[2] * b->m[9];
    r.m[2]  = a->m[0] * b->m[2]  + a->m[1] * b->m[6]  + a->m[2] * b->m[10];
    r.m[3]  = 0.0f;
    r.m[4]  = a->m[4] * b->m[0]  + a->m[5] * b->m[4]  + a->m[6] * b->m[8];
    r.m[5]  = a->m[4] * b->m[1]  + a->m[5] * b->m[5]  + a->m[6] * b->m[9];
    r.m[6]  = a->m[4] * b->m[2]  + a->m[5] * b->m[6]  + a->m[6] * b->m[10];
    r.m[7]  = 0.0f;
    r.m[8]  = a->m[8] * b->m[0]  + a->m[9] * b->m[4]  + a->m[10]* b->m[8];
    r.m[9]  = a->m[8] * b->m[1]  + a->m[9] * b->m[5]  + a->m[10]* b->m[9];
    r.m[10] = a->m[8] * b->m[2]  + a->m[9] * b->m[6]  + a->m[10]* b->m[10];
    r.m[11] = 0.0f;
    r.m[12] = a->m[12]* b->m[0]  + a->m[13]* b->m[4]  + a->m[14]* b->m[8]  + b->m[12];
    r.m[13] = a->m[12]* b->m[1]  + a->m[13]* b->m[5]  + a->m[14]* b->m[9]  + b->m[13];
    r.m[14] = a->m[12]* b->m[2]  + a->m[13]* b->m[6]  + a->m[14]* b->m[10] + b->m[14];
    r.m[15] = 1.0f;
    threedi_mat4_copy(out, &r);
}

static inline void threedi_mat4_make_translation(ThreediMatrix4x4 *out, float x, float y, float z) {
    threedi_mat4_identity(out);
    threedi_mat4_set_translation(out, x, y, z);
}

// p' = p * M (with translation)
static inline void threedi_mat4_apply_point(const ThreediMatrix4x4 *m, const float v[3], float out[3]) {
    out[0] = v[0] * m->m[0] + v[1] * m->m[4] + v[2] * m->m[8]  + m->m[12];
    out[1] = v[0] * m->m[1] + v[1] * m->m[5] + v[2] * m->m[9]  + m->m[13];
    out[2] = v[0] * m->m[2] + v[1] * m->m[6] + v[2] * m->m[10] + m->m[14];
}

// v' = v * M (without translation, rotation only)
static inline void threedi_mat4_apply_vec3(const ThreediMatrix4x4 *m, const float v[3], float out[3]) {
    out[0] = v[0] * m->m[0] + v[1] * m->m[4] + v[2] * m->m[8];
    out[1] = v[0] * m->m[1] + v[1] * m->m[5] + v[2] * m->m[9];
    out[2] = v[0] * m->m[2] + v[1] * m->m[6] + v[2] * m->m[10];
}

// Rotation around Y-axis
static inline void threedi_mat4_make_rot_y(ThreediMatrix4x4 *out, float angle) {
    threedi_mat4_identity(out);
    float c = cosf(angle);
    float s = sinf(angle);
    out->m[0]  = c;
    out->m[2]  = -s;
    out->m[8]  = s;
    out->m[10] = c;
}

// Rotation around X-axis
static inline void threedi_mat4_make_rot_x(ThreediMatrix4x4 *out, float angle) {
    threedi_mat4_identity(out);
    float c = cosf(angle);
    float s = sinf(angle);
    out->m[5]  = c;
    out->m[6]  = s;
    out->m[9]  = -s;
    out->m[10] = c;
}

// Rotation around Z-axis
static inline void threedi_mat4_make_rot_z(ThreediMatrix4x4 *out, float angle) {
    threedi_mat4_identity(out);
    float c = cosf(angle);
    float s = sinf(angle);
    out->m[0] = c;
    out->m[1] = s;
    out->m[4] = -s;
    out->m[5] = c;
}

#ifdef __cplusplus
}
#endif

typedef struct ThreediMatrixTable {
    uint32_t count;
    uint32_t record_size;
    ThreediMatrix4x4 *matrices;  // Array of count matrices
} ThreediMatrixTable;

typedef struct ThreediLod {
    char model_type[5]; // From RMDL
    int32_t lod_threshold;
    int32_t rmdl_render_object_count; // As declared in RMDL; may differ from actual ROBJ count.

    ThreediVertexBuffer vertices;
    ThreediIndexBuffer indices;

    ThreediTriangleStrip *strips;
    size_t strip_count;
    uint32_t strip_record_size;

    ThreediRenderObject *render_objects;
    size_t render_object_count;

    // Per-LOD part animations (PANM is written per-RLOD in the original).
    ThreediPartAnimation *part_animations;
    size_t part_animation_count;
    uint32_t part_animation_record_size;
} ThreediLod;

typedef struct Threedi3di3 {
    uint32_t version;
    ThreediHeader header;
    ThreediInfo info;
    ThreediLod *lods;
    size_t lod_count;

    // Materials (MTRL)
    uint32_t material_count;
    uint32_t material_record_size;
    struct ThreediMaterial *materials;

    // Lights (LGHT)
    struct ThreediLight *lights;
    size_t light_count;

    // User points (USRP)
    struct ThreediUserPoint *user_points;
    size_t user_point_count;

    // Collision data
    struct ThreediCollisionModel *collision;

    // Misc chunks
    ThreediCtrl ctrl;
    ThreediRawTable ovrt;
    ThreediMatrixTable mtrx;

    // Occlusion (OCCL children)
    ThreediOcclusionVertex *occlusion_vertices;
    size_t occlusion_vertex_count;
    uint32_t occlusion_vertex_record_size;
    ThreediOcclusionFace *occlusion_faces;
    size_t occlusion_face_count;
    uint32_t occlusion_face_record_size;
    ThreediOcclusionObject *occlusion_objects;
    size_t occlusion_object_count;
    uint32_t occlusion_object_record_size;
    ThreediOcclusionPlane *occlusion_planes;
    size_t occlusion_plane_count;
    uint32_t occlusion_plane_record_size;

    // Part animations (PANM)
    ThreediPartAnimation *part_animations;
    size_t part_animation_count;
    uint32_t part_animation_record_size;

} Threedi3di3;

// Parse a ThreediFile's chunk tree into typed geometry structures.
// Returns 0 on success, -1 on parse/validation errors.
int threedi_3di3_parse(const ThreediFile *file, Threedi3di3 *out_model);

// Convenience: read a file from disk and parse it into a Threedi3di3.
int threedi_3di3_read(const char *path, Threedi3di3 *out_model);

// Convenience: parse memory-backed 3DI bytes into a Threedi3di3.
int threedi_3di3_read_memory(const uint8_t *data, size_t size, Threedi3di3 *out_model);

// Convenience: write a previously-read model back to disk (round-trip).
int threedi_3di3_write(const char *path, const Threedi3di3 *model);

// Free allocations inside a Threedi3di3.
void threedi_3di3_free(Threedi3di3 *model);

#pragma pack(pop)
#endif // THREEDI_3DI3_H
