// Typed 3DI3 format parsing utilities.
// This interprets GHDR/RLOD trees into stable native C structs.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <string_view>
#include <vector>
#include <base/io/strutil.h>
#include <formats/threedi/threedi.h>

#pragma pack(push, 1)

namespace opennova::threedi {

inline constexpr uint32_t THREEDI_VERTEX_FLAG_TANGENTS = 0x14u;
inline constexpr uint32_t THREEDI_VERTEX_FLAG_SKINNED = 0x40u;

// Material flags (material_flags field in ThreediMaterial)
inline constexpr uint32_t THREEDI_MATERIAL_FLAG_ALPHA_TEST = 0x01u;  // Enable alpha test/scissor
inline constexpr uint32_t THREEDI_MATERIAL_FLAG_ALPHA_INVERT = 0x02u;  // Invert alpha test (1 - threshold)
inline constexpr uint32_t THREEDI_MATERIAL_FLAG_TWO_SIDED = 0x04u;  // Disable backface culling

// Texture slot types (slot field in ThreediMaterialTexture)
inline constexpr int THREEDI_TEX_SLOT_DIFFUSE = 1;  // Primary diffuse texture
inline constexpr int THREEDI_TEX_SLOT_DETAIL = 2;  // Secondary/detail texture
inline constexpr int THREEDI_TEX_SLOT_NORMAL = 3;  // Normal map texture
inline constexpr int THREEDI_TEX_SLOT_NORMAL_B = 4;  // Secondary normal map texture

/* Light flags byte (ThreediLight::flags): bits 0-2 disable legs, bit 3 the
 * light type (0 = Omni, 1 = Target). */
inline constexpr uint32_t THREEDI_LIGHT_FLAG_DISABLE_CORONA = 0x01u;
inline constexpr uint32_t THREEDI_LIGHT_FLAG_DISABLE_TERRAIN = 0x02u;
inline constexpr uint32_t THREEDI_LIGHT_FLAG_DISABLE_OBJECTS = 0x04u;
inline constexpr uint32_t THREEDI_LIGHT_FLAG_TYPE_TARGET = 0x08u;

// Texture format types (type field in ThreediMaterialTexture)
inline constexpr int THREEDI_TEX_TYPE_DIFFUSE = 0;  // Standard diffuse texture
inline constexpr int THREEDI_TEX_TYPE_NORMAL_MDT = 4;  // Normal map from MDT format
inline constexpr int THREEDI_TEX_TYPE_NORMAL_TGA = 5;  // Normal map from TGA alpha channel

// Texture flags (flags field in ThreediMaterialTexture)
inline constexpr uint32_t THREEDI_TEX_FLAG_ANIMATED = 0x01u;  // Part of animation sequence
// Bind the batch's render-state override texture in place of this row when
// one is pushed; with no override the row's own texture binds. The only
// pusher is the player preview, and the blip field it pushes is only ever
// written zero, so every draw binds the row's own texture.
// [orig: Material_ApplyShaderParameters @ 0x58DC92..0x58DCC1;
//  PlayerInfo_RenderPlayerPreview3D @ 0x560F43;
//  MinimapSlot_InitBlipFromPackedId @ 0x57B12D]
inline constexpr uint32_t THREEDI_TEX_FLAG_STATE_OVERRIDE = 0x02u;

/* 3DI3 chunk-header dword: high bit = parent (has children), low 24 bits =
 * payload length. One home; the reader and writer TUs both use these. */
inline constexpr uint32_t THREEDI_3DI3_PARENT_FLAG = 0x80000000u;
inline constexpr uint32_t THREEDI_3DI3_LENGTH_MASK = 0x00FFFFFFu;

// Emissive type values (emissive_type field in ThreediMaterial)
inline constexpr int THREEDI_EMISSIVE_NONE = 0;  // Not emissive
inline constexpr int THREEDI_EMISSIVE_FULL = 2;  // Full emissive (LUM shader variants)

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
    int32_t max_radius_fp16; // GHDR +24: max derived LOD radius, unsigned Q16.16 on disk.
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

// A strip's STRP bone table holds at most 16 slots: the palette the strip
// uploads, one matrix per entry, which every skinned vertex shader indexes.
inline constexpr int32_t kThreediStripBoneTableMax = 16;

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
    uint8_t bone_table[kThreediStripBoneTableMax];
    int32_t bone_table_length; // 0 if absent.
} ThreediTriangleStrip;

// One influence of a skinned vertex: the bone-table slot its index byte
// names, the part that slot of the strip's table holds (-1 for a slot past
// the table: the palette uploaded for the strip holds no matrix there, so
// retail reads whatever that constant last held), and its weight.
typedef struct ThreediSkinInfluence {
    int32_t slot;
    int32_t part;
    float weight;
} ThreediSkinInfluence;

// A skinned vertex's four influences as the retail vertex shader blends
// them: the three stored weights on slots bone_indices[0..2], and the
// remainder 1 - (w0 + w1 + w2) on slot bone_indices[3], the sum added in
// float in slot order and never renormalized. A vertex with no stored
// weight rides slot 3 wholly; one whose weights sum past 1 gives slot 3 a
// small negative weight (retail's four-decimal weights reach 1.0001,
// ArmGlovD). `bone_table` is the strip's table (STRP), at most 16 slots.
// The loader copies the three weights and the four index bytes verbatim;
// the declaration feeds them as BLENDWEIGHT FLOAT3 and BLENDINDICES
// D3DCOLOR, which D3DCOLORtoUBYTE4 turns back into index byte k =
// IndexArray[k]; the strip's palette entry k is its bone-table entry k's
// matrix; every skinned vertex shader compiles NumBones 4.
// [orig: ThreediGp_ConvertVerticesToGPUFormat @ 0x5B4C90 (the weights
// @ 0x5B4E2C); D3DDevice_CreateVertexDeclarations @ 0x5B0A00;
// CRenderBatchQueue_FlushBatches @ 0x5DA170; _BaseInc.fx
// CalcSkinWorldPosAndNormal: lastweight = 1 - (w0 + w1 + w2) on
// IndexArray[NumBones - 1]]
static inline void threedi_skin_influences(const ThreediVertex *v, const uint8_t *bone_table,
                                           int32_t bone_table_length, ThreediSkinInfluence out[4]) {
    float sum = 0.0f;
    for (int k = 0; k < 3; ++k) sum += v->bone_weights[k];
    for (int k = 0; k < 4; ++k) {
        const int32_t slot = v->bone_indices[k];
        out[k].slot = slot;
        out[k].part = slot < bone_table_length && slot < kThreediStripBoneTableMax ? bone_table[slot] : -1;
        out[k].weight = k < 3 ? v->bone_weights[k] : 1.0f - sum;
    }
}

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
    uint8_t flags;   // Texture flags: THREEDI_TEX_FLAG_ANIMATED/STATE_OVERRIDE
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
static_assert(sizeof(ThreediLight) == 116, "ThreediLight layout mismatch");

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

// Decode a userpoint's authored 16.16 position into model-space floats.
// Source axes swizzle as x->z, y->x, z->y, with the side axis mirrored so a
// consumer's render-space -X transform preserves the authored driver/passenger
// side. Every consumer (Godot runtime and ground anchor) applies this one
// convention.
static inline void threedi_user_point_position(const ThreediUserPoint *up, float out[3]) {
    out[0] = -(float)up->y / 65536.0f;
    out[1] = (float)up->z / 65536.0f;
    out[2] = (float)up->x / 65536.0f;
}

// rot_x/y/z are the local Z-axis direction unit vector, stored 16.16 with the
// same swizzle as the position.
static inline void threedi_user_point_direction(const ThreediUserPoint *up, float out[3]) {
    out[0] = -(float)up->rot_y / 65536.0f;
    out[1] = (float)up->rot_z / 65536.0f;
    out[2] = (float)up->rot_x / 65536.0f;
}

typedef struct ThreediCollisionModelData {
    float bbox[6];                // {minX, minY, minZ, maxX, maxY, maxZ}
    int32_t bbox_fp16[6];         // exact CMDL words for entity projection bounds
    int has_bbox_fp16;            // 1 when CMDL was parsed; hand-built models may use bbox
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
static_assert(sizeof(ThreediBoundingVolume) == 36, "ThreediBoundingVolume layout mismatch");

typedef struct ThreediCollisionVertex {
    float position[3];
} ThreediCollisionVertex;

typedef struct ThreediCollisionNormal {
    float normal[3];
    int16_t dominate_axis;
} ThreediCollisionNormal;

// The corner and CNRM indices are SIGNED 16-bit words, as every retail
// reader takes them (movsx), so a section addresses at most 32,768 vertices:
// a corner past 32,767 reads before the section's vertex table (retail
// Pinegr_L's 40,824-vertex section is broken in retail too).
// [orig: Math_PointInTriangle2D @ 0x414071..0x414095 (the corners, shared by
// every ray path); Physics_RaycastAgainstBoneCollision @ 0x4E5079 (the normal
// index); Entity_SpawnSectionDebris @ 0x43F5F6..0x43F5FE]
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

static_assert(sizeof(ThreediCollisionObject) == 88, "ThreediCollisionObject layout mismatch");
static_assert(sizeof(ThreediCollisionTranslation) == 12, "ThreediCollisionTranslation layout mismatch");

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

// Per-COBJ run starts into the object-contiguous CVRT/CNRM/CFAC/BVOL pools.
// The on-disk pools carry no explicit starts: each COBJ owns the next
// num_vertices/num_normals/num_faces/num_bounding_volumes entries, exactly the
// prefix-sum fixup retail's collision builder performs at load
// [orig: the per-COBJ normal-run fixup in the collision builder @ 0x5b3bf0].
typedef struct ThreediCollisionObjectRun {
    int32_t vertex_start;
    int32_t normal_start;
    int32_t face_start;
    int32_t volume_start;
} ThreediCollisionObjectRun;

// Fill out[0..object_count) with each COBJ's run starts. Returns 1 on success,
// 0 when a count is negative or a run leaves its pool (out contents undefined).
static inline int threedi_collision_object_runs(const ThreediCollisionModel *col,
                                                ThreediCollisionObjectRun *out) {
    if (!col || (!out && col->object_count != 0)) return 0;
    size_t vertex_cursor = 0, normal_cursor = 0, face_cursor = 0, volume_cursor = 0;
    for (size_t oi = 0; oi < col->object_count; ++oi) {
        const ThreediCollisionObject *object = &col->objects[oi];
        if (object->num_vertices < 0 || object->num_faces < 0 || object->num_normals < 0 ||
            object->num_bounding_volumes < 0) return 0;
        out[oi].vertex_start = (int32_t)vertex_cursor;
        out[oi].normal_start = (int32_t)normal_cursor;
        out[oi].face_start = (int32_t)face_cursor;
        out[oi].volume_start = (int32_t)volume_cursor;
        if ((size_t)object->num_vertices > col->vertex_count - vertex_cursor ||
            (size_t)object->num_normals > col->normal_count - normal_cursor ||
            (size_t)object->num_faces > col->face_count - face_cursor ||
            (size_t)object->num_bounding_volumes > col->volume_count - volume_cursor)
            return 0;
        vertex_cursor += (size_t)object->num_vertices;
        normal_cursor += (size_t)object->num_normals;
        face_cursor += (size_t)object->num_faces;
        volume_cursor += (size_t)object->num_bounding_volumes;
    }
    return 1;
}

/* The runtime-safety gates over the raw collision block, one per pool section.
   Retail's loader copies every pool as authored and links the COBJ runs by
   prefix sum with no check of its own [orig: Threedi_BuildCollisionModelFromChunks
   @ 0x5B3BF0: the CFAC corners and normal copied as words @ 0x5B3EC7..0x5B3EEA,
   the per-COBJ vertex/face/normal/volume runs @ 0x5B4326..0x5B43C4, the BPLN
   windows @ 0x5B43FA], so a corner past its section's run reads outside it
   (docs/threedi/3di-gp-format-re.md section 2.11: Pinegr_L, broken in retail).
   The face mesh (CVRT/CNRM/CFAC through the COBJ runs) and the volumes
   (BVOL/BPLN through the COBJ runs) are separate pools that no walker crosses,
   so each is gated on its own: a consumer keeps the section it can walk
   safely. Header-local so validation does not expand the stable ABI. */

// Return 1 when the face mesh is safe for runtime queries: backing arrays
// exist, COBJ-owned vertex/normal/face runs are contiguous and exactly
// partition their pools, and local CFAC indices stay within their object.
// Object-less legacy convex blocks (no faces) remain supported.
static inline int threedi_3di3_collision_faces_runtime_safe(const ThreediCollisionModel *col) {
    if (!col) return 0;
    if (col->vertex_count != 0 && !col->vertices) return 0;
    if (col->normal_count != 0 && !col->normals) return 0;
    if (col->face_count != 0 && !col->faces) return 0;
    if (col->object_count != 0 && !col->objects) return 0;

    if (col->object_count == 0 &&
        (col->vertex_count != 0 || col->normal_count != 0 || col->face_count != 0)) return 0;

    size_t vertex_cursor = 0, normal_cursor = 0, face_cursor = 0;
    for (size_t oi = 0; oi < col->object_count; ++oi) {
        const ThreediCollisionObject *object = &col->objects[oi];
        if (object->num_vertices < 0 || object->num_faces < 0 || object->num_normals < 0) return 0;
        const size_t nv = (size_t)object->num_vertices;
        const size_t nn = (size_t)object->num_normals;
        const size_t nf = (size_t)object->num_faces;
        if (nv > col->vertex_count - vertex_cursor ||
            nn > col->normal_count - normal_cursor ||
            nf > col->face_count - face_cursor) return 0;
        for (size_t fi = 0; fi < nf; ++fi) {
            const ThreediCollisionFace *face = &col->faces[face_cursor + fi];
            /* A negative CNRM index is authorable; the runtime CFAC walker
               skips such faces rather than rejecting the model
               [orig: the CNRM resolve gate in
               Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]. */
            if (face->normal_index >= 0 && (size_t)face->normal_index >= nn) return 0;
            for (int k = 0; k < 3; ++k)
                if (face->vert_index[k] < 0 || (size_t)face->vert_index[k] >= nv) return 0;
        }
        vertex_cursor += nv;
        normal_cursor += nn;
        face_cursor += nf;
    }
    /* The vertex/normal/face runs must exactly partition their pools. */
    if (col->object_count != 0 &&
        (vertex_cursor != col->vertex_count || normal_cursor != col->normal_count ||
         face_cursor != col->face_count))
        return 0;
    return 1;
}

// Return 1 when the volumes are safe for runtime queries: backing arrays
// exist, COBJ-owned volume runs are contiguous and in range, and every BVOL
// owns a non-empty BPLN window (windows are consecutive across the whole
// BVOL pool).
static inline int threedi_3di3_collision_volumes_runtime_safe(const ThreediCollisionModel *col) {
    if (!col) return 0;
    if (col->volume_count != 0 && !col->volumes) return 0;
    if (col->plane_count != 0 && !col->planes) return 0;
    if (col->object_count != 0 && !col->objects) return 0;

    size_t volume_cursor = 0;
    for (size_t oi = 0; oi < col->object_count; ++oi) {
        const ThreediCollisionObject *object = &col->objects[oi];
        if (object->num_bounding_volumes < 0) return 0;
        const size_t nb = (size_t)object->num_bounding_volumes;
        if (nb > col->volume_count - volume_cursor) return 0;
        volume_cursor += nb;
    }
    /* Retail models legitimately author TRAILING BVOLs owned by no COBJ:
       Zodiacs, mounted-weapon items, and large buildings in the JO corpus all
       carry them. Retail never reaches them - every walker consumes volumes
       only through per-COBJ runs - so an unowned tail is dead data, not an
       unsafe model. */
    if (col->object_count != 0 && volume_cursor > col->volume_count) return 0;

    size_t plane_cursor = 0;
    for (size_t i = 0; i < col->volume_count; ++i) {
        const ThreediBoundingVolume *volume = &col->volumes[i];
        if (volume->plane_count <= 0) return 0;
        if ((size_t)volume->plane_count > col->plane_count - plane_cursor) return 0;
        plane_cursor += (size_t)volume->plane_count;
    }
    return 1;
}

// Return 1 when every collision slice is safe for runtime queries: both
// sections above.
static inline int threedi_3di3_collision_is_runtime_safe(const ThreediCollisionModel *col) {
    return threedi_3di3_collision_faces_runtime_safe(col) &&
           threedi_3di3_collision_volumes_runtime_safe(col);
}

/* The two vehicle platform-solve probe boxes retail derives at collision-model
   build time and stores in the runtime collision block (the "modelData
   [0x28..0x4C]" pair every vehicle contact solve reads). All values 16.16
   model space.
   [orig: Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0, tail
   @ 0x5b4455..0x5b45db]:
   - box Z pair    = the CMDL header bbox Z pair, verbatim.
   - box X/Y pairs = the fold of type-1 (solid) BVOL X/Y extents over volumes
     whose min-Z lies below CMDL minZ + zspan/2 (the model's lower HALF).
   - footprint X/Y = the same fold over volumes whose min-Z lies below
     CMDL minZ + zspan/8 (the bottom EIGHTH - the wheel/skid volumes), then
     each side clamped to at least q + 0x2000 from the origin, with
     q = (box Y span) >> 2 (the solve's pad radius).
   The Z bottom is the header bbox floor, NOT the deepest collision vertex:
   wheeled hulls author their origin at wheel contact with the CMDL floor at
   ~0, so a solve resting pads at boxZlo + q puts the ORIGIN on the terrain.
   Folding raw per-COBJ AABBs instead floats every hull by the below-origin
   wheel depth. */
typedef struct ThreediCollisionProbeBoxes {
    int32_t box_x_lo, box_x_hi;
    int32_t box_y_lo, box_y_hi;
    int32_t box_z_lo, box_z_hi;
    int32_t foot_x_lo, foot_x_hi;
    int32_t foot_y_lo, foot_y_hi;
} ThreediCollisionProbeBoxes;

/* Returns 1 when the witnessed derivation produced a live box set: a CMDL
   with nonzero Z span and at least one type-1 volume folded into the half
   box. Returns 0 otherwise (callers keep their degenerate-model fallback;
   retail carries the raw +-0x40000000 fold sentinels through in that case,
   which its solves never meet on shipped vehicles). Volumes are walked
   through the per-COBJ runs exactly like every retail consumer, so unowned
   trailing BVOLs stay dead. The CMDL floats were parsed from 16.16 disk
   words (parse_cmdl); the round-trip back is exact for any |value| < 128
   units and 1-LSB at worst beyond that. */
static inline int threedi_3di3_collision_probe_boxes(const ThreediCollisionModel *col,
                                                     ThreediCollisionProbeBoxes *out) {
    if (!col || !out || !col->objects || !col->volumes) return 0;
    const int32_t z_lo = (int32_t)lround((double)col->model_data.bbox[2] * 65536.0);
    const int32_t z_hi = (int32_t)lround((double)col->model_data.bbox[5] * 65536.0);
    if (z_hi == z_lo) return 0;
    /* [orig: @ 0x5b446e/@ 0x5b4477] */
    const int32_t thr_eighth = z_lo + ((z_hi - z_lo) >> 3);
    const int32_t thr_half = z_lo + ((z_hi - z_lo) >> 1);
    const int32_t kSentinel = 0x40000000;
    int32_t half_x_lo = kSentinel, half_x_hi = -kSentinel;
    int32_t half_y_lo = kSentinel, half_y_hi = -kSentinel;
    int32_t eighth_x_lo = kSentinel, eighth_x_hi = -kSentinel;
    int32_t eighth_y_lo = kSentinel, eighth_y_hi = -kSentinel;
    int folded_half = 0;
    size_t volume_cursor = 0;
    for (size_t oi = 0; oi < col->object_count; ++oi) {
        const ThreediCollisionObject *object = &col->objects[oi];
        if (object->num_bounding_volumes < 0) return 0;
        const size_t nb = (size_t)object->num_bounding_volumes;
        if (nb > col->volume_count - volume_cursor) return 0;
        for (size_t bi = 0; bi < nb; ++bi) {
            const ThreediBoundingVolume *v = &col->volumes[volume_cursor + bi];
            if (v->collidable_type != 1) continue; /* [orig: @ 0x5b44c4] */
            if (v->min_z_fp16 < thr_half) {
                if (v->min_x_fp16 < half_x_lo) half_x_lo = v->min_x_fp16;
                if (v->max_x_fp16 > half_x_hi) half_x_hi = v->max_x_fp16;
                if (v->min_y_fp16 < half_y_lo) half_y_lo = v->min_y_fp16;
                if (v->max_y_fp16 > half_y_hi) half_y_hi = v->max_y_fp16;
                folded_half = 1;
            }
            if (v->min_z_fp16 < thr_eighth) {
                if (v->min_x_fp16 < eighth_x_lo) eighth_x_lo = v->min_x_fp16;
                if (v->max_x_fp16 > eighth_x_hi) eighth_x_hi = v->max_x_fp16;
                if (v->min_y_fp16 < eighth_y_lo) eighth_y_lo = v->min_y_fp16;
                if (v->max_y_fp16 > eighth_y_hi) eighth_y_hi = v->max_y_fp16;
            }
        }
        volume_cursor += nb;
    }
    if (!folded_half) return 0;
    /* The footprint minimum-extent clamps [orig: @ 0x5b4563..0x5b45b0]; the
       half box is stored unclamped. */
    const int32_t q = (half_y_hi - half_y_lo) >> 2;
    if (q + eighth_x_lo + 0x2000 > 0) eighth_x_lo = -0x2000 - q;
    if (q + eighth_y_lo + 0x2000 > 0) eighth_y_lo = -0x2000 - q;
    if (eighth_x_hi - q - 0x2000 < 0) eighth_x_hi = q + 0x2000;
    if (eighth_y_hi - q - 0x2000 < 0) eighth_y_hi = q + 0x2000;
    out->box_x_lo = half_x_lo;
    out->box_x_hi = half_x_hi;
    out->box_y_lo = half_y_lo;
    out->box_y_hi = half_y_hi;
    out->box_z_lo = z_lo;
    out->box_z_hi = z_hi;
    out->foot_x_lo = eighth_x_lo;
    out->foot_x_hi = eighth_x_hi;
    out->foot_y_lo = eighth_y_lo;
    out->foot_y_hi = eighth_y_hi;
    return 1;
}

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
    float slot_priority_scale; /* [orig: OOBJ disk +20 -> runtime record +0x2C -
                                  the portal-slot priority weight the slot
                                  collector multiplies @ 0x5c6ea3; its only
                                  consumer is the slot sort's compare
                                  (Terrain_SortPortalSlotsByPriority @ 0x5c4440);
                                  zero across the JO 3DI3 corpus] */
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

static_assert(sizeof(ThreediTransform) == 8, "ThreediTransform layout mismatch");
static_assert(sizeof(ThreediPartAnimation) == 0x44, "ThreediPartAnimation layout mismatch");

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
int threedi_3di3_read_memory(const uint8_t *data, size_t size,
                                            Threedi3di3 *out_model);

// Convenience: write a previously-read model back to disk (round-trip).
int threedi_3di3_write(const char *path, const Threedi3di3 *model);

// A chunk too large for its header: a 3DI3 chunk says its payload length in
// 24 bits (THREEDI_3DI3_LENGTH_MASK), and a parent's payload is its children,
// so ROOT holds the whole model and an RLOD one LOD. `chunk` is its path from
// ROOT, a repeated chunk by its index ("ROOT/RDTA/RLOD[1]/VERT"), and
// `bytes` its payload.
typedef struct ThreediChunkOverflow {
    char chunk[48];
    size_t bytes;
} ThreediChunkOverflow;

// The same parity writer into memory: `out` receives exactly the bytes
// threedi_3di3_write puts on disk. Returns 0 on success, -1 when the writer
// refuses the model; when it refuses it because a chunk outgrew its length
// field, `overflow` (when given) receives the first such chunk, children
// before their parent, in file order (otherwise its chunk is left empty).
int threedi_3di3_write_memory(const Threedi3di3 *model, std::vector<uint8_t> &out,
                              ThreediChunkOverflow *overflow = nullptr);

// Free allocations inside a Threedi3di3.
void threedi_3di3_free(Threedi3di3 *model);

// Compute a model's placement "ground anchor" — the point of the model that
// should sit at an object's placed position. Resolution order:
//   1. The first userpoint whose name matches "ground" case-insensitively.
//   2. Otherwise the model ORIGIN (0,0,0): shipped missions place userpoint-less
//      models with their origin exactly on the terrain (verified against JO
//      data), so any other fallback mis-grounds them.
// The anchor is written to out[3] in model space (threedi_user_point_position's
// axis order, NOT render-swizzled): callers apply their own axis convention
// (e.g. godot_vec3). Returns 1 unless model/out is NULL (out untouched then).
int threedi_3di3_ground_anchor(const Threedi3di3 *model, float out[3]);

// User point kinds as the retail corpus spells them: 71 ('G') for gameplay
// points (seats, ground, cameras), 83 ('S') for effect/particle points.
inline constexpr int32_t THREEDI_USER_POINT_GAMEPLAY = 71;
inline constexpr int32_t THREEDI_USER_POINT_EFFECT = 83;

// The seat scan binds a model's `sitex` user points, in order, to its 8
// passenger seats. A ninth takes the control seat (the slot ctrlx and drvrx
// fill), sets no seat bit and ends the scan: the model still loads, and no
// user point after it is bound.
// [orig: EntityDef_LoadModelsAndCallbacks @ 0x439F50 - the seat store
//  @ 0x43A4F0, the seat bit 1 << n @ 0x43A4CC..0x43A4D0 (a byte, so none at
//  n = 8), the scan end `cmp ebp, 8; jg` @ 0x43A5AF]
inline constexpr int THREEDI_SITEX_SEAT_LIMIT = 8;

// Whether a user point is a `sitex` seat: its name starts `sitex` in any
// case, a five-character strnicmp from byte zero (a padded or embedded token
// is none). [orig: Entity_GetBoneSlotType @ 0x434ED0, the strnicmp @ 0x434F16;
// EntityDef_LoadModelsAndCallbacks, the strnicmp @ 0x43A4BC]
inline bool threedi_user_point_is_sitex(std::string_view name) {
    return opennova::strutil::starts_with_icase(name, "sitex");
}

// The attach scan reads only a model's FIRST 16 userpoints — the result is a
// 16-bit mask. [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]
inline constexpr int THREEDI_USER_POINT_SCAN_LIMIT = 16;

// A userpoint name -> the 16-bit mask over the model's FIRST 16 userpoints:
// exact case-insensitive match, and duplicate names all set their bit. This is
// the item-effect attach scan every consumer shares (the ITEMS.DEF particlefx
// resolve; the death/fire/other families mask the HUSK's fixed names).
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40 — the first-16 stricmp walk;
//  consumed by Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0]
static inline uint16_t threedi_3di3_user_point_mask(const Threedi3di3 *model,
                                                    const char *name) {
    if (model == NULL || name == NULL || name[0] == '\0') return 0;
    uint16_t mask = 0;
    size_t count = model->user_point_count < THREEDI_USER_POINT_SCAN_LIMIT
                       ? model->user_point_count
                       : THREEDI_USER_POINT_SCAN_LIMIT;
    for (size_t i = 0; i < count; ++i) {
        const char *a = model->user_points[i].name;
        const char *b = name;
        while (*a != '\0' && *b != '\0') {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') mask |= (uint16_t)(1u << i);
    }
    return mask;
}

} // namespace opennova::threedi
#pragma pack(pop)
