// GP format (GPM/GPS/GPP) definitions and parser.
// This provides pure C parsing of legacy GP 3DI files.

#ifndef THREEDI_GP_H
#define THREEDI_GP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// GP Mesh Types
// ============================================================================

typedef enum ThreediGpMeshType {
    THREEDI_GP_MESH_UNKNOWN = 0,
    THREEDI_GP_MESH_BASIC = 1,    // GPM - basic mesh
    THREEDI_GP_MESH_STATIC = 2,   // GPS - static mesh with tangents
    THREEDI_GP_MESH_SKINNED = 3   // GPP - skinned mesh
} ThreediGpMeshType;

// ============================================================================
// GP Header
// ============================================================================

// Raw header size constant
#define THREEDI_GP_HEADER_SIZE 0xEC

typedef struct ThreediGpHeader {
    ThreediGpMeshType mesh_type;
    uint8_t format_ver;             // 0x03: always 0x02
    uint32_t revision;              // 0x04: file revision (0x0102=C4, 0x0103=BHD)
    uint32_t flags;
    int32_t num_lods;
    uint32_t lod_thresholds[3];     // 0x20/0x24/0x28: Q16.16 far/mid/near
    char model_tag[5];              // 0x40: 4-char model type tag, null-terminated
    int32_t rverts_count;
    int32_t userpoint_count;
    int32_t ctrl_reg_count;
    int32_t matrix_count;
    int32_t occlusion_count;
    char name[17];              // Null-terminated
    uint8_t raw[THREEDI_GP_HEADER_SIZE];  // Raw header bytes for roundtrip
} ThreediGpHeader;

// ============================================================================
// GP UserPoint
// ============================================================================

typedef struct ThreediGpUserPoint {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t rot_x;
    int32_t rot_y;
    int32_t rot_z;
    int32_t parent_subobject;
    int32_t type_code;
    char name[17];              // Null-terminated
} ThreediGpUserPoint;

// ============================================================================
// GP Material (152 bytes on disk)
// ============================================================================

// Material transform animation parameters (8 bytes)
typedef struct ThreediGpMaterialTransform {
    uint8_t style;              // Animation style
    uint8_t param;              // Parameter (phase if style <= 112, ctrl_reg index otherwise)
    int16_t rate;               // Animation rate (Q8.8 fixed-point)
    int16_t start;              // Start value (Q8.8 fixed-point)
    int16_t end;                // End value (Q8.8 fixed-point)
} ThreediGpMaterialTransform;

typedef struct ThreediGpMaterial {
    char texture_name[17];      // 0x00: Null-terminated (16 bytes on disk)
    uint32_t render_attributes; // 0x10: legacy render attributes
    uint32_t physical_attributes; // 0x14: legacy physical attributes (usually zero)
    uint32_t use_alpha_pcx;     // 0x18: legacy alpha PCX flag (usually zero)
    uint32_t color_rgb[3];      // 0x1C: legacy color_rgb[0]=color_green (rgbgen start R,G,B)
                                //        color_rgb[1]=color_alpha (rgbgen end R,G,B)
                                //        color_rgb[2]=color_type (material color mode enum)
    uint32_t render_lookup;     // 0x28: legacy poly152_index — material lookup table index
    uint32_t luminosity;        // 0x2C: legacy luminosity
    uint32_t specular_intensity; // 0x30: legacy specular intensity
    uint32_t specular_sharpness; // 0x34: legacy specular sharpness (usually zero)
    uint32_t shader_flags;      // 0x38: legacy shader_type — shader mode enum
    uint32_t tex_addressing_mode; // 0x3C: Texture addressing mode
    uint32_t reserved_40;       // 0x40: legacy u_offset (always zero on disk)
    uint32_t reserved_44;       // 0x44: legacy v_offset (always zero on disk)
    float u_tiling;             // 0x48: legacy u_tiling
    float v_tiling;             // 0x4C: legacy v_tiling
    ThreediGpMaterialTransform mapfunc_u;  // 0x50: legacy mapfunc_u (style/param/rate/start/end)
    ThreediGpMaterialTransform mapfunc_v;  // 0x58: legacy mapfunc_v
    ThreediGpMaterialTransform rgbgen;     // 0x60: legacy rgbgen
    uint32_t emissive_color;    // 0x68: legacy transparency — emissive/transparency value
    ThreediGpMaterialTransform alphagen;   // 0x6C: legacy alphagen
    // 0x74: 9 x u32 = 36 bytes, from IDA (0x421120). All zero in on-disk corpus.
    uint32_t runtime_ptr;       // 0x74: Runtime pointer (always 0 on disk)
    float reflect_r;            // 0x78: Reflection color red
    float reflect_g;            // 0x7C: Reflection color green
    float reflect_b;            // 0x80: Reflection color blue
    uint32_t reflect_type;      // 0x84: Reflection type (0=none)
    float reflect_alpha;        // 0x88: Reflection alpha
    uint32_t actionplane_type;  // 0x8C: Action plane type
    uint32_t projector_type;    // 0x90: Projector type
    uint32_t projector_no_receive; // 0x94: Projector no-receive flag
} ThreediGpMaterial;

// ============================================================================
// GP Subobject (Part)
// ============================================================================

typedef struct ThreediGpSubObject {
    uint32_t unk_00;            // 0x00: runtime batch ptr (zero on disk)
    int32_t batch_count;        // 0x04
    uint32_t unk_08;            // 0x08
    int32_t parent;             // 0x0C
    float rel[3];               // 0x10: Relative position
    float abs[3];               // 0x1C: Absolute position
    float bounding_min[3];      // 0x28: Bounding box min
    uint32_t bounding_radius;   // 0x34: Bounding sphere radius (raw u32, likely Q16.16)
    float bounding_max[3];      // 0x38: Bounding box max
    uint8_t visible;            // 0x44
    uint8_t special_flag;       // 0x45
    uint8_t pad[2];             // 0x46-0x47
} ThreediGpSubObject;

// ============================================================================
// GP Variable Poly (Primitive)
// ============================================================================

typedef struct ThreediGpVariablePoly {
    int32_t subobject_index;
    int32_t material_index;
    int32_t topology;           // 0 = list, 1 = strip
    int32_t first_vertex;
    int32_t max_vertex_index;
    uint16_t *indices;
    size_t index_count;
    uint8_t bone_table[16];
    int32_t bone_table_length;
} ThreediGpVariablePoly;

// ============================================================================
// GP Extra Poly (88 bytes = VariablePoly1[40] + VariablePoly2[48])
// ============================================================================
//
// IDA: VariablePoly1 (40 bytes) — poly header with 4 reserved u32s
// IDA: VariablePoly2 (48 bytes) — poly header with bounding box (6 floats)
//
// Each 88-byte record contains two draw call descriptors. The first half
// (VariablePoly1) has max_vertex_index + 4 reserved u32s; the second half
// (VariablePoly2) has vertex_count + 6 bounding floats. Both halves share
// the same poly header layout for the first 24 bytes.

typedef struct ThreediGpExtraPolyHalf {
    int32_t material_index;
    uint32_t indices_tag;       // Expected: 0x72646441 ("Addr")
    uint16_t index_count;
    uint16_t triangle_count;
    int32_t topology;           // 0 = list, 1 = strip
    int32_t first_vertex;
    int32_t vertex_count;       // VP1: max_vertex_index, VP2: vertex_count
} ThreediGpExtraPolyHalf;

typedef struct ThreediGpExtraPoly {
    // First half (VariablePoly1 — 40 bytes)
    ThreediGpExtraPolyHalf vp1;
    uint32_t vp1_reserved[4];   // 16 bytes at offset 0x18-0x24

    // Second half (VariablePoly2 — 48 bytes)
    ThreediGpExtraPolyHalf vp2;
    float bbox_min[3];          // Bounding box min (x, y, z)
    float bbox_max[3];          // Bounding box max (x, y, z)
} ThreediGpExtraPoly;

// ============================================================================
// GP Render Vertex
// ============================================================================

typedef struct ThreediGpRVert {
    float position[3];
    float normal[3];
    float uv0[2];
    float uv1[2];
    float tangent[3];
    float bone_weights[3];
    uint8_t bone_indices[4];
    uint32_t packed_color;
    int has_normal;
    int is_skinned;
} ThreediGpRVert;

// ============================================================================
// GP Part Animation (per-subobject, 92 bytes in file)
// ============================================================================

// Animation transform channel (8 bytes) - same layout as material transform
typedef struct ThreediGpAnimTransform {
    uint8_t control;            // Animation control type
    uint8_t param;              // Parameter (phase or ctrl_reg index)
    int16_t rate;               // Animation rate (Q8.8 fixed-point)
    int16_t start;              // Start value (Q8.8 fixed-point)
    int16_t end;                // End value (Q8.8 fixed-point)
} ThreediGpAnimTransform;

typedef struct ThreediGpPartAnimation {
    uint32_t flags;
    uint8_t parent_subobject;
    uint8_t subobject_index;
    uint8_t matrix_index;
    uint8_t matrix_offset;
    int32_t bind_matrix_index;
    // GP order: scale first, then rotation (different from Modern)
    ThreediGpAnimTransform scale_x;
    ThreediGpAnimTransform scale_y;
    ThreediGpAnimTransform scale_z;
    ThreediGpAnimTransform rot_x;
    ThreediGpAnimTransform rot_y;
    ThreediGpAnimTransform rot_z;
    ThreediGpAnimTransform translate;
    // Legacy animation controls: rotate_type, scale_type, transform_as, yaw_rate, pitch_rate, roll_rate
    uint32_t rotate_type;       // Rotation interpolation type
    uint32_t scale_type;        // Scale interpolation type
    uint32_t transform_as;      // Transform mode
    float yaw_rate;             // Base yaw rotation rate
    float pitch_rate;           // Base pitch rotation rate
    float roll_rate;            // Base roll rotation rate
} ThreediGpPartAnimation;

// ============================================================================
// GP Control Register (44 bytes in file) — IDA: CtrlRegEntry
// ============================================================================

typedef struct ThreediGpControlRegister {
    char name[17];              // 0x00: 16 bytes on disk, null-terminated
    uint32_t name_index;        // 0x10: Sequential index
    uint32_t param[6];          // 0x14: param_1..param_6 (6 × u32 = 24 bytes)
} ThreediGpControlRegister;

// ============================================================================
// GP Matrix (64 bytes = 4x4 row-major floats)
// ============================================================================

typedef struct ThreediGpMatrix {
    float m[16];
} ThreediGpMatrix;

// ============================================================================
// GP Batch (32 bytes on disk)
// ============================================================================

typedef struct ThreediGpBatch {
    uint32_t opaque_ptr;        // Runtime pointer (zero on disk)
    uint32_t opaque_count;
    uint32_t transparent_ptr;   // Runtime pointer (zero on disk)
    uint32_t transparent_count;
    uint32_t reserved_10;
    uint32_t reserved_14;
    uint32_t reserved_18;
    uint32_t reserved_1C;
} ThreediGpBatch;

// ============================================================================
// GP Strip Triplet (12 bytes on disk)
// ============================================================================

typedef struct ThreediGpStripTriplet {
    uint32_t a;
    uint32_t b;
    uint32_t c;
} ThreediGpStripTriplet;

// ============================================================================
// GP Material Lookup (60 bytes per entry) — IDA: RenderInfo60
// ============================================================================

typedef struct ThreediGpMaterialLookup {
    char texture_name[17];      // 0x00: 16 bytes on disk, null-terminated
    uint32_t unk_10;            // 0x10: Unknown
    uint32_t unk_14;            // 0x14: Unknown
    uint32_t unk_18;            // 0x18: Unknown
    uint32_t unk_1C;            // 0x1C: Unknown
    uint32_t unk_20;            // 0x20: Unknown
    uint8_t seq_index;          // 0x24: Sequential index (0,1,2...)
    uint8_t pad_25;             // 0x25: Zero padding
    uint8_t flags_26;           // 0x26: Unknown flags
    uint8_t slot_type;          // 0x27: Texture slot type (0x02=diffuse, 0x04=lightmap)
    uint16_t tex_width;         // 0x28: Texture width (encoded)
    uint16_t tex_height;        // 0x2A: Texture height (encoded)
    uint32_t runtime_2C;        // 0x2C: Runtime-only (zeroed on load)
    uint32_t runtime_30;        // 0x30: Runtime-only (zeroed on load)
    uint32_t unk_34;            // 0x34: Unknown
    uint32_t unk_38;            // 0x38: Unknown
} ThreediGpMaterialLookup;

// ============================================================================
// GP VStream (tangent/binormal data for GPS)
// ============================================================================

typedef struct ThreediGpVStream {
    uint32_t buffer_ptr;        // Runtime pointer (zero on disk)
    uint32_t data_size;         // Size of data blob
    uint32_t unk_08;            // Unknown header field at offset 0x08
    uint32_t unk_0C;            // Unknown header field at offset 0x0C
    uint8_t *data;              // Raw vertex stream data (24 bytes per vertex)
    size_t data_len;            // Actual data length
} ThreediGpVStream;

// ============================================================================
// GP Render Model (LOD)
// ============================================================================

// RModel header size constant
#define THREEDI_GP_RMODEL_HEADER_SIZE 0x88

typedef struct ThreediGpRModel {
    ThreediGpSubObject *subobjects;
    size_t subobject_count;

    ThreediGpVariablePoly *polys;
    size_t poly_count;

    ThreediGpMaterial *materials;
    size_t material_count;

    ThreediGpPartAnimation *part_animations;
    size_t part_animation_count;

    ThreediGpStripTriplet *strip_triplets;
    size_t strip_triplet_count;

    uint32_t flags;

    // RModel header fields 0x20-0x4C (from IDA RModelHeader)
    // 0x20/0x38: Q16.16 values on skinned chars & flags (15/1092 files)
    // 0x28: small integer (always 3 when present, on weapons/chars)
    // Remaining 11 u32s at 0x24/0x2C/0x30/0x34/0x3C/0x40/0x44/0x6C-0x84: always zero in corpus
    uint32_t unk_20;            // 0x20: Q16.16 (e.g. 4.0 on skinned, 7.0 on flags)
    uint32_t unk_28;            // 0x28: Small int (always 3 when present)
    uint32_t unk_38;            // 0x38: Q16.16 (e.g. 4.0 on flag models)
    uint32_t unk_48;            // 0x48: Zero in corpus
    uint32_t unk_4C;            // 0x4C: Zero in corpus

    ThreediGpExtraPoly *extra_polys;
    uint32_t extra_poly_count;  // 0x60: Number of extra polys (88 bytes each)
    uint32_t extra_xform_count; // 0x68: Number of extra transforms

    uint8_t raw_header[THREEDI_GP_RMODEL_HEADER_SIZE];  // Raw header for roundtrip
    uint8_t *raw_blob;          // Raw data blob for roundtrip (if non-NULL, use instead of reconstruction)
    size_t raw_blob_len;        // Length of raw blob
} ThreediGpRModel;

// ============================================================================
// GP Ambient Light (from 60-byte light section inner header)
// ============================================================================

typedef struct ThreediGpAmbientLight {
    uint32_t colorgen_style;    // 0x00: Animation style
    float colorgen_rate;        // 0x04: Animation rate
    float colorgen_phase;       // 0x08: Animation phase
    float color_start[3];       // 0x0C: Start RGB (float)
    float color_end[3];         // 0x18: End RGB (float)
} ThreediGpAmbientLight;

// ============================================================================
// GP Light
// ============================================================================

typedef struct ThreediGpLight {
    uint8_t style;              // Colorgen animation style
    uint8_t phase;              // Animation phase (or ctrl_reg index if style > 0x70)
    uint16_t rate;              // Animation rate
    uint8_t color_start[3];     // BGR start color on disk (0-255)
    uint8_t color_end[3];       // BGR end color on disk (0-255)
    float position[3];          // Authored model-space position (x, y, z)
    float attenuation_start;    // Falloff start distance
    float attenuation_end;      // Falloff end distance
    int32_t part_index;         // Attached part index
    uint32_t unk_36;            // Unknown field at offset 0x24 (36)
    uint32_t unk_40;            // Unknown field at offset 0x28 (40)
    uint32_t unk_44;            // Unknown field at offset 0x2C (44)
} ThreediGpLight;

// ============================================================================
// GP Occlusion
// ============================================================================

// Occlusion object (60 bytes on disk)
// Layout verified from BHD revision (0x0103) files via hex dump.
// Note: This differs from earlier IDA documentation which had type/parent at end.
typedef struct ThreediGpOcclusionObject {
    uint8_t type;               // 0x00: Occlusion type
    uint8_t parent_subobject_index; // 0x01: Parent subobject
    uint8_t connecting_subobject;   // 0x02: Connecting subobject
    uint8_t pad;                // 0x03: Padding
    float center[3];            // 0x04-0x0F: Position/centroid
    float radius;               // 0x10-0x13: Bounding sphere radius
    int32_t num_vertices;       // 0x14: Vertex count
    uint32_t vertex_ptr;        // 0x18: Runtime pointer (zero on disk)
    int32_t num_planes;         // 0x1C: Plane count
    uint32_t plane_ptr;         // 0x20: Runtime pointer (zero on disk)
    int32_t num_faces;          // 0x24: Face count
    uint32_t face_ptr;          // 0x28: Runtime pointer (zero on disk)
    int32_t reserved[4];        // 0x2C-0x3B: Reserved (16 bytes to reach 60)
} ThreediGpOcclusionObject;     // 60 bytes total

typedef struct ThreediGpOcclusionVertex {
    float position[3];
} ThreediGpOcclusionVertex;

typedef struct ThreediGpOcclusionPlane {
    float normal[3];
    float radius;
} ThreediGpOcclusionPlane;

typedef struct ThreediGpOcclusionFace {
    uint32_t raw_indices;
    uint32_t edge_data;
    uint32_t other_edge_data;
} ThreediGpOcclusionFace;

typedef struct ThreediGpOcclusion {
    ThreediGpOcclusionObject *objects;
    size_t object_count;

    ThreediGpOcclusionVertex *vertices;
    size_t vertex_count;

    ThreediGpOcclusionPlane *planes;
    size_t plane_count;

    ThreediGpOcclusionFace *faces;
    size_t face_count;
} ThreediGpOcclusion;

// ============================================================================
// GP Collision
// ============================================================================

// Collision vertex: int16[3] position (8.8 fixed-point * scale) + int16 material
typedef struct ThreediGpCollisionVertex {
    int16_t x, y, z;
    int16_t material_index;
} ThreediGpCollisionVertex;

// Collision normal: int16[3] (1.14 fixed-point unit normal) + int16 dominant axis
typedef struct ThreediGpCollisionNormal {
    int16_t nx, ny, nz;
    int16_t dominant_axis;       // 1=Z, 2=Y, 4=X
} ThreediGpCollisionNormal;

// Collision face (44 bytes)
typedef struct ThreediGpCollisionFace {
    uint16_t vertex_indices[3];
    int16_t normal_index;
    int32_t plane_d;            // Plane distance (scaled fixed-point)
    int32_t bbox_min_x;
    int32_t bbox_max_x;
    int32_t bbox_min_y;
    int32_t bbox_max_y;
    int32_t bbox_min_z;
    int32_t bbox_max_z;
    int32_t surface_flags;
    uint8_t surface_type;
    uint8_t pad;
    int16_t reserved;
} ThreediGpCollisionFace;

// Collision object (128 bytes)
typedef struct ThreediGpCollisionObject {
    int32_t flags;
    int32_t vertex_count;
    uint32_t vertex_ptr;        // Runtime pointer (ignored on parse)
    int32_t face_count;
    uint32_t face_ptr;
    int32_t normal_count;
    uint32_t normal_ptr;
    int32_t volume_count;
    uint32_t volume_ptr;
    int32_t parent_subobject;   // Parent render subobject index
    int32_t reserved[3];
    int32_t translation[3];     // Scaled fixed-point position
    int32_t bbox_min_x;
    int32_t bbox_max_x;
    int32_t bbox_min_y;
    int32_t bbox_max_y;
    int32_t bbox_min_z;
    int32_t bbox_max_z;
    int32_t center[3];
    int32_t bounding_sphere_radius;
    int32_t bounding_cylinder_radius;
    int32_t bbox_height;
    int32_t reserved2[4];
} ThreediGpCollisionObject;

// Collision translation (12 bytes)
typedef struct ThreediGpCollisionTranslation {
    int32_t x, y, z;           // Scaled fixed-point
} ThreediGpCollisionTranslation;

// Collision plane (16 bytes)
typedef struct ThreediGpCollisionPlane {
    int32_t a, b, c, d;        // 16.16 fixed-point plane equation
} ThreediGpCollisionPlane;

// Collision volume (96 bytes)
typedef struct ThreediGpCollisionVolume {
    int32_t type;               // Volume type enum
    int32_t flags;              // BlinkBox visibility flags
    int32_t min_x, max_x;
    int32_t min_y, max_y;
    int32_t min_z, max_z;
    int32_t extent[3];
    int32_t reserved1;
    int32_t bbox_min_x;
    int32_t bbox_max_x;
    int32_t bbox_min_y;
    int32_t bbox_max_y;
    int32_t bbox_min_z;
    int32_t bbox_max_z;
    int32_t plane_count;
    uint32_t plane_ptr;         // Runtime pointer (ignored on parse)
    int32_t reserved2[4];
} ThreediGpCollisionVolume;

// Collision header size constant
#define THREEDI_GP_COLLISION_HEADER_SIZE 136

typedef struct ThreediGpCollision {
    uint32_t is_skinned;
    uint32_t data_size;
    float mid[3];
    float min[3];
    float max[3];

    ThreediGpCollisionVertex *vertices;
    int32_t vertex_count;

    ThreediGpCollisionNormal *normals;
    int32_t normal_count;

    ThreediGpCollisionFace *faces;
    int32_t face_count;

    ThreediGpCollisionObject *objects;
    int32_t object_count;

    ThreediGpCollisionTranslation *translations;
    int32_t translation_count;

    ThreediGpCollisionPlane *planes;
    int32_t plane_count;

    ThreediGpCollisionVolume *volumes;
    int32_t volume_count;

    uint8_t raw_header[THREEDI_GP_COLLISION_HEADER_SIZE];  // Raw header for roundtrip
} ThreediGpCollision;

// ============================================================================
// GP Parsed File
// ============================================================================

typedef struct ThreediGpFile {
    ThreediGpHeader header;

    ThreediGpUserPoint *userpoints;
    size_t userpoint_count;

    ThreediGpMaterialLookup *material_lookups;
    size_t material_lookup_count;

    ThreediGpRVert *rverts;
    size_t rvert_count;

    ThreediGpRModel *rmodels;
    size_t rmodel_count;

    ThreediGpCollision *collision;

    ThreediGpOcclusion *occlusion;

    // Light section header data
    uint32_t light_version;         // Outer header version (must be >= 256)
    uint32_t light_entries_ptr;     // Runtime pointer (zero on disk)
    uint32_t light_reserved_2C;     // Unused (always zero)
    uint32_t light_reserved_30;     // Unused (always zero)
    uint32_t light_reserved_34;     // Unused (always zero)
    uint32_t light_reserved_38;     // Unused (always zero)

    ThreediGpAmbientLight ambient_light;
    int has_ambient_light;          // 1 if ambient light data was parsed

    ThreediGpLight *lights;
    size_t light_count;

    ThreediGpControlRegister *control_registers;
    size_t control_register_count;

    ThreediGpMatrix *matrices;
    size_t matrix_count;

    ThreediGpVStream *vstream;
} ThreediGpFile;

// ============================================================================
// API Functions
// ============================================================================

// Initialize a GP file structure
void threedi_gp_init(ThreediGpFile *gp);

// Free all allocations inside a GP file structure
void threedi_gp_free(ThreediGpFile *gp);

// Parse a GP file from a memory buffer
// Returns 0 on success, -1 on error
int threedi_gp_parse(const uint8_t *data, size_t data_len, ThreediGpFile *out);

// Read and parse a GP file from disk
// Returns 0 on success, -1 on error
int threedi_gp_read(const char *path, ThreediGpFile *out);

// Detect if a file is a GP format (GPM/GPS/GPP)
// Returns the mesh type, or THREEDI_GP_MESH_UNKNOWN if not a GP file
ThreediGpMeshType threedi_gp_detect(const uint8_t *data, size_t data_len);

// Write a GP file to a memory buffer
// Returns 0 on success, -1 on error
// On success, *out_data and *out_len are set to the allocated buffer and its size
// Caller must free *out_data
int threedi_gp_write_buffer(const ThreediGpFile *gp, uint8_t **out_data, size_t *out_len);

// Write a GP file to disk
// Returns 0 on success, -1 on error
int threedi_gp_write(const char *path, const ThreediGpFile *gp);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_GP_H
