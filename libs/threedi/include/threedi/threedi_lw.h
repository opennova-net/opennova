// Land Warrior (LW) .3di format definitions and parser.
// LW is the oldest .3di lineage: bare "3DI" magic + a numeric version byte,
// predating the GP-era ("GPM"/"GPS"/"GPP") and modern "3DI3" formats.
//
// v10 layout reverse-engineered from Dflw.exe (see notes/3di-lw/lw-3di-format.md).
// v8 files are detected only so callers can reject them explicitly.

#ifndef THREEDI_LW_H
#define THREEDI_LW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Versions
// ============================================================================

typedef enum ThreediLwVersion {
    THREEDI_LW_VERSION_UNKNOWN = 0,
    THREEDI_LW_VERSION_8 = 8,   // older engine; magic "3DI" + 0x08
    THREEDI_LW_VERSION_10 = 10  // Land Warrior client; magic "3DI" + 0x0A
} ThreediLwVersion;

// Detect whether the leading bytes are an LW .3di file.
// Returns the version (8 or 10), or THREEDI_LW_VERSION_UNKNOWN.
// Does NOT match the modern "3DI3" container (byte[3] == '3') nor GP formats.
ThreediLwVersion threedi_lw_detect(const uint8_t *data, size_t len);

// ============================================================================
// Material (v10: 80-byte on-disk record, LW3di_ReadMaterial @ 0x47e040)
// ============================================================================

typedef struct ThreediLwMaterial {
    char tex_name_0[17];   // +0x00 primary texture name (<=15 chars, null-terminated)
    char tex_name_1[17];   // +0x10 secondary texture name
    uint32_t group_id;     // +0x24
    uint16_t selector_id;   // +0x28 material selector compared against surface +0x34 variant bytes
    uint16_t flags;        // +0x2A (0x08 force-load, 0x100/0x200 has tex0/tex1, 0x80 dup)
    uint16_t tex_width;    // +0x2C (power of two, <=256)
    uint16_t tex_height;   // +0x2E
} ThreediLwMaterial;

// ============================================================================
// LOD geometry
// ============================================================================

// 8-byte vertex/normal record: int16 x,y,z,w. Vertex xyz are 24.8 fixed-point
// in LW's raw Z-up source frame; threedi_ir_from_lw swizzles them to the newer
// Y-up IR frame as raw x/y/z -> IR -y/z/x. For flag-1 LODs, vertex w carries
// the skeleton sub-object index used for the emitted skinning weight.
typedef struct ThreediLwVertex {
    int16_t x, y, z, w;
} ThreediLwVertex;

// One triangle (v10 on-disk record is 80 bytes). Vertex and normal indices are
// local to the owning sub-object's contiguous vertex/normal runs. The per-face
// material is resolved through surfaces[surface_index]; the surface's
// material_index is only a fallback in LW v10 files.
typedef struct ThreediLwFace {
    int32_t u[3];            // +0x04/+0x08/+0x0C  (tu1..tu3)
    int32_t v[3];            // +0x10/+0x14/+0x18  (tv1..tv3)
    int16_t vertex[3];       // +0x1C/+0x1E/+0x20
    int16_t normal[3];       // +0x22/+0x24/+0x26
    int32_t surface_index;   // +0x4C -> index into this LOD's surfaces[]
} ThreediLwFace;

// 120-byte sub-object record (LOD blob array [40]) — the rigid-part skeleton.
// Sub-objects partition the LOD's vertices, normals, and faces IN ORDER:
// sub-object i owns the next counted run after its predecessor.
typedef struct ThreediLwSubObject {
    uint32_t vertex_count;   // +0x04 vertices owned (contiguous run)
    uint32_t face_count;     // +0x0C faces owned (contiguous run)
    uint32_t normal_count;   // +0x1C normals owned (contiguous run)
    int32_t parent;          // +0x2C parent sub-object index (self/0 => root)
    int32_t pos[3];          // +0x3C/+0x40/+0x44 raw-frame rest pivot (16.16; used when LOD flags&1)
} ThreediLwSubObject;

// 128-byte named surface record (LOD blob array [44]). material_index is
// provisional: many authored LW v10 files use the surface name/reference data
// to link materials after load, and some surface indices are out of range for
// the local material table. Consumers should treat out-of-range indices as
// unresolved rather than blindly clamping.
typedef struct ThreediLwSurface {
    char name[17];           // +0x00 (<=15 chars)
    uint32_t flags;          // +0x10
    uint16_t material_index; // +0x18 (24) -> ThreediLwFile.materials fallback
    uint8_t anim_frames;     // +0x1E (30) -> flipbook frame span (flag 0x4000)
    uint8_t material_selectors[4]; // +0x34..+0x37 variant material selector bytes
} ThreediLwSurface;

// One LOD's parsed geometry. The on-disk LOD is a 232-byte header followed by a
// packed blob of 9 arrays (see notes/3di-lw/lw-3di-format.md §3.3/§3.4). The blob
// size equals the sum of count*stride across all arrays.
typedef struct ThreediLwLod {
    uint32_t blob_size;        // LOD-header dword[5]
    uint32_t flags;            // LOD-header dword[4]

    // array element counts (LOD-header dword indices in comments)
    uint32_t vertex_count;     // [32], 8-byte records
    uint32_t normal_count;     // [34], 8-byte records
    uint32_t faceref_count;    // [36], 80-byte records
    uint32_t array12_count;    // [38], 12-byte records
    uint32_t subobject_count;  // [40], 120-byte records (skeleton parts)
    uint32_t triindex_count;   // [42], 12-byte records (3x u32)
    uint32_t surface_count;    // [44], 128-byte named records
    uint32_t array8_count;     // [46], 8-byte records
    uint32_t array80_count;    // [48], 80-byte records

    ThreediLwVertex *vertices;  // vertex_count entries (blob offset 0)
    ThreediLwVertex *normals;   // normal_count entries
    ThreediLwFace *faces;       // faceref_count entries (triangles)
    ThreediLwSurface *surfaces; // surface_count entries (last in blob)
    ThreediLwSubObject *subobjects; // subobject_count entries (skeleton parts)
} ThreediLwLod;

// ============================================================================
// Parsed LW model
// ============================================================================

typedef struct ThreediLwFile {
    ThreediLwVersion version;
    char name[20];                  // model name (null-terminated)
    uint32_t lod_count;             // <= 4
    uint32_t lod_thresholds[3];     // Q16.16 distance thresholds (v10 header +0x18)
    char render_tags[4][5];         // per-LOD 4-char render tag (null-terminated)

    uint32_t material_count;
    ThreediLwMaterial *materials;   // material_count entries

    ThreediLwLod *lods;             // lod_count entries
} ThreediLwFile;

// Zero-initialize a file struct.
void threedi_lw_init(ThreediLwFile *out);

// Parse an LW .3di from a memory buffer. Returns 0 on success, -1 on error.
int threedi_lw_parse(const uint8_t *data, size_t len, ThreediLwFile *out);

// Read and parse an LW .3di from disk. Returns 0 on success, -1 on error.
int threedi_lw_read(const char *path, ThreediLwFile *out);

// Free all heap-allocated members and zero the struct.
void threedi_lw_free(ThreediLwFile *out);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_LW_H
