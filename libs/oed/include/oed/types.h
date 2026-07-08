#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace oed {

// 32-bit layouts mirrored from ModSuperOed (IDA types). Do not reorder/resize.

// Material info flags (matches gMaterialInfoTable flags in ModSuperOed).
//
// Bit meanings are witnessed against retail Jointops.exe, where the runtime
// builds the same word per effect at .fx load: the FF_* rows are AUTHORED
// ([orig: HLSLEffect_InitFixedFunctionShaders @ 0x5af790 — _ST 0x4 / _MT 0xC,
// _AB 0x1002 / _AD 0x1000, _LUM 0x10000001, #UV |0x10000]) and the file-effect
// rows are PROBED per technique ([orig: HLSLEffect_LoadFromFile @ 0x5ae690 —
// TexDiffuse1 0x4, TexDiffuse2 0x8, TexNormal1 0x10, TexNormal2 0x20,
// TexHorizon 0x40, TexOcclusion 0x80, TexSpecularCtrl 0x100, DisplaceAmount
// 0x200, "blending" annotation 0x1000, ReflectColor 0x2000,
// SkinWorldMatrixArray 0x4000, TANGENT input semantic 0x8000, EffectAlt_UV
// variant 0x10000]). The 0x10000000 dialects RESOLVED at REN-4: the file-effect
// probe sets it for "uses TexCubeRotSpecular" [orig: @ 0x5af04b] and the FF
// path authors it on the _LUM rows [orig: selflum table @ 0x5afa36] — both mean
// the same runtime capability, "renders a glow/bloom copy": the batch queue's
// Q3 duplicate is gated on this bit [orig: collect_render_objects_for_batch
// @ 0x5d93b5], and the probe booleans are UNIONS over ALL techniques [orig:
// HLSLEffect_LoadFromFile @ 0x5ae690 technique loop], so at runtime FFP_GLASS
// (whose GLOW technique samples TexCubeRotSpecular — Glass.fx) carries it even
// though this OED-dump table does not. The self-lum LOOK is the EMISSIVE bit
// (0x1); GLOW is the bloom-copy capability. See
// docs/render/render-material-re.md D-RMAT-4.
enum MaterialInfoTypeFlags : uint32_t {
  MATERIAL_FLAG_EMISSIVE  = 0x0001,   // self-lum look (FF _LUM rows author 0x10000001)
  MATERIAL_FLAG_ALPHA     = 0x0002,   // alpha-blend (AB) variant (FF _AB rows author 0x1002)
  MATERIAL_FLAG_DIFFUSE   = 0x0004,   // uses TexDiffuse1
  MATERIAL_FLAG_SECONDARY = 0x0008,   // uses TexDiffuse2 (detail/multi-texture)
  MATERIAL_FLAG_NORMAL_A  = 0x0010,   // uses TexNormal1
  MATERIAL_FLAG_NORMAL_B  = 0x0020,   // uses TexNormal2
  MATERIAL_FLAG_BLENDING  = 0x1000,   // technique declares a "blending" annotation
  MATERIAL_FLAG_GLASS     = 0x2000,   // uses ReflectColor (glass/reflective)
  MATERIAL_FLAG_SKINNED   = 0x4000,   // uses SkinWorldMatrixArray
  MATERIAL_FLAG_TANGENT   = 0x8000,   // vertex shader consumes the TANGENT semantic
  MATERIAL_FLAG_UVGEN     = 0x10000,  // the TEX_UVXFORM (#UV / ", UVGen") variant
  MATERIAL_FLAG_GLOW      = 0x10000000,  // glow/bloom-copy capable (Q3 duplicate; dialect note above)
};

struct MaterialInfoRecord {
  const char name[24];
  MaterialInfoTypeFlags flags;
};

// The runtime shader-tag registry, as retail Jointops.exe builds it at boot:
// 24 FF_* built-ins compiled from _FFP.fx [orig: HLSLEffect_InitFixedFunctionShaders
// @ 0x5af790], the shipped localres.pff .fx effect tags (underscore-prefixed
// includes are skipped) [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5afed0 /
// HLSLEffect_LoadFromFile @ 0x5ae690], and a "#UV" twin for every effect that
// declares EffectAlt_UV [orig: @ 0x5aea03]. Identical to ModSuperOed's dumped
// gMaterialInfoTable plus VS_TRACER (present in the runtime registry; absent
// from OED's authorable table).
inline constexpr MaterialInfoRecord kMaterialInfoTable[] = {
    {"FF_ST_OP", MATERIAL_FLAG_DIFFUSE},
    {"FF_ST_OP#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AB", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AB#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AD", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AD#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_OP_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE)},
    {"FF_ST_OP_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AB_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AB_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_ST_AD_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"FF_ST_AD_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_OP", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"FF_MT_OP#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AB", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AB#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AD", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AD#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_OP_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"FF_MT_OP_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AB_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AB_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_ALPHA | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FF_MT_AD_LUM", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING)},
    {"FF_MT_AD_LUM#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLOW | MATERIAL_FLAG_EMISSIVE | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY | MATERIAL_FLAG_BLENDING | MATERIAL_FLAG_UVGEN)},
    {"FFP_GLASS", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_BLENDING)},
    {"VS_DOT3DIFFOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_PHONGO", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_DOT3DIFF", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_DOT3DIFF#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_PHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_PHONGT#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_DOT3DIFF2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_BMTXMIRRT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_BUMPMIRRT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_ENVPHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_TANGENT | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBASIC", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBASIC#UV", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_UVGEN)},
    {"VS_SKGLASS", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_BLENDING)},
    {"VS_SKBUMPDIFFOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPPHONGOBJ", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPDIFFOBJ2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_SKBUMPDIFFT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPPHONGT", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE)},
    {"VS_SKBUMPDIFFT2", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_GLASS | MATERIAL_FLAG_SKINNED | MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_DIFFUSE | MATERIAL_FLAG_SECONDARY)},
    {"VS_FLAG", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE)},
    // Runtime-registry row absent from OED's table: Tracer.fx registers
    // VS_TRACER (EffectSpecial=true, TECHNIQUE_NORMAL with usevs/ZMODE_NOWRITE,
    // TexDiffuse1 only, RSAlphaMode(TRUE, ONE, ONE)). The probed flag word is
    // DIFFUSE only — Tracer.fx declares no "blending" annotation (it sorts via
    // EffectSpecial instead). docs/render/render-material-re.md D-RMAT-2.
    {"VS_TRACER", static_cast<MaterialInfoTypeFlags>(MATERIAL_FLAG_DIFFUSE)},
};
inline constexpr size_t kMaterialInfoTableCount = std::size(kMaterialInfoTable);

struct Vec3 {
  float x;
  float y;
  float z;
};
static_assert(sizeof(Vec3) == 12, "Vec3 layout mismatch");

struct CollisionPlane {
  Vec3 n;
  float d;
  uint32_t flags;
};
static_assert(sizeof(CollisionPlane) == 20, "CollisionPlane layout mismatch");

struct Vertex {
  Vec3 pos;              // 0x00
  int32_t boneIndex[4];  // 0x0C
  float boneWeight[4];   // 0x1C
  uint32_t flags;        // 0x2C
};
static_assert(sizeof(Vertex) == 48, "Vertex layout mismatch");

struct Uv {
  float uv[2];  // 0x00
};
static_assert(sizeof(Uv) == 8, "Uv layout mismatch");

struct Face {
  int32_t matIndex;          // 0x00 ? final computed material bucket index
  int32_t srcMatId;          // 0x04 ? ASE source material index
  int32_t smoothingGroup;    // 0x08 ? smoothing group bitmask
  uint32_t vert[4];          // 0x0C ? vertex indices (4th is duplicate of 3rd for quads)
  uint32_t uv[4];            // 0x1C ? UV coordinate indices
  uint32_t edge_visibility[4]; // 0x2C ? edge visibility flags (stored as int, low byte used)
  float faceBasis[3][3];     // 0x3C ? negated face basis: [0]=normal, [1]=tangent, [2]=binormal
  uint32_t planeIndex;       // 0x60 ? index into collision plane array
  uint32_t srcObjIndex;      // 0x64 ? source ASE object index
  uint8_t matSlot;           // 0x68 ? material slot assignment
  uint8_t hasDollar;         // 0x69 ? 1 if ASE object name contains '$'
  uint8_t collOverlap;       // 0x6A ? 1 if face AABB overlaps a CB collision bbox
  uint8_t pad6b;             // 0x6B
};
static_assert(sizeof(Face) == 108, "Face layout mismatch");

struct Colori {
  int r;
  int g;
  int b;
};
static_assert(sizeof(Colori) == 12, "Colori layout mismatch");

struct FaceColor {
  Colori rgb;  // 0x00
};
static_assert(sizeof(FaceColor) == 12, "FaceColor layout mismatch");

struct Point92 {
  char name[32];   // 0x00
  float pos[3];    // 0x20
  float axis[4][3];// 0x2C
};
static_assert(sizeof(Point92) == 92, "Point92 layout mismatch");

struct UserPoint {
  char name[32];   // 0x00
  float pos[3];    // 0x20
  int32_t subObj;  // 0x2C
  uint32_t type;   // 0x30
  float axis[4][3];// 0x34
};
static_assert(sizeof(UserPoint) == 100, "UserPoint layout mismatch");

struct Color {
  float r;
  float g;
  float b;
};
static_assert(sizeof(Color) == 12, "Color layout mismatch");

struct LightColorGen {
  int32_t style;       // 0x00
  float rate;          // 0x04
  float phase;         // 0x08
  Colori start;        // 0x0C
  Colori end;          // 0x18
  char ctrlReg[64];    // 0x24
};
static_assert(sizeof(LightColorGen) == 100, "LightColorGen layout mismatch");

struct Light {
  char name[32];        // 0x00
  int32_t type;         // 0x20
  float x;              // 0x24
  float y;              // 0x28
  float z;              // 0x2C
  Color color;          // 0x30
  float intensity;      // 0x3C
  float near_atten_start; // 0x40
  float atten_end;      // 0x44
  LightColorGen colorgen; // 0x48
  int32_t subObj;       // 0xAC
  int32_t disable_corona;      // 0xB0
  int32_t disable_lightterrain;// 0xB4
  int32_t disable_lightobjects;// 0xB8
  float hotspot;        // 0xBC
  float falloff;        // 0xC0
  float rotX;           // 0xC4
  float rotY;           // 0xC8
  float rotZ;           // 0xCC
};
static_assert(sizeof(Light) == 208, "Light layout mismatch");

struct Material {
  uint32_t flags;      // 0x00
  char name[32];       // 0x04
  char maps[4][32];    // 0x24
  float uv_u_offset[2]; // 0xA4
  float uv_v_offset[2]; // 0xAC
  float uv_u_tiling[2]; // 0xB4
  float uv_v_tiling[2]; // 0xBC
  uint32_t extra_flags; // 0xC4
};
static_assert(sizeof(Material) == 200, "Material layout mismatch");

struct SubObject {
  char name[32];     // 0x00
  uint32_t flags;    // 0x20
  uint32_t unk24;    // 0x24
  uint32_t attachIndex; // 0x28
  uint32_t unk2c;    // 0x2C
  uint32_t vertCount;// 0x30
  Vertex* verts;     // 0x34
  uint32_t uvCount;  // 0x38
  Uv* uvs;           // 0x3C
  int32_t faceCount; // 0x40
  Face* faces;       // 0x44
  uint32_t colorCount; // 0x48
  FaceColor* colors; // 0x4C
  uint32_t collisionCount; // 0x50
  // Pre-computed per-face-vertex normals from IR (faceCount * 9 floats).
  // When non-NULL, build_render_geometry uses these instead of recomputing
  // from smoothing groups.  Layout: for face i, vertex j (0..2):
  //   preSmoothedNormals[i*9 + j*3 + 0..2] = (nx, ny, nz)
  float* preSmoothedNormals;  // NULL when not available
};
// static_assert omitted: SubObject contains pointers, size varies by platform.

struct Collision {
  char name[32];    // 0x00
  uint32_t bvolFlags; // 0x20
  uint32_t collidableType; // 0x24
  uint32_t objectIndex; // 0x28
  uint32_t sub_index;   // 0x2C
  Vec3 center;          // 0x30
  float radius;         // 0x3C
  float radiusXY;       // 0x40
  float height;         // 0x44
  float halfExtents[3]; // 0x48
  uint32_t unk54;       // 0x54
  struct {
    float minX;
    float maxX;
    float minY;
    float maxY;
    float minZ;
    float maxZ;
  } bbox;               // 0x58
  uint32_t vertCount;   // 0x70
  Vertex* verts;        // 0x74
  uint32_t faceCount;   // 0x78
  Face* faces;          // 0x7C
  CollisionPlane planes[32]; // 0x80
  uint32_t planeCount;  // 0x300
  uint32_t unused;      // 0x304
};
// static_assert omitted: Collision contains pointers, size varies by platform.

struct MaterialAnimMeta {
  uint32_t anim_frames;     // 0x00
  uint32_t anim_type;       // 0x04
  float anim_frametime;     // 0x08
  char ctrl_reg[64];        // 0x0C
};
static_assert(sizeof(MaterialAnimMeta) == 76, "MaterialAnimMeta layout mismatch");

struct MaterialTexSlot {
  char path[16];  // 0x00
  int32_t flags;  // 0x10
};
static_assert(sizeof(MaterialTexSlot) == 20, "MaterialTexSlot layout mismatch");

struct MaterialAnimTextures {
  MaterialTexSlot normal[2];           // 0x00
  MaterialTexSlot anim_diffuse[2][8];  // 0x28
  MaterialTexSlot anim_normal[2][8];   // 0x168
};
static_assert(sizeof(MaterialAnimTextures) == 680, "MaterialAnimTextures layout mismatch");

// Generator parameter block for UV map functions (u/v).
// Layout: style(4) + rate(4) + phase(4) + start(4) + end(4) + ctrlReg(64) = 84 bytes.
struct MaterialMapFunc {
  int32_t style;       // 0x00
  float rate;          // 0x04
  float phase;         // 0x08
  float start;         // 0x0C
  float end;           // 0x10
  char ctrlReg[64];    // 0x14
};
static_assert(sizeof(MaterialMapFunc) == 84, "MaterialMapFunc layout mismatch");

// RGB generator: style + rate + phase + start_rgb(3 ints) + end_rgb(3 ints) + ctrlReg(64) = 100 bytes.
struct MaterialRgbGen {
  int32_t style;       // 0x00
  float rate;          // 0x04
  float phase;         // 0x08
  Colori srgb;         // 0x0C  (start color)
  Colori ergb;         // 0x18  (end color)
  char ctrlReg[64];    // 0x24
};
static_assert(sizeof(MaterialRgbGen) == 100, "MaterialRgbGen layout mismatch");

// Alpha generator: style + rate + phase + start(float) + end(float) + ctrlReg(56) = 76 bytes.
// Note: start/end are floats internally but written as ints to 3DP files.
struct MaterialAlphaGen {
  int32_t style;       // 0x00
  float rate;          // 0x04
  float phase;         // 0x08
  float start;         // 0x0C
  float end;           // 0x10
  char ctrlReg[56];    // 0x14  (8 bytes shorter than MapFunc/RgbGen)
};
static_assert(sizeof(MaterialAlphaGen) == 76, "MaterialAlphaGen layout mismatch");

// All material generators. 4-byte pad at front, then u/v map funcs, rgb gen, alpha gen.
// Verified via disassembly: u_params starts at MaterialBucketSlot+0x3BC (gens+4).
struct MaterialGenerators {
  uint32_t pad_head;           // 0x00  (4 bytes, unused ? shifts sub-structs by 4)
  MaterialMapFunc u_params;    // 0x04  (84 bytes)
  MaterialMapFunc v_params;    // 0x58  (84 bytes)
  MaterialRgbGen rgb_gen;      // 0xAC  (100 bytes)
  MaterialAlphaGen alpha_gen;  // 0x110 (76 bytes)
};
static_assert(sizeof(MaterialGenerators) == 348, "MaterialGenerators layout mismatch");

struct MaterialBucketSlot {
  uint32_t initialized;     // 0x00
  char name[80];            // 0x04
  uint32_t rattrib;         // 0x54
  uint32_t pattrib;         // 0x58
  uint32_t ptype;           // 0x5C
  uint32_t geofx;           // 0x60
  float geofx_value;        // 0x64
  uint32_t tex3_stage_count; // 0x68
  uint32_t tex3_enabled;    // 0x6C
  MaterialAnimMeta anim_meta; // 0x70
  MaterialTexSlot tex1;     // 0xBC
  MaterialTexSlot tex2;     // 0xD0
  MaterialAnimTextures anim_textures; // 0xE4
  uint32_t present;         // 0x38C
  uint32_t flagsA;          // 0x390
  float uv0_u_offset;       // 0x394
  float uv1_u_offset;       // 0x398
  float uv0_v_offset;       // 0x39C
  float uv1_v_offset;       // 0x3A0
  float uv0_u_tiling;       // 0x3A4
  float uv1_u_tiling;       // 0x3A8
  float uv0_v_tiling;       // 0x3AC
  float uv1_v_tiling;       // 0x3B0
  uint8_t alphatestvalue;   // 0x3B4
  uint8_t pad_alphatest[3]; // 0x3B5
  MaterialGenerators gens;  // 0x3B8
  uint32_t pad_514;         // 0x514
  uint32_t pad_518;         // 0x518
  int glass_reflect_hi;     // 0x51C
  int glass_reflect_mid;    // 0x520
  int glass_reflect_lo;     // 0x524
  char shader_name[24];     // 0x528
  int32_t p1;               // 0x540
  int32_t p2;               // 0x544
};
static_assert(sizeof(MaterialBucketSlot) == 1352, "MaterialBucketSlot layout mismatch");

struct MaterialBucket {
  float min[3];      // 0x00
  float max[3];      // 0x0C
  float center[3];   // 0x18
  uint32_t groupCount; // 0x24
  uint32_t groupIds[128]; // 0x28
  float groupMin[128][3]; // 0x228
  float groupMax[128][3]; // 0x828
  float groupCenter[128][3]; // 0xE28
};
static_assert(sizeof(MaterialBucket) == 5160, "MaterialBucket layout mismatch");

struct MaterialTable {
  uint32_t count;                          // 0x00
  MaterialBucketSlot slots[1002];          // 0x04
};
static_assert(sizeof(MaterialTable) == 1354708, "MaterialTable layout mismatch");

struct PartAnimFunc {
  int func;          // 0x00
  float param0;      // 0x04
  float param1;      // 0x08
  float param2;      // 0x0C
  float param3;      // 0x10
  char ctrlReg[64];  // 0x14
};
static_assert(sizeof(PartAnimFunc) == 84, "PartAnimFunc layout mismatch");

struct PartAnimSubobject {
  int rotate_type;   // 0x00
  int scale_type;    // 0x04
  int trans_type;    // 0x08
  int transform_as;  // 0x0C
  float yaw_rate;    // 0x10
  float pitch_rate;  // 0x14
  float roll_rate;   // 0x18
  PartAnimFunc yaw_func;    // 0x1C
  PartAnimFunc pitch_func;  // 0x70
  PartAnimFunc roll_func;   // 0xC4
  int reverse_rotate;       // 0x118
  PartAnimFunc scale_func;  // 0x11C
  PartAnimFunc scalex_func; // 0x170
  PartAnimFunc scaley_func; // 0x1C4
  PartAnimFunc scalez_func; // 0x218
  PartAnimFunc transx_func; // 0x26C
  PartAnimFunc transy_func; // 0x2C0
  PartAnimFunc transz_func; // 0x314
};
static_assert(sizeof(PartAnimSubobject) == 872, "PartAnimSubobject layout mismatch");

struct PartAnimHeader {
  uint8_t pad[216];        // 0x00
  uint32_t enable_part_anim; // 0xD8
};
static_assert(sizeof(PartAnimHeader) == 220, "PartAnimHeader layout mismatch");

struct PartAnimData {
  PartAnimHeader header;           // 0x00
  PartAnimSubobject slots[64];     // 0xDC
};
static_assert(sizeof(PartAnimData) == 56028, "PartAnimData layout mismatch");

struct MaterialBucketBounds {
  float bboxMin[3];       // 0x00
  float bboxMax[3];       // 0x0C
  float bboxMid[3];       // 0x18
  uint32_t slotCount;     // 0x24
  int32_t slotSubobjects[128]; // 0x28
  float slotMin[128][3];  // 0x228
  float slotMax[128][3];  // 0x828
  float slotMid[128][3];  // 0xE28
};
static_assert(sizeof(MaterialBucketBounds) == 5160, "MaterialBucketBounds layout mismatch");

struct LodHeader {
  uint8_t pad0[36];    // 0x00
  uint32_t flags;      // 0x24
  uint8_t render_function[36]; // 0x28 (function pointer blob)
  float bboxMin[3];    // 0x4C
  float bboxMax[3];    // 0x58
  float maxRadius;     // 0x64
  float maxRadiusXY;   // 0x68
  float maxRadiusZ;    // 0x6C
  int32_t subobjectCount; // 0x70
  SubObject* subobjects;   // 0x74
  uint32_t attachCount;    // 0x78
  Point92* attachPoints;   // 0x7C
  uint32_t centerCount;    // 0x80
  Point92* centerPoints;   // 0x84
  uint32_t totalCollPlanes;// 0x88
  uint32_t collisionCount; // 0x8C
  Collision* collisions;   // 0x90
  uint32_t userPointCount; // 0x94
  UserPoint* userPoints;   // 0x98
  uint32_t lightCount;     // 0x9C
  Light* lights;           // 0xA0
  uint32_t materialCount;  // 0xA4
  Material* materials;     // 0xA8
};
// static_assert omitted: LodHeader contains pointers, size varies by platform.

struct LodBucketWorkspace {
  LodHeader lod;                     // 0x00
  PartAnimData part_anim;            // 0xAC
  MaterialBucketBounds buckets[64];  // 0xDB88
};
// static_assert omitted: LodBucketWorkspace contains pointers (via LodHeader).

struct LodWorkSlot {
  LodBucketWorkspace work;
};
// LodWorkSlot is always the same size as LodBucketWorkspace by construction.

struct InternalState {
  MaterialTable material_table{};
  LodBucketWorkspace workspace{};
};

// Release heap allocations inside LodHeader.
void free_internal(LodHeader& lod);
void free_internal(LodBucketWorkspace& work);
void free_internal(InternalState& state);

}  // namespace oed
