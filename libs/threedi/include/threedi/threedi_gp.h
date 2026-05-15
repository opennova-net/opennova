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
    uint32_t render_attributes; // 0x10: 3DA: render_attributes
    uint32_t physical_attributes; // 0x14: 3DA: physical_attributes (usually zero)
    uint32_t use_alpha_pcx;     // 0x18: 3DA: use_alpha_pcx (usually zero)
    uint32_t color_rgb[3];      // 0x1C: 3DA: color_rgb[0]=color_green (rgbgen start R,G,B)
                                //        color_rgb[1]=color_alpha (rgbgen end R,G,B)
                                //        color_rgb[2]=color_type (material color mode enum)
    uint32_t render_lookup;     // 0x28: 3DA: poly152_index — material lookup table index
    uint32_t luminosity;        // 0x2C: 3DA: luminosity
    uint32_t specular_intensity; // 0x30: 3DA: specular_intensity
    uint32_t specular_sharpness; // 0x34: 3DA: specular_sharpness (usually zero)
    uint32_t shader_flags;      // 0x38: 3DA: shader_type — shader mode enum
    uint32_t tex_addressing_mode; // 0x3C: Texture addressing mode
    // 0x40 / 0x44: Always zero in 22/22 corpus fixtures.  dfvas's GP
    // loader (load_rmodel_resource @ 0x510650) does not read or write
    // these bytes.  df4oed's in-memory material struct has UI-state
    // u_offset/v_offset at the same offsets, but the GP disk format
    // leaves them as zero padding.  See notes/gp-corpus-probe-material.md.
    uint32_t pad_disk_uvoffset_u; // 0x40
    uint32_t pad_disk_uvoffset_v; // 0x44
    float u_tiling;             // 0x48: 3DA: u_tiling
    float v_tiling;             // 0x4C: 3DA: v_tiling
    ThreediGpMaterialTransform mapfunc_u;  // 0x50: 3DA: mapfunc_u (style/param/rate/start/end)
    ThreediGpMaterialTransform mapfunc_v;  // 0x58: 3DA: mapfunc_v
    ThreediGpMaterialTransform rgbgen;     // 0x60: 3DA: rgbgen
    uint32_t emissive_color;    // 0x68: 3DA: transparency — emissive/transparency value
    ThreediGpMaterialTransform alphagen;   // 0x6C: 3DA: alphagen
    // 0x74: Runtime pointer slot.  Always zero on disk in 22/22 corpus
    // fixtures.  dfvas::load_rmodel_resource @ 0x510bd3 overwrites this
    // value at load time with either 0 (no matching palette entry) or a
    // pointer to a 60-byte palette/material-info table entry — the
    // on-disk value is never read.  See notes/gp-corpus-probe-material.md.
    uint32_t pad_runtime_ptr;   // 0x74
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
    // 0x00: Always zero on disk (22/22 fixtures).  dfvas::load_rmodel_resource
    // @ 0x510833 (static path) overwrites this slot at load time with a pointer
    // to the per-subobject batch buffer.  On-disk value is unused; treat as
    // runtime-only padding.
    uint32_t pad_runtime_submesh_ptr;
    int32_t batch_count;        // 0x04
    // 0x08: Always zero on disk (22/22 fixtures).  Not read or written by
    // dfvas::load_rmodel_resource's 18-DWORD per-subobject loop; may be
    // runtime-reserved padding adjacent to pad_runtime_submesh_ptr.
    // Corpus probe 2026-05-08: all 22 fixtures return 0x00000000.
    uint32_t pad_subobject_08;
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
    // 7-byte gap in the 24-byte bone-info sub-record (between the 16-byte
    // bone_table and the 1-byte bone_table_length).  Always zero in all
    // 788/788 global_batch polys across 639/639 corpus files (AS_ASSETS).
    // Structural alignment: 16 + 7 + 1 = 24-byte sub-record.
    // dfvas::load_rmodel_resource @ 0x510aa6 does not access these bytes.
    // See notes/gp-corpus-probe-bone-info-gap-phase-4.md.
    uint8_t pad_bone_info_align[7];
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
    // 0x18..0x27: 4 dwords always zero.  0 extra_polys found across 639 AS_ASSETS
    // fixtures — no corpus sample exists to show non-zero.  dfvas::load_rmodel_resource
    // @ 0x510650 does not access these offsets in the loader decompilation.
    // See notes/gp-corpus-probe-vp1-reserved-phase-3.md.
    uint32_t vp1_pad_18;
    uint32_t vp1_pad_1C;
    uint32_t vp1_pad_20;
    uint32_t vp1_pad_24;

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
    // GPS only: 4th component of the on-disk normal vector (normal[3]).
    // In all 2379 GPS verts across the single GPS fixture in the 639-file
    // AS_ASSETS corpus (Oicw_1R.3di), values range across [-1, +1] with
    // arbitrary magnitudes — not pure padding (325/2379 are 0.0, the rest
    // are non-zero floats) and not a ±1.0 tangent handedness sign (neither
    // 0x3F800000 nor 0xBF800000 appears exactly).  Consistent with a
    // 4-component normal vector stored as (nx, ny, nz, nw).  The VStream
    // section carries the tangent/binormal data separately (24 bytes/vert).
    // dfvas::load_gpm_model_0 GPS-vertex read @ 0x50ba14: v14[7] = this field.
    // See notes/gp-corpus-probe-gps-extra-float-phase-4.md.
    float normal_w;
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
    // 3DA: rotate_type, scale_type, transform_as, yaw_rate, pitch_rate, roll_rate
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
    // 0x10..0x1C: 4 dwords per batch entry that dfvas's loader does not
    // access (load_rmodel_resource @ 0x510650 only reads offsets 0..0x0C
    // in its submesh loop).  Always zero in 3/639 corpus fixtures that
    // contained batch entries (all 639 files probed 2026-05-08).
    // See notes/gp-corpus-probe-batch-phase-2.md.
    uint32_t pad_batch_10;
    uint32_t pad_batch_14;
    uint32_t pad_batch_18;
    uint32_t pad_batch_1C;
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
    // Offsets 0x10..0x20: always zero across 639-fixture / 4047-entry corpus.
    // dfvas::load_gpm_model_0 reads all 60 bytes from disk but never acts on
    // these 5 dwords.  load_rmodel_resource @ 0x510bd3 only reads +0x24.
    // Corpus probe: 2026-05-08, _dump_gp_material_lookup, 639 files, 4047 entries.
    uint32_t pad_lookup_10;     // 0x10: Always zero (corpus-confirmed padding)
    uint32_t pad_lookup_14;     // 0x14: Always zero (corpus-confirmed padding)
    uint32_t pad_lookup_18;     // 0x18: Always zero (corpus-confirmed padding)
    uint32_t pad_lookup_1C;     // 0x1C: Always zero (corpus-confirmed padding)
    uint32_t pad_lookup_20;     // 0x20: Always zero (corpus-confirmed padding)
    uint8_t seq_index;          // 0x24: Sequential index (0,1,2...)
    uint8_t pad_25;             // 0x25: Zero padding
    uint8_t flags_26;           // 0x26: Unknown flags
    uint8_t slot_type;          // 0x27: Texture slot type (0x02=diffuse, 0x04=detail/overlay)
    uint16_t tex_width;         // 0x28: Texture width (encoded)
    uint16_t tex_height;        // 0x2A: Texture height (encoded)
    uint32_t runtime_2C;        // 0x2C: Runtime-only (zeroed on load)
    uint32_t runtime_30;        // 0x30: Runtime-only (zeroed on load)
    // 0x34 / 0x38: 2 dwords always zero across 4,047 lookup entries in 639/639
    // fixtures (corpus probe 2026-05-08, _dump_gp_material_lookup).
    // dfvas::load_gpm_model_0 reads them from disk but the post-load loop only
    // zeroes +0x2C and +0x30; neither load_gpm_model_0 nor load_rmodel_resource
    // @ 0x510bd3 reads or writes +0x34 / +0x38 after loading.  Likely additional
    // runtime slots or padding.
    // See notes/gp-corpus-probe-material-lookup-tail-phase-2.md.
    uint32_t pad_lookup_34;     // 0x34: Always zero (corpus-confirmed padding)
    uint32_t pad_lookup_38;     // 0x38: Always zero (corpus-confirmed padding)
} ThreediGpMaterialLookup;

// ============================================================================
// GP VStream (tangent/binormal data for GPS)
// ============================================================================

typedef struct ThreediGpVStream {
    uint32_t buffer_ptr;        // Runtime pointer (zero on disk)
    uint32_t data_size;         // Size of data blob (0 = derive from rverts_count * 24)
    // +0x08 / +0x0C: Always zero in 113/113 VStream-bearing fixtures (639-file
    // AS_ASSETS corpus).  dfvas::load_gpm_model_0 @ 0x50b710 reads these into
    // the runtime buffer verbatim but does not test or use them.  Downstream
    // code (not traced in this RE pass) could read them; for now documented as
    // padding.  See notes/gp-corpus-probe-vstream.md.
    uint32_t pad_vstream_08;
    uint32_t pad_vstream_0C;
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

    // RModel header section 0x20-0x4C (Phase 1 + Phase 4 Task 1 named).
    // dfvas::load_rmodel_resource @ 0x510650 does NOT read these fields —
    // they are metadata consumed by the LOD-selection / rendering path, not
    // the loader.  Corpus: 639 BHD GPM files from AS_ASSETS (1598 rmodels).
    //
    // 0x20: Q16.16 fixed-point LOD distance threshold.  Skinned character
    //   models: 3.0 (0x00030000) or 4.0 (0x00040000); flag models: 7.0
    //   (0x00070000); static geometry: 0.  Read by the rendering pass to
    //   decide when to switch LOD levels or disable skinning/animation.
    uint32_t lod_dist_threshold_q16;  // 0x20
    uint32_t pad_lod_24;               // 0x24: zero in 1598/1598 rmodels (639 files)

    // 0x28: Animated-LOD count.  Always 0 or 3 in corpus.  3 means the first
    //   three LODs carry full articulated geometry (skinned chars, mounted
    //   guns); 0 means static or flag cloth (no bone-driven geometry).
    //   dfvas::load_rmodel_resource @ 0x510650 does not reference this field.
    uint32_t lod_anim_lod_count;      // 0x28
    uint32_t pad_lod_2C;               // 0x2C: zero in 1598/1598 rmodels (639 files)
    uint32_t pad_lod_30;               // 0x30: zero in 1598/1598 rmodels (639 files)
    uint32_t pad_lod_34;               // 0x34: zero in 1598/1598 rmodels (639 files)

    // 0x38: Secondary Q16.16 LOD distance threshold.  Non-zero only on flag
    //   models (Flagblu/Flaggrn/Flagred): 4.0 (0x00040000).  Likely the
    //   cloth-sim activation radius.
    uint32_t lod_secondary_dist_q16;  // 0x38
    uint32_t pad_lod_3C;               // 0x3C: zero in 1598/1598 rmodels (639 files)
    uint32_t pad_lod_40;               // 0x40: zero in 1598/1598 rmodels (639 files)
    uint32_t pad_lod_44;               // 0x44: zero in 1598/1598 rmodels (639 files)

    // 0x48 / 0x4C: Zero in all 639 corpus files.
    //   dfvas::load_rmodel_resource @ 0x510650 reads but never uses these.
    uint32_t pad_lod_48;               // 0x48: zero in 1598/1598 rmodels (Phase 1)
    uint32_t pad_lod_4C;               // 0x4C: zero in 1598/1598 rmodels (Phase 1)

    ThreediGpExtraPoly *extra_polys;
    uint32_t extra_poly_count;  // 0x60: Number of extra polys (88 bytes each)
    uint32_t extra_xform_count; // 0x68: Number of extra transforms

    // RModel header tail at 0x6C..0x84: 7 dwords always zero in 1598/1598
    // rmodels (639 corpus files).  dfvas::load_rmodel_resource @ 0x510650
    // does not access these offsets.
    // See notes/gp-corpus-probe-rmodel-discards-phase-4.md.
    uint32_t pad_lod_6C;               // 0x6C
    uint32_t pad_lod_70;               // 0x70
    uint32_t pad_lod_74;               // 0x74
    uint32_t pad_lod_78;               // 0x78
    uint32_t pad_lod_7C;               // 0x7C
    uint32_t pad_lod_80;               // 0x80
    uint32_t pad_lod_84;               // 0x84

    // 20-byte RModel-level metadata block in the global_batch path (flags & 1).
    // Sits immediately after the total_batch dword (ONCE per RModel, not once per
    // subobject).  All 5 dwords always zero in corpus — see
    // notes/gp-corpus-probe-global-batch-metadata-phase-4.md.
    // dfvas::load_rmodel_resource @ 0x510650 reads these from disk but corpus
    // shows no non-zero patterns.  Phase 4 Task 3.
    uint32_t pad_global_batch_00;      // +0 from block start: always zero
    uint32_t pad_global_batch_04;      // +4: always zero
    uint32_t pad_global_batch_08;      // +8: always zero
    uint32_t pad_global_batch_0C;      // +C: always zero
    uint32_t pad_global_batch_10;      // +10: always zero

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
    uint8_t color_start[3];     // RGB start color (0-255)
    uint8_t color_end[3];       // RGB end color (0-255)
    float position[3];          // Position (x, y, z)
    float attenuation_start;    // Falloff start distance
    float attenuation_end;      // Falloff end distance
    int32_t part_index;         // Attached part index
    // 0x24..0x2C: 3 dwords always zero across 93 light entries in 639/639
    // fixtures.  dfvas::load_gpm_model_0 light loop @ 0x50c08b-0x50c0c6 does
    // not access these offsets; likely padding or renderer-side metadata.
    // See notes/gp-corpus-probe-light-entry-phase-2.md.
    uint32_t pad_light_entry_24; // Always zero across full corpus
    uint32_t pad_light_entry_28; // Always zero across full corpus
    uint32_t pad_light_entry_2C; // Always zero across full corpus
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
    // 0x2C..0x3B: 4 dwords always zero across 592 occlusion objects in
    // 639 fixtures.  dfvas::load_gpm_occdata @ 0x50fd70 doesn't access
    // these offsets.  See notes/gp-corpus-probe-occlusion-object-phase-3.md.
    int32_t pad_occ_obj_2C;
    int32_t pad_occ_obj_30;
    int32_t pad_occ_obj_34;
    int32_t pad_occ_obj_38;
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
    // +0x2A: 2-byte tail.  Always zero across 465,598 faces in 639 fixtures.
    // dfvas::load_gpm_model @ 0x50fb30 reads the collision data buffer in one
    // chunk (file_read_0); it does not access this offset individually.  See
    // notes/gp-corpus-probe-collision-face-phase-3.md.
    int16_t pad_face_2A;
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
    // +0x28..+0x33: 3 dwords always zero across 3,815 objects in 639 fixtures.
    // dfvas::load_gpm_model @ 0x50fb30 reads the collision buffer in one chunk
    // and does not access these offsets individually.
    // See notes/gp-corpus-probe-collision-object-phase-3.md.
    int32_t pad_object_28;
    int32_t pad_object_2C;
    int32_t pad_object_30;
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
    // +0x70..+0x7F: 4 dwords always zero across 3,815 objects in 639 fixtures.
    // See notes/gp-corpus-probe-collision-object-phase-3.md.
    int32_t pad_object_70;
    int32_t pad_object_74;
    int32_t pad_object_78;
    int32_t pad_object_7C;
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
    // 0x2C: 1 dword.  Always zero across 9,128 volumes in 639 fixtures.
    // dfvas::load_gpm_model @ 0x50fb30 doesn't access this offset.
    // See notes/gp-corpus-probe-collision-volume-phase-3.md.
    int32_t pad_volume_2C;
    int32_t bbox_min_x;
    int32_t bbox_max_x;
    int32_t bbox_min_y;
    int32_t bbox_max_y;
    int32_t bbox_min_z;
    int32_t bbox_max_z;
    int32_t plane_count;
    uint32_t plane_ptr;         // Runtime pointer (ignored on parse)
    // 0x50..0x5F: 4 dwords always zero across 9,128 volumes in 639 fixtures.
    // dfvas::load_gpm_model @ 0x50fb30 doesn't access these offsets.
    // See notes/gp-corpus-probe-collision-volume-phase-3.md.
    int32_t pad_volume_50;
    int32_t pad_volume_54;
    int32_t pad_volume_58;
    int32_t pad_volume_5C;
} ThreediGpCollisionVolume;

// Collision header size constant
#define THREEDI_GP_COLLISION_HEADER_SIZE 136

typedef struct ThreediGpCollision {
    uint32_t is_skinned;
    uint32_t data_size;
    // +0x08: Always zero on disk across the 639-fixture corpus.
    // dfvas::load_gpm_model @ 0x50fb30 overwrites this slot at load time
    // with the allocated data-buffer pointer (header[2] = data_ptr).
    // The 9 trailing runtime-pointer slots at +0x64..+0x84 are handled
    // identically — all zero on disk, populated at runtime with per-sub-array
    // offsets (vertices/normals/faces/objects/...).  Those 9 slots are
    // round-tripped via col->raw_header[136]; no IR storage needed.
    // See notes/gp-corpus-probe-collision-header-phase-3.md.
    uint32_t pad_collision_08;
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
    // 0x2C..0x38: 4 dwords following the per-light array.  Always zero in the
    // corpus (validated by gp_corpus_invariants_test); dfvas's GP loader does
    // not test these bytes — preserved for round-trip.
    uint32_t pad_light_2C;
    uint32_t pad_light_30;
    uint32_t pad_light_34;
    uint32_t pad_light_38;

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
