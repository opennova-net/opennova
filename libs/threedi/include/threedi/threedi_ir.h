// Common Intermediate Representation (IR) for 3DI models.
// This provides a unified representation that both Modern (3DI3) and GP (GPM/GPS/GPP)
// formats can be converted to, enabling a single scene builder and format conversion.

#ifndef THREEDI_IR_H
#define THREEDI_IR_H

#include <stddef.h>
#include <stdint.h>

// Visibility macro for shared library exports
#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_EXPORT __declspec(dllexport)
#  else
#    define THREEDI_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_EXPORT __attribute__((visibility("default")))
#  else
#    define THREEDI_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct Threedi3di3;
struct ThreediGpFile;
struct ThreediLwFile;

// ============================================================================
// Source Format
// ============================================================================

typedef enum ThreediIRSourceFormat {
    THREEDI_IR_SOURCE_UNKNOWN = 0,
    THREEDI_IR_SOURCE_3DI3 = 1,    // Modern 3DI3 format
    THREEDI_IR_SOURCE_GPM = 2,     // GP mesh format (basic)
    THREEDI_IR_SOURCE_GPS = 3,     // GP mesh format (static)
    THREEDI_IR_SOURCE_GPP = 4,     // GP mesh format (skinned)
    THREEDI_IR_SOURCE_LW = 5       // Land Warrior .3di v10
} ThreediIRSourceFormat;

typedef enum ThreediIRMeshType {
    THREEDI_IR_MESH_INVALID = 0,
    THREEDI_IR_MESH_BASIC = 1,     // Basic mesh (no tangents, no skinning)
    THREEDI_IR_MESH_STATIC = 2,    // Static mesh with tangents
    THREEDI_IR_MESH_SKINNED = 3    // Skinned mesh with bone weights
} ThreediIRMeshType;

// ============================================================================
// Vertex Definition
// ============================================================================

typedef struct ThreediIRVertex {
    float position[3];
    float normal[3];
    float uv0[2];
    float uv1[2];
    float tangent[3];
    float bitangent[3];
    float bone_weights[4];     // Normalized weights (sum to 1.0)
    uint8_t bone_indices[4];   // Indices into bone_table
    uint32_t flags;            // Original vertex flags
} ThreediIRVertex;

// ============================================================================
// Primitive (Triangle Strip/List)
// ============================================================================

typedef enum ThreediIRTopology {
    THREEDI_IR_TOPOLOGY_TRIANGLES = 0,   // Triangle list
    THREEDI_IR_TOPOLOGY_STRIP = 1        // Triangle strip
} ThreediIRTopology;

typedef struct ThreediIRPrimitive {
    int32_t material_index;         // Index into materials array
    int32_t part_index;             // Index of parent part (render object)
    uint32_t index_offset;          // Offset into LOD's index buffer
    uint32_t index_count;           // Number of indices
    uint32_t vertex_offset;         // Start vertex for indexed rendering
    uint32_t vertex_count;          // Number of vertices used
    ThreediIRTopology topology;     // Triangles or strip (strips pre-converted to triangles in IR)
    float min[3];                   // Bounding box min
    float max[3];                   // Bounding box max
    uint8_t bone_table[16];         // Maps local bone index -> skeleton bone index
    uint8_t bone_table_length;      // Number of valid entries (0 if no skinning)
} ThreediIRPrimitive;

// ============================================================================
// Part (Render Object / Subobject)
// ============================================================================

typedef struct ThreediIRPart {
    int32_t parent_index;           // -1 if root
    float rel_position[3];          // Relative to parent
    float abs_position[3];          // Absolute/world position
    float bounding_center[3];       // Bounding sphere center
    float bounding_radius;          // Bounding sphere radius
    int32_t primitive_start;        // First primitive index
    int32_t primitive_count;        // Number of primitives (opaque + alpha)
    int32_t opaque_count;           // Number of opaque primitives
    int32_t alpha_count;            // Number of alpha primitives
} ThreediIRPart;

// ============================================================================
// Material
// ============================================================================

// Blend modes
typedef enum ThreediIRBlendMode {
    THREEDI_IR_BLEND_OPAQUE = 0,
    THREEDI_IR_BLEND_ALPHA  = 1,   // _AB: standard alpha blending
    THREEDI_IR_BLEND_ADD    = 2,   // _AD: additive blending
} ThreediIRBlendMode;

// Material flags
#define THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST    0x01u
#define THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT  0x02u
#define THREEDI_IR_MATERIAL_FLAG_TWO_SIDED     0x04u
#define THREEDI_IR_MATERIAL_FLAG_EMISSIVE      0x08u

// Texture slot types
#define THREEDI_IR_TEX_SLOT_DIFFUSE  1
#define THREEDI_IR_TEX_SLOT_DETAIL   2
#define THREEDI_IR_TEX_SLOT_NORMAL   3
#define THREEDI_IR_TEX_SLOT_NORMAL_B 4

typedef struct ThreediIRMaterialTexture {
    char name[17];           // Texture filename (null-terminated)
    uint8_t slot;            // THREEDI_IR_TEX_SLOT_*
    uint8_t type;            // Original texture type
    uint8_t flags;           // Texture flags (animated, clamped)
    uint8_t frame;           // Animation frame index
} ThreediIRMaterialTexture;

// Shader animation parameter structs

typedef struct ThreediIRUvParams {
    uint8_t style;
    float phase;
    int32_t reg;        // Control register index, -1 if unused
    float gen_rate;
    float start;
    float end;
} ThreediIRUvParams;

typedef struct ThreediIRAlphaGen {
    uint8_t style;
    float phase;
    int32_t reg;
    float rate;
    int16_t start;
    int16_t end;
} ThreediIRAlphaGen;

typedef struct ThreediIRRgbGen {
    uint8_t style;
    float phase;
    int32_t reg;
    float rate;
    float start_color[4];  // RGBA 0..1
    float end_color[4];    // RGBA 0..1
} ThreediIRRgbGen;

typedef struct ThreediIRTexAnim {
    uint8_t num_frames;
    uint8_t animation_type;   // 0=time-based, 1=ctrl reg driven
    int16_t cycle_frame_time;
} ThreediIRTexAnim;

typedef struct ThreediIRMaterial {
    int32_t index;                         // Material index
    char shader_name[33];                  // Shader tag
    uint32_t texture_count;                // Number of valid texture slots
    ThreediIRMaterialTexture textures[8];  // Texture slots
    uint32_t flags;                        // THREEDI_IR_MATERIAL_FLAG_*
    float alpha_threshold;                 // Alpha test threshold (0..1)
    ThreediIRBlendMode blend_mode;         // Blend mode (opaque/alpha/additive)

    // Shader animation parameters
    ThreediIRUvParams u_params;
    ThreediIRUvParams v_params;
    ThreediIRAlphaGen alpha_gen;
    ThreediIRRgbGen rgb_gen;
    ThreediIRTexAnim animation;

    // Reflection / glass
    float reflect_color[4];   // RGBA 0..1
    int is_glass;

    // Material properties
    uint32_t specular_intensity;
    uint32_t luminosity;
    uint32_t emissive_color;   // Raw transparency/emissive value
    uint8_t emissive_type;     // 0=none, 2=full

    // Tiling
    float u_tiling;
    float v_tiling;

    // Collision surface type (raw binary value from collision faces)
    // Default 0x01 (Dirt). Maps to ptype via surface_type_to_ptype().
    uint8_t surface_type;

    // Collision polygon attributes (reconstructed from collision face material_flags).
    // Bit mapping from collision face flags:
    //   coll 0x100 → pattrib 0x100, coll 0x400 → pattrib 0x1000, coll 0x800 → pattrib 0x2000
    uint32_t pattrib;
} ThreediIRMaterial;

// ============================================================================
// Light
// ============================================================================

typedef struct ThreediIRLight {
    float offset[3];            // Position offset from part
    float attenuation_start;    // Light falloff start distance
    float attenuation_end;      // Light falloff end distance
    float color_start[3];       // RGB color at start (0..1)
    float color_end[3];         // RGB color at end (0..1)
    uint8_t style;              // Light animation style
    uint8_t phase;              // Animation phase
    uint16_t rate;              // Animation rate
    int32_t part_index;         // Attached part index
    uint8_t flags;              // Light flags (bits 0-2: disable, bit 3: type)
    float falloff;              // Light falloff angle (degrees, from 3di byte)
    float rotation[3];          // Light direction (-rotY, rotZ, rotX from 3di)
    uint8_t light_type;         // 0=Omni, 1=Target (from flags bit 3)
} ThreediIRLight;

// ============================================================================
// UserPoint (Marker)
// ============================================================================

typedef struct ThreediIRUserPoint {
    char name[17];              // UserPoint name
    float position[3];          // Position (in model units, pre-scaled)
    float direction[3];         // Direction/rotation
    int32_t part_index;         // Attached part index (-1 if none)
    int32_t type_code;          // UserPoint type
} ThreediIRUserPoint;

// ============================================================================
// Collision Data
// ============================================================================

typedef struct ThreediIRCollisionVertex {
    float position[3];
} ThreediIRCollisionVertex;

typedef struct ThreediIRCollisionPlane {
    float normal[3];
    float distance;             // Plane distance from origin
    uint16_t flags;
} ThreediIRCollisionPlane;

typedef struct ThreediIRCollisionVolume {
    int32_t type;               // Collidable type (CB, CC, etc.)
    int32_t flags;              // Volume flags
    float min[3];               // AABB min (world units)
    float max[3];               // AABB max (world units)
    int32_t plane_start;        // First plane index in volume's planes
    int32_t plane_count;        // Number of bounding planes
    int32_t part_index;         // Associated part index
    int32_t object_index;       // Source collision object index (if available)
} ThreediIRCollisionVolume;

typedef struct ThreediIRCollisionFace {
    int16_t vert_index[3];      // Local vertex indices (within subobject)
    uint32_t material_flags;    // CFAC material_flags (for material reverse-mapping)
    uint8_t poly_type;          // CFAC poly_type (for material reverse-mapping)
} ThreediIRCollisionFace;

typedef struct ThreediIRCollisionObject {
    int32_t num_vertices;       // Vertex count for this subobject
    int32_t num_faces;          // Face count for this subobject
    int32_t parent_subobject_index;  // Part hierarchy parent
    float offset[3];            // Center point position (raw fp16.16 stored as float)
} ThreediIRCollisionObject;

typedef struct ThreediIRCollisionTranslation {
    float translation[3];       // Attachment point position (raw fp16.16 stored as float)
} ThreediIRCollisionTranslation;

typedef struct ThreediIRCollision {
    // Global collision data
    float model_min[3];
    float model_max[3];
    float model_center[3];

    // Vertices
    ThreediIRCollisionVertex *vertices;
    size_t vertex_count;

    // Bounding planes
    ThreediIRCollisionPlane *planes;
    size_t plane_count;

    // Bounding volumes
    ThreediIRCollisionVolume *volumes;
    size_t volume_count;

    // Mesh data (for BulletLOD reconstruction)
    ThreediIRCollisionFace *faces;
    size_t face_count;

    ThreediIRCollisionObject *objects;
    size_t object_count;

    ThreediIRCollisionTranslation *translations;
    size_t translation_count;
} ThreediIRCollision;

// ============================================================================
// Occlusion Data
// ============================================================================

typedef struct ThreediIROcclusionVertex {
    float position[3];
} ThreediIROcclusionVertex;

typedef struct ThreediIROcclusionFace {
    uint32_t raw_indices;
    uint32_t edge_data;
    uint32_t other_edge_data;
} ThreediIROcclusionFace;

typedef struct ThreediIROcclusionPlane {
    float normal[3];
    float radius;
} ThreediIROcclusionPlane;

typedef struct ThreediIROcclusionObject {
    int32_t type;
    int32_t parent_subobject_index;
    int32_t connecting_subobject;
    float position[3];
    float radius;
    int32_t unk1;
    int32_t num_vertices;
    int32_t num_planes;
    int32_t face_count;
    int32_t vertex_start;
    int32_t plane_start;
    int32_t face_start;
} ThreediIROcclusionObject;

typedef struct ThreediIROcclusion {
    ThreediIROcclusionVertex *vertices;
    size_t vertex_count;

    ThreediIROcclusionFace *faces;
    size_t face_count;

    ThreediIROcclusionPlane *planes;
    size_t plane_count;

    ThreediIROcclusionObject *objects;
    size_t object_count;
} ThreediIROcclusion;

// ============================================================================
// Part Animation (from PANM)
// ============================================================================

typedef struct ThreediIRTransform {
    uint8_t control;            // Control function code
    uint8_t control_param;      // Control parameter
    int16_t rate;               // Animation rate
    int16_t start;              // Start value
    int16_t end;                // End value
} ThreediIRTransform;

typedef struct ThreediIRPartAnimation {
    uint32_t flags;             // Animation flags
    uint8_t parent_part;        // Parent part index
    uint8_t part_index;         // This part's index (transform_as)
    uint8_t matrix_index;       // Matrix table index
    uint8_t matrix_offset;      // Matrix table offset
    int32_t bind_matrix_index;  // Bind pose matrix index
    ThreediIRTransform rotation_x;
    ThreediIRTransform rotation_y;
    ThreediIRTransform rotation_z;
    ThreediIRTransform scale_x;
    ThreediIRTransform scale_y;
    ThreediIRTransform scale_z;
    ThreediIRTransform translation;
} ThreediIRPartAnimation;

// ============================================================================
// LOD (Level of Detail)
// ============================================================================

typedef struct ThreediIRLod {
    int32_t threshold;              // LOD distance threshold
    int32_t declared_part_count;    // RMDL declared count (may differ from part_count)

    // Vertices (unified format)
    ThreediIRVertex *vertices;
    size_t vertex_count;

    // Indices
    uint16_t *indices;
    size_t index_count;

    // Primitives (triangle strips/lists)
    ThreediIRPrimitive *primitives;
    size_t primitive_count;

    // Parts (render objects)
    ThreediIRPart *parts;
    size_t part_count;

    // Part animations (PANM) — per-LOD, as in the 3DI format
    ThreediIRPartAnimation *part_animations;
    size_t part_animation_count;
} ThreediIRLod;

typedef struct ThreediIRControlRegister {
    char name[25];
} ThreediIRControlRegister;

typedef struct ThreediIRMatrix {
    float m[16];                // 4x4 matrix, row-major
} ThreediIRMatrix;

// ============================================================================
// Complete Model IR
// ============================================================================

typedef struct ThreediModelIR {
    // Identification
    char name[32];
    char render_function[5];    // RMDL model_type tag (e.g. "org0", "gnrc")
    ThreediIRSourceFormat source_format;
    ThreediIRMeshType mesh_type;

    // LODs
    ThreediIRLod *lods;
    size_t lod_count;

    // Materials (shared across all LODs)
    ThreediIRMaterial *materials;
    size_t material_count;

    // Lights
    ThreediIRLight *lights;
    size_t light_count;

    // UserPoints
    ThreediIRUserPoint *userpoints;
    size_t userpoint_count;

    // Collision data
    ThreediIRCollision *collision;
    int32_t collision_lod;          // LOD index used as collision source (-1 = last)

    // Occlusion data
    ThreediIROcclusion *occlusion;

    // Control registers (CTRL)
    ThreediIRControlRegister *control_registers;
    size_t control_register_count;

    // Matrix table (MTRX)
    ThreediIRMatrix *matrices;
    size_t matrix_count;

} ThreediModelIR;

// ============================================================================
// API Functions
// ============================================================================

// Initialize an IR structure to empty/zero state
THREEDI_EXPORT void threedi_ir_init(ThreediModelIR *ir);

// Free all allocations inside an IR structure
THREEDI_EXPORT void threedi_ir_free(ThreediModelIR *ir);

// Convert Modern (3DI3) model to IR
// Returns 0 on success, -1 on error
THREEDI_EXPORT int threedi_ir_from_3di3(const struct Threedi3di3 *model, ThreediModelIR *out);

// Convert GP (GPM/GPS/GPP) file to IR
// Returns 0 on success, -1 on error
THREEDI_EXPORT int threedi_ir_from_gp(const struct ThreediGpFile *gp, ThreediModelIR *out);

// Convert Land Warrior .3di v10 file to IR
// Returns 0 on success, -1 on error
THREEDI_EXPORT int threedi_ir_from_lw(const struct ThreediLwFile *lw, ThreediModelIR *out);

// Read any 3DI file and produce IR (auto-detects format)
// Returns 0 on success, -1 on error
THREEDI_EXPORT int threedi_ir_read(const char *path, ThreediModelIR *out);

// Convert IR back to Modern model (for format conversion)
// Returns 0 on success, -1 on error
THREEDI_EXPORT int threedi_ir_to_3di3(const ThreediModelIR *ir, struct Threedi3di3 *out);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_IR_H
