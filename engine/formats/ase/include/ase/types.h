// ASE document structures — C-compatible layout for FFI.
// Ported from ModSuperOed-derived types.
//
// Struct definitions live in the global namespace so they are accessible
// from both C and C++ (including extern "C" blocks).  A C++ namespace
// provides short aliases for internal use.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ase_UV {
  float u;
  float v;
  float w;
} ase_UV;

typedef struct ase_Weight {
  int32_t bone_index[4];
  float weight[4];
} ase_Weight;

typedef struct ase_Face {
  int32_t material_id;        // 0x00
  int32_t material_index;     // 0x04
  uint32_t smoothing_mask;    // 0x08
  int32_t vert[4];            // 0x0C (v0..v2, v_dup)
  uint8_t edge_visibility[4]; // 0x1C
  int32_t uv[4];              // 0x20 (t0..t3)
  int32_t color[3];           // 0x30 (Colori)
  int32_t reserved1;          // 0x3C
  int32_t reserved2;          // 0x40
} ase_Face;

typedef struct ase_MappingChannel {
  int32_t channel_id;
  int32_t tv_count;
  ase_UV* tverts;
  int32_t face_count;
  int32_t (*faces)[3];
} ase_MappingChannel;

typedef struct ase_Object {
  char name[64];                       // 0x00
  int32_t node_id;                     // 0x40
  int32_t material_ref;                // 0x44
  int32_t mapping_channel_count;       // 0x48
  ase_MappingChannel* mapping_channels; // 0x4C
  char parent_name[64];                // 0x50
  int32_t vert_count;                  // 0x90
  float* verts;                        // 0x94 (triples of float)
  int32_t weight_count;                // 0x98
  ase_Weight* weights;                 // 0x9C
  int32_t uv_count;                    // 0xA0
  ase_UV* uvs;                         // 0xA4
  int32_t face_count;                  // 0xA8
  ase_Face* faces;                     // 0xAC
  int32_t color_count;                 // 0xB0
  uint32_t* colors;                    // 0xB4 (packed RGB)
  int32_t color_face_count;            // 0xB8
  float tm_row[4][3];                  // 0xBC
  int32_t skinned;                     // 0xEC
  // Pre-computed per-face-vertex normals from IR (bypasses smoothing group
  // recomputation during roundtrip).  When non-NULL, face_normal_count ==
  // face_count and the array contains face_count*9 floats: for each face,
  // 3 vertices × (nx, ny, nz).  Stored in the ASE parser's swizzled
  // coordinate space (same as verts[]).
  int32_t face_normal_count;
  float* face_normals;                 // face_count * 9 floats, or NULL
} ase_Object;

typedef struct ase_Light {
  char name[64];
  int32_t type;
  float pos[3];
  float color[3];
  float intensity;
  float atten_start;
  float atten_end;
  float near_atten_start;
  float near_atten_end;
  float hotspot;
  float falloff;
  float tm_row2[3];
} ase_Light;

typedef struct ase_Material {
  uint32_t flags;
  char name[32];
  char maps[4][32];
  float uv_u_offset[2];
  float uv_v_offset[2];
  float uv_u_tiling[2];
  float uv_v_tiling[2];
  uint32_t extra_flags;
  uint32_t has_submaterials;
  float ambient[3];
  float diffuse[3];
  float specular[3];
  float shine;
  float shine_strength;
  float transparency;
  float wiresize;
  int32_t shading;            // 0=Blinn, 1=Phong, 2=Metal, 3=Constant
  int32_t submaterial_count;
  struct ase_Material* submaterials;
} ase_Material;

typedef struct ase_Document {
  uint8_t _reserved[64];      // unverified fields from original
  int32_t object_count;       // 0x40
  int32_t light_count;        // 0x44
  int32_t material_count;     // 0x48
  uint32_t flags;             // 0x4C
  ase_Object* objects;        // 0x50
  ase_Light* lights;          // 0x54
  ase_Material* materials;    // 0x58
  int32_t skinned_flags;      // 0x5C
} ase_Document;

#ifdef __cplusplus
namespace ase {

// C++ aliases for internal use
using UV = ase_UV;
using Weight = ase_Weight;
using Face = ase_Face;
using MappingChannel = ase_MappingChannel;
using Object = ase_Object;
using Light = ase_Light;
using Material = ase_Material;
using Document = ase_Document;

}  // namespace ase
#endif
