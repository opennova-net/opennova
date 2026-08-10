#include "oed/convert_internal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#ifndef _WIN32
#include <strings.h>
#endif
#include "oed/project_types.h"

namespace oed {
namespace {

struct Classification {
  int objType = 0;         // 1 center, 2 attach, 3 stretch, 4 subobject member, 5 collision, 6 user point, 7 light
  int collidableType = 0;  // for collision classes
  int objectIndex = -1;    // parsed numeric suffix (-1 when absent)
  int subIndex = 0;        // used by OP- classes
  uint32_t bvolFlags = 0;
  char userType = 0;       // single letter for UPxNN
};

struct StretchBox {
  int subObjA = -1;
  int subObjB = -1;
  Vec3 min{};
  Vec3 max{};
};

static inline bool starts_with(const char* s, const char* prefix) {
  if (!s || !prefix) return false;
  const size_t n = std::strlen(prefix);
  return std::strncmp(s, prefix, n) == 0;
}

static inline bool is_digit(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

static int parse_two_char_atol(const char* name, size_t offset) {
  char tmp[3] = {'\0', '\0', '\0'};
  if (name) {
    const size_t len = std::strlen(name);
    if (offset < len) tmp[0] = name[offset];
    if (offset + 1 < len) tmp[1] = name[offset + 1];
  }
  return static_cast<int>(std::strtol(tmp, nullptr, 10));
}

static int parse_two_char_index(const char* name, size_t offset) {
  return parse_two_char_atol(name, offset) - 1;
}

static inline void copy_str(char* dst, size_t dst_sz, const char* src) {
  if (!dst || dst_sz == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = '\0';
}

static inline int icase_cmp(const char* a, const char* b) {
#ifdef _WIN32
  return _stricmp(a, b);
#else
  return ::strcasecmp(a, b);
#endif
}

static Classification classify_name(const char* name, uint32_t prev_sub_index = 0) {
  Classification out{};
  out.subIndex = static_cast<int>(prev_sub_index);  // carry-over matches original stack behavior
  if (!name || !*name) return out;
  const size_t len = std::strlen(name);
  if (len == 0) return out;
  const char c0_raw = name[0];
  const char c1_raw = len > 1 ? name[1] : '\0';
  const char c0 = std::toupper(static_cast<unsigned char>(c0_raw));
  const char c1 = std::toupper(static_cast<unsigned char>(c1_raw));
  if (c0_raw == '_') {
    out.objType = 1;
    out.objectIndex = parse_two_char_index(name, 1);
    return out;
  }
  if (c0_raw == '~') {
    out.objType = 2;
    out.objectIndex = parse_two_char_index(name, 1);
    return out;
  }
  if (c0_raw == '#') {
    out.objType = 3;
    out.objectIndex = parse_two_char_index(name, 1);
    return out;
  }

  // Subobject member: leading digit at name[0] only.
  if (is_digit(c0_raw)) {
    out.objType = 4;
    out.objectIndex = parse_two_char_index(name, 0);
    return out;
  }

  // User points: U/P prefix in object list, with two-digit index at [3..4].
  if (c0 == 'U') {
    out.objType = 6;
    if (c1 == 'P') {
      out.userType = len > 2 ? name[2] : 0;
      out.objectIndex = parse_two_char_index(name, 3);
    }
    return out;
  }

  // Collision / volume classification.
  if (std::strchr("CBLDOV", c0) && c1 >= 'A' && c1 <= 'Z' &&
      !(c0 == 'B' && c1 == 'I' && len > 2 &&
        std::toupper(static_cast<unsigned char>(name[2])) == 'P') &&
      !(c0 == 'B' && c1 == 'N')) {
    out.objType = 5;
    size_t indexOffset = 2;
    switch (c0) {
      case 'C':
        switch (c1) {
          case 'B': out.collidableType = 1; break;
          case 'S': out.collidableType = 2; break;
          case 'C': out.collidableType = 3; break;
          case 'L': out.collidableType = 4; break;
          case 'V': out.collidableType = 5; break;
          case 'A': out.collidableType = 6; break;
          case 'D': out.collidableType = 9; break;
          case 'T': out.collidableType = 10; break;
          case 'M': out.collidableType = 11; break;
          case 'F': out.collidableType = 13; break;
          case 'P': out.collidableType = 19; break;
          default: break;
        }
        break;
      case 'D':
        if (c1 == 'H') out.collidableType = 16;
        else if (c1 == 'M') out.collidableType = 17;
        else if (c1 == 'L') out.collidableType = 18;
        break;
      case 'V':
        if (c1 == 'C') out.collidableType = 7;
        else if (c1 == 'K') out.collidableType = 12;
        break;
      case 'L':
        if (c1 == 'P') out.collidableType = 14;
        break;
      case 'O':
        if (c1 == 'B') out.collidableType = 20;
        else if (c1 == 'S') out.collidableType = 21;
        else if (c1 == 'P') out.collidableType = 22;
        else if (c1 == 'H') out.collidableType = 23;
        break;
      case 'B': {
        if (c1 == 'B') {
          out.collidableType = 8;
          out.bvolFlags = 0x3E;
          size_t i = 2;
          for (; i < len; ++i) {
            const char suffix = std::toupper(static_cast<unsigned char>(name[i]));
            if (suffix == 'L') out.bvolFlags &= ~0x10u;
            else if (suffix == 'O') out.bvolFlags &= ~0x20u;
            else if (suffix == 'S') out.bvolFlags &= ~0x4u;
            else if (suffix == 'V') out.bvolFlags &= ~0x2u;
            else if (suffix == 'W') out.bvolFlags &= ~0x8u;
            if (suffix >= '0' && suffix <= '9') break;
          }
          indexOffset = i;
        }
        break;
      }
      default:
        break;
    }
    out.objectIndex = parse_two_char_index(name, indexOffset);
    if (out.collidableType == 22) {
      if (indexOffset + 2 < len && name[indexOffset + 2] == '-') out.subIndex = parse_two_char_index(name, indexOffset + 3);
      else out.subIndex = 0;
    }
  }
  return out;
}

static inline Vec3 vec_add(const Vec3& a, const Vec3& b) {
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
static inline Vec3 vec_sub(const Vec3& a, const Vec3& b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
static inline Vec3 vec_scale(const Vec3& v, float s) {
  return Vec3{v.x * s, v.y * s, v.z * s};
}

// Match game engine's NormalizeVec3 (0x4215e0) x87 FPU behavior:
// sum computed at 80-bit (double covers this for float inputs),
// sqrt receives double, result truncated to float, 1.0/len at double
// truncated to float, then each component multiplied at double and stored float.
static inline void normalize_vec3(Vec3& v) {
  const double x = v.x, y = v.y, z = v.z;
  const double sum = x * x + y * y + z * z;
  const float len_f = static_cast<float>(std::sqrt(sum));
  if (len_f == 0.0f) {
    v.x = v.y = v.z = 0.0f;
    return;
  }
  const float inv = static_cast<float>(1.0 / static_cast<double>(len_f));
  v.x = static_cast<float>(static_cast<double>(inv) * x);
  v.y = static_cast<float>(static_cast<double>(inv) * y);
  v.z = static_cast<float>(static_cast<double>(inv) * z);
}

static float vec_length(const Vec3& v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// ---------------------------------------------------------------------------
// Material info table helpers (mirrors gMaterialInfoTable in the original).
// ---------------------------------------------------------------------------

// IDA: FindMaterialByFlags(key) with key[0]=5 (diffuse/secondary + skinned),
// key[1]=desired slot count (0..2), key[3]=skinned?1:0.
static int find_material_index_by_flags(int map_count, bool skinned) {
  const int desired = map_count < 0 ? 0 : (map_count > 2 ? 2 : map_count);
  const int count = static_cast<int>(std::size(kMaterialInfoTable));
  for (int i = 0; i < count; ++i) {
    const uint32_t flags = kMaterialInfoTable[i].flags;
    const bool table_skinned = (flags & MATERIAL_FLAG_SKINNED) != 0;
    if (skinned != table_skinned) continue;
    int present = (flags & MATERIAL_FLAG_DIFFUSE ? 1 : 0) +
                  (flags & MATERIAL_FLAG_SECONDARY ? 1 : 0);
    if (present != desired) continue;
    return i;
  }
  return 0;
}

// Try to extract a shader code from the material name (e.g. "Material_0_VS_PHONGT").
// Scans the name for a substring matching any entry in kMaterialInfoTable.
// Returns the table index, or -1 if no match.
static int try_parse_shader_from_name(const char* name) {
  if (!name || !*name) return -1;
  const std::string_view sv(name);
  const int count = static_cast<int>(std::size(kMaterialInfoTable));
  // Search longest names first to avoid partial matches (e.g. "VS_SKBUMPPHONGT"
  // should match before "VS_PHONGT"). We iterate all entries and track the
  // best (longest) match.
  int best_idx = -1;
  size_t best_len = 0;
  for (int i = 0; i < count; ++i) {
    const char* shader_name = kMaterialInfoTable[i].name;
    const size_t slen = std::strlen(shader_name);
    // Skip #UV variants ? they're UI toggles, not distinct shaders
    if (slen > 3 && std::strcmp(shader_name + slen - 3, "#UV") == 0) continue;
    if (slen > best_len && sv.find(shader_name) != std::string_view::npos) {
      best_idx = i;
      best_len = slen;
    }
  }
  return best_idx;
}

// ---------------------------------------------------------------------------
// Material table seeding from 3DP project definitions.
// ---------------------------------------------------------------------------

static void seed_generators_from_project(const project::Material& src, MaterialBucketSlot& dst) {
  // Mapfunc U.
  dst.gens.u_params.style = src.mapfunc_u_style;
  dst.gens.u_params.rate = src.mapfunc_u_rate;
  dst.gens.u_params.phase = src.mapfunc_u_phase;
  dst.gens.u_params.start = src.mapfunc_u_start;
  dst.gens.u_params.end = src.mapfunc_u_end;
  copy_str(dst.gens.u_params.ctrlReg, sizeof(dst.gens.u_params.ctrlReg), src.mapfunc_u_ctrlreg.c_str());
  // Mapfunc V.
  dst.gens.v_params.style = src.mapfunc_v_style;
  dst.gens.v_params.rate = src.mapfunc_v_rate;
  dst.gens.v_params.phase = src.mapfunc_v_phase;
  dst.gens.v_params.start = src.mapfunc_v_start;
  dst.gens.v_params.end = src.mapfunc_v_end;
  copy_str(dst.gens.v_params.ctrlReg, sizeof(dst.gens.v_params.ctrlReg), src.mapfunc_v_ctrlreg.c_str());
  // RGB generators.
  dst.gens.rgb_gen.style = src.rgbgen_style;
  dst.gens.rgb_gen.rate = src.rgbgen_rate;
  dst.gens.rgb_gen.phase = src.rgbgen_phase;
  dst.gens.rgb_gen.srgb.r = src.rgbgen_srgb[0];
  dst.gens.rgb_gen.srgb.g = src.rgbgen_srgb[1];
  dst.gens.rgb_gen.srgb.b = src.rgbgen_srgb[2];
  dst.gens.rgb_gen.ergb.r = src.rgbgen_ergb[0];
  dst.gens.rgb_gen.ergb.g = src.rgbgen_ergb[1];
  dst.gens.rgb_gen.ergb.b = src.rgbgen_ergb[2];
  copy_str(dst.gens.rgb_gen.ctrlReg, sizeof(dst.gens.rgb_gen.ctrlReg), src.rgbgen_ctrlreg.c_str());
  // Alpha generators.
  dst.gens.alpha_gen.style = src.alphagen_style;
  dst.gens.alpha_gen.rate = src.alphagen_rate;
  dst.gens.alpha_gen.phase = src.alphagen_phase;
  dst.gens.alpha_gen.start = src.alphagen_start;
  dst.gens.alpha_gen.end = src.alphagen_end;
  copy_str(dst.gens.alpha_gen.ctrlReg, sizeof(dst.gens.alpha_gen.ctrlReg), src.alphagen_ctrlreg.c_str());
}

static void seed_material_table_from_project(const project::Project* proj, MaterialTable& table) {
  if (!proj) return;
  const size_t count = std::min(proj->materials.size(), std::size(table.slots));
  table.count = static_cast<uint32_t>(count);
  for (size_t i = 0; i < count; ++i) {
    const auto& src = proj->materials[i];
    auto& dst = table.slots[i];
    std::memset(&dst, 0, sizeof(dst));
    dst.initialized = 1;
    copy_str(dst.name, sizeof(dst.name), src.name.c_str());
    copy_str(dst.shader_name, sizeof(dst.shader_name), src.shader_tag.c_str());
    dst.rattrib = static_cast<uint32_t>(src.rattrib);
    dst.pattrib = static_cast<uint32_t>(src.pattrib);
    dst.ptype = static_cast<uint32_t>(src.ptype);
    dst.geofx = static_cast<uint32_t>(src.geofx);
    dst.geofx_value = src.geofx_value;
    dst.alphatestvalue = static_cast<uint8_t>(src.alphatestvalue);
    // Base textures.
    copy_str(dst.tex1.path, sizeof(dst.tex1.path), src.diffuse_tex[0].c_str());
    copy_str(dst.tex2.path, sizeof(dst.tex2.path), src.diffuse_tex[1].c_str());
    dst.tex1.flags = src.diffuse_flags[0];
    dst.tex2.flags = src.diffuse_flags[1];
    copy_str(dst.anim_textures.normal[0].path, sizeof(dst.anim_textures.normal[0].path),
             src.normal_tex[0].c_str());
    copy_str(dst.anim_textures.normal[1].path, sizeof(dst.anim_textures.normal[1].path),
             src.normal_tex[1].c_str());
    dst.anim_textures.normal[0].flags = src.normal_flags[0];
    dst.anim_textures.normal[1].flags = src.normal_flags[1];
    for (int n = 0; n < 2; ++n) {
      if (dst.anim_textures.normal[n].path[0] == '\0') {
        dst.anim_textures.normal[n].path[0] = '0';
        dst.anim_textures.normal[n].path[1] = '\0';
      }
    }
    // Animation header/meta.
    dst.anim_meta.anim_frames = static_cast<uint32_t>(src.anim_frames);
    dst.anim_meta.anim_type = static_cast<uint32_t>(src.anim_type);
    dst.anim_meta.anim_frametime = static_cast<float>(src.anim_frametime);
    copy_str(dst.anim_meta.ctrl_reg, sizeof(dst.anim_meta.ctrl_reg), src.anim_ctrlreg.c_str());
    const int frame_limit = std::min(src.anim_frames, project::kMaxAnimFrames);
    for (int tex = 0; tex < 2; ++tex) {
      for (int f = 0; f < frame_limit; ++f) {
        dst.anim_textures.anim_diffuse[tex][f].flags = src.anim_diffuse[tex][f].enabled;
        dst.anim_textures.anim_normal[tex][f].flags = src.anim_normal[tex][f].enabled;
        copy_str(dst.anim_textures.anim_diffuse[tex][f].path,
                 sizeof(dst.anim_textures.anim_diffuse[tex][f].path),
                 src.anim_diffuse[tex][f].path.c_str());
        copy_str(dst.anim_textures.anim_normal[tex][f].path,
                 sizeof(dst.anim_textures.anim_normal[tex][f].path),
                 src.anim_normal[tex][f].path.c_str());
      }
    }
    dst.glass_reflect_hi = src.reflect_rgb[0];
    dst.glass_reflect_mid = src.reflect_rgb[1];
    dst.glass_reflect_lo = src.reflect_rgb[2];
    // UV defaults mirrored from serializer (tiling 1 on channel 0).
    dst.uv0_u_tiling = dst.uv0_v_tiling = 1.0f;
    dst.uv1_u_tiling = dst.uv1_v_tiling = 0.0f;
    seed_generators_from_project(src, dst);
  }
}

// Mirrors IsInStretchBox (0x4226E0): tests whether every midpoint of a vertex pair
// lies inside all face planes of the given ASE object (slight 0.02 tolerance).
static bool is_in_stretch_box(const ase::Object& obj) {
  if (obj.vert_count < 2 || obj.face_count == 0) return true;
  for (int i = 0; i < obj.vert_count - 1; ++i) {
    float x0 = obj.verts[i * 3 + 0];
    float y0 = obj.verts[i * 3 + 1];
    float z0 = obj.verts[i * 3 + 2];
    for (int j = i + 1; j < obj.vert_count; ++j) {
      float midx = (x0 + obj.verts[j * 3 + 0]) * 0.5f;
      float midy = (y0 + obj.verts[j * 3 + 1]) * 0.5f;
      float midz = (z0 + obj.verts[j * 3 + 2]) * 0.5f;
      for (int k = 0; k < obj.face_count; ++k) {
        const auto& f = obj.faces[k];
        float fx0 = obj.verts[f.vert[0] * 3 + 0];
        float fy0 = obj.verts[f.vert[0] * 3 + 1];
        float fz0 = obj.verts[f.vert[0] * 3 + 2];
        float fx1 = obj.verts[f.vert[1] * 3 + 0];
        float fy1 = obj.verts[f.vert[1] * 3 + 1];
        float fz1 = obj.verts[f.vert[1] * 3 + 2];
        float fx2 = obj.verts[f.vert[2] * 3 + 0];
        float fy2 = obj.verts[f.vert[2] * 3 + 1];
        float fz2 = obj.verts[f.vert[2] * 3 + 2];
        float e1x = fx1 - fx0;
        float e1y = fy1 - fy0;
        float e1z = fz1 - fz0;
        float e2x = fx2 - fx0;
        float e2y = fy2 - fy0;
        float e2z = fz2 - fz0;
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len >= 1e-5f) {
          const float invLen = 1.0f / len;
          nx *= invLen; ny *= invLen; nz *= invLen;
          float d = -(nx * fx0 + ny * fy0 + nz * fz0);
          float dist = midx * nx + midy * ny + midz * nz + d;
          if (dist > 0.02f) return false;
        }
      }
    }
  }
  return true;
}

// Mirror BuildTransformMatrix/NegateFacePlanes (0x421b80/0x4231a0).
// Uses double intermediates to match x87 FPU 80-bit precision; for float
// inputs, double captures the exact intermediate values that x87 would hold.
// The game engine computes each tangent/bitangent component via a per-axis
// cross product and divides (-y/x, -z/x) rather than using a shared inv_det.
static void compute_face_plane(float (&out)[3][3],
                               const Vec3& v0, const Vec3& v1, const Vec3& v2,
                               const Uv& uv0, const Uv& uv1, const Uv& uv2) {
  // Deltas stored as float (matching x87 fstp dword after each subtraction).
  const float du1 = uv1.uv[0] - uv0.uv[0];
  const float dv1 = uv1.uv[1] - uv0.uv[1];
  const float du2 = uv2.uv[0] - uv0.uv[0];
  const float dv2 = uv2.uv[1] - uv0.uv[1];
  const float dx1 = v1.x - v0.x;
  const float dy1 = v1.y - v0.y;
  const float dz1 = v1.z - v0.z;
  const float dx2 = v2.x - v0.x;
  const float dy2 = v2.y - v0.y;
  const float dz2 = v2.z - v0.z;

  // Game engine computes 3 cross products (one per position axis) and
  // extracts tangent/bitangent via -y/x, -z/x division on each.
  // The first component of each cross product is always det = du1*dv2-dv1*du2.
  // Cross product computed at x87 80-bit then stored to float (fstp dword).
  static float prev[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float v34 = prev[0], v35 = prev[1], v36 = prev[2];
  float v37 = prev[3], v38 = prev[4], v39 = prev[5];

  // X-axis cross product: (du1,dv1,dx1) x (du2,dv2,dx2)
  const float cp_x0 = static_cast<float>((double)du1 * dv2 - (double)dv1 * du2);  // det
  const float cp_x1 = static_cast<float>((double)dv1 * dx2 - (double)dx1 * dv2);
  const float cp_x2 = static_cast<float>((double)dx1 * du2 - (double)du1 * dx2);
  if (std::fabs(cp_x0) > 1e-9f) {
    v34 = static_cast<float>(-((double)cp_x1 / (double)cp_x0));
    v37 = static_cast<float>(-((double)cp_x2 / (double)cp_x0));
  }

  // Y-axis cross product
  const float cp_y0 = static_cast<float>((double)du1 * dv2 - (double)dv1 * du2);
  const float cp_y1 = static_cast<float>((double)dv1 * dy2 - (double)dy1 * dv2);
  const float cp_y2 = static_cast<float>((double)dy1 * du2 - (double)du1 * dy2);
  if (std::fabs(cp_y0) > 1e-9f) {
    v35 = static_cast<float>(-((double)cp_y1 / (double)cp_y0));
    v38 = static_cast<float>(-((double)cp_y2 / (double)cp_y0));
  }

  // Z-axis cross product
  const float cp_z0 = static_cast<float>((double)du1 * dv2 - (double)dv1 * du2);
  const float cp_z1 = static_cast<float>((double)dv1 * dz2 - (double)dz1 * dv2);
  const float cp_z2 = static_cast<float>((double)dz1 * du2 - (double)du1 * dz2);
  if (std::fabs(cp_z0) > 1e-9f) {
    v36 = static_cast<float>(-((double)cp_z1 / (double)cp_z0));
    v39 = static_cast<float>(-((double)cp_z2 / (double)cp_z0));
  }

  if (std::fabs(cp_x0) > 1e-9f) {
    prev[0] = v34; prev[1] = v35; prev[2] = v36;
    prev[3] = v37; prev[4] = v38; prev[5] = v39;
  }

  // Face normal: cross product of position edges, at double precision.
  Vec3 n;
  n.x = static_cast<float>((double)dy2 * dz1 - (double)dz2 * dy1);
  n.y = static_cast<float>((double)dz2 * dx1 - (double)dx2 * dz1);
  n.z = static_cast<float>((double)dx2 * dy1 - (double)dy2 * dx1);
  normalize_vec3(n);

  out[0][0] = -n.x;
  out[0][1] = -n.y;
  out[0][2] = -n.z;
  out[1][0] = -v34;
  out[1][1] = -v35;
  out[1][2] = -v36;
  out[2][0] = -v37;
  out[2][1] = -v38;
  out[2][2] = -v39;
}

static void update_minmax(Vec3& minv, Vec3& maxv, const Vec3& p) {
  minv.x = std::min(minv.x, p.x);
  minv.y = std::min(minv.y, p.y);
  minv.z = std::min(minv.z, p.z);
  maxv.x = std::max(maxv.x, p.x);
  maxv.y = std::max(maxv.y, p.y);
  maxv.z = std::max(maxv.z, p.z);
}

static void normalize_axis_row(float (&row)[3]) {
  float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
  if (len > 0.0f) {
    row[0] /= len;
    row[1] /= len;
    row[2] /= len;
  }
}

static void update_minmax(float (&minv)[3], float (&maxv)[3], const Vec3& p) {
  minv[0] = std::min(minv[0], p.x);
  minv[1] = std::min(minv[1], p.y);
  minv[2] = std::min(minv[2], p.z);
  maxv[0] = std::max(maxv[0], p.x);
  maxv[1] = std::max(maxv[1], p.y);
  maxv[2] = std::max(maxv[2], p.z);
}

static uint32_t bitcast_f32(float v) {
  uint32_t out;
  std::memcpy(&out, &v, sizeof(out));
  return out;
}

}  // namespace

void populate_part_anim_from_project(const project::Project* proj, LodBucketWorkspace& work, int lod_index) {
  std::memset(&work.part_anim, 0, sizeof(work.part_anim));
  // Default transform_as to identity mapping (transform_as[i] = i).
  // The original tool always maintains this default for slots not
  // explicitly overridden by 3DP part_anim data.
  for (int i = 0; i < work.lod.subobjectCount &&
                  i < static_cast<int>(std::size(work.part_anim.slots)); ++i) {
    work.part_anim.slots[i].transform_as = i;
  }
  if (!proj) return;
  const auto& lod = proj->lods[lod_index];
  work.part_anim.header.enable_part_anim = lod.part_anim_enabled ? 1u : 0u;
  const size_t max_slots = std::min({lod.part_anims.size(),
                                     static_cast<size_t>(work.lod.subobjectCount),
                                     std::size(work.part_anim.slots)});
  auto copy_axis = [](const project::AxisFunc& src, PartAnimFunc& dst) {
    dst.func = src.func_id;
    dst.param0 = src.param0;
    dst.param1 = src.param1;
    dst.param2 = src.param2;
    dst.param3 = src.param3;
    copy_str(dst.ctrlReg, sizeof(dst.ctrlReg), src.ctrl_reg.c_str());
  };
  for (size_t i = 0; i < max_slots; ++i) {
    const auto& src = lod.part_anims[i];
    auto& dst = work.part_anim.slots[i];
    dst.rotate_type = src.rotate_type;
    dst.scale_type = src.scale_type;
    dst.trans_type = src.trans_type;
    dst.transform_as = src.transform_as;
    dst.yaw_rate = src.yaw_rate;
    dst.pitch_rate = src.pitch_rate;
    dst.roll_rate = src.roll_rate;
    dst.reverse_rotate = src.reverse_rotate;
    copy_axis(src.yaw, dst.yaw_func);
    copy_axis(src.pitch, dst.pitch_func);
    copy_axis(src.roll, dst.roll_func);
    copy_axis(src.scale, dst.scale_func);
    copy_axis(src.scale_x, dst.scalex_func);
    copy_axis(src.scale_y, dst.scaley_func);
    copy_axis(src.scale_z, dst.scalez_func);
    copy_axis(src.trans_x, dst.transx_func);
    copy_axis(src.trans_y, dst.transy_func);
    copy_axis(src.trans_z, dst.transz_func);
  }
}

void apply_material_table_from_project(const project::Project* proj, MaterialTable& table) {
  seed_material_table_from_project(proj, table);
}

void apply_render_function_from_project(const project::Project* proj,
                                        LodHeader& lod,
                                        int lod_index) {
  if (!proj || lod_index < 0 ||
      lod_index >= static_cast<int>(proj->lods.size())) {
    return;
  }
  const auto& rf = proj->lods[static_cast<size_t>(lod_index)].render_function;
  if (rf.empty()) return;
  std::memset(lod.render_function, 0, sizeof(lod.render_function));
  copy_str(reinterpret_cast<char*>(lod.render_function), sizeof(lod.render_function),
           rf.c_str());
}

void apply_lights_from_project(const project::Project* proj,
                               LodHeader& lod,
                               int lod_index) {
  (void)lod_index;
  if (!proj || proj->lods.empty()) {
    return;
  }
  // LGHT is global in the 3DI format; source it from project LOD 0.
  const auto& plights = proj->lods[0].lights;
  const size_t count = std::min(plights.size(), static_cast<size_t>(lod.lightCount));
  for (size_t i = 0; i < count; ++i) {
    const auto& src = plights[i];
    Light& dst = lod.lights[i];
    dst.colorgen.style = src.colorgen_style;
    dst.colorgen.rate  = src.colorgen_rate;
    dst.colorgen.phase = src.colorgen_phase;
    dst.colorgen.start = Colori{src.colorgen_start[0], src.colorgen_start[1], src.colorgen_start[2]};
    dst.colorgen.end   = Colori{src.colorgen_end[0], src.colorgen_end[1], src.colorgen_end[2]};
    copy_str(dst.colorgen.ctrlReg, sizeof(dst.colorgen.ctrlReg), src.colorgen_ctrlreg.c_str());
    dst.disable_corona       = src.disable_corona;
    dst.disable_lightterrain = src.disable_lightterrain;
    dst.disable_lightobjects = src.disable_lightobjects;
  }
}

bool convert_to_internal(const ase::Document& doc,
                         MaterialTable& table,
                         LodBucketWorkspace& work,
                         const ConvertOptions& opts,
                         std::string* err) {
  // Clear any existing allocations.
  free_internal(work);
  std::memset(&table, 0, sizeof(table));
  seed_material_table_from_project(opts.project, table);

  bool hasError = false;
  bool hasWarning = false;
  const bool project_forces_skinned =
      opts.project &&
      opts.lod_index >= 0 &&
      opts.lod_index < static_cast<int>(opts.project->lods.size()) &&
      opts.project->lods[static_cast<size_t>(opts.lod_index)].attributes == 5;
  const bool doc_is_skinned = (doc.skinned_flags != 0);
  const bool export_is_skinned = doc_is_skinned || project_forces_skinned;

  // Skinned path in the original sets bit 2 (0x4) on the LOD flags up front.
  if (export_is_skinned) work.lod.flags |= 4;

  // Default render function signature "gnrc" lives in lod.render_function.
  const char kDefaultRenderFunc[] = "gnrc";
  std::memcpy(work.lod.render_function, kDefaultRenderFunc, sizeof(kDefaultRenderFunc) - 1);
  apply_render_function_from_project(opts.project, work.lod, opts.lod_index);

  // First pass: classify objects.
  std::vector<std::vector<int>> subobject_members;
  std::vector<uint8_t> subobject_has_mesh;
  std::vector<Vertex> center_seed_verts;
  std::vector<uint8_t> center_seed_valid;
  std::vector<int> attach_indices;
  std::unordered_map<std::string, int> name_to_node;
  int max_bone_node_id = -1;
  int attachCount = 0;
  int centerCount = 0;
  int stretchCount = 0;
  int collisionCount = 0;
  int userPointCount = 0;
  for (int i = 0; i < doc.object_count; ++i) {
    const auto& obj = doc.objects[i];
    name_to_node[obj.name] = obj.node_id;
    if (doc_is_skinned &&
        obj.node_id >= 0 &&
        obj.name &&
        !std::isdigit(static_cast<unsigned char>(obj.name[0]))) {
      max_bone_node_id = std::max(max_bone_node_id, obj.node_id);
    }
    Classification c = classify_name(obj.name);
    if (std::strlen(obj.name) > 30) hasError = true;
    switch (c.objType) {
      case 1: {
        centerCount++;
        if (c.objectIndex >= 0 && obj.vert_count > 0 && obj.verts) {
          const size_t idx = static_cast<size_t>(c.objectIndex);
          if (idx >= center_seed_verts.size()) {
            center_seed_verts.resize(idx + 1);
            center_seed_valid.resize(idx + 1, 0);
          }
          Vertex v{};
          v.pos.x = obj.verts[0] * opts.scale;
          v.pos.y = obj.verts[1] * opts.scale;
          v.pos.z = obj.verts[2] * opts.scale;
          center_seed_verts[idx] = v;
          center_seed_valid[idx] = 1;
        }
        break;
      }
      case 2: attach_indices.push_back(i); attachCount++; break;
      case 3: stretchCount++; break;
      case 4: {
        if (!doc_is_skinned) {
          int idx = c.objectIndex >= 0 ? c.objectIndex : 0;
          if (idx >= static_cast<int>(subobject_members.size())) subobject_members.resize(idx + 1);
          subobject_members[idx].push_back(i);
          if (subobject_members[idx].size() >= 1000) hasError = true;
        }
        break;
      }
      case 5: collisionCount++; break;
      case 6: userPointCount++; break;
      default: break;
    }

    // Skinned path: include non-leading-digit bone helpers via node_id+1.
    if (doc_is_skinned) {
      int subIdx = -1;
      if (obj.name && std::isdigit(static_cast<unsigned char>(obj.name[0]))) {
        subIdx = parse_two_char_index(obj.name, 0);
        const bool is_weighted_mesh =
            obj.skinned && obj.face_count > 0 && obj.vert_count > 0;
        if (is_weighted_mesh && max_bone_node_id >= 0) {
          // Importer-generated skinned ASEs can place the weighted mesh at a
          // low digit index (e.g. "01 Mesh0"), while bone helpers occupy the
          // full node-id range. Anchor the weighted payload at the terminal
          // bone subobject to match legacy OED indexing.
          subIdx = std::max(subIdx, max_bone_node_id);
        }
      } else if (obj.node_id >= 0) {
        subIdx = obj.node_id;
      }
      if (subIdx >= 0) {
        if (subIdx >= static_cast<int>(subobject_members.size())) subobject_members.resize(subIdx + 1);
        subobject_members[subIdx].push_back(i);
        if (subobject_members[subIdx].size() >= 1000) hasError = true;
      }
    }
  }

  LodHeader& lod = work.lod;
  lod.subobjectCount = static_cast<uint32_t>(subobject_members.size());
  centerCount = std::max(centerCount, static_cast<int>(lod.subobjectCount));
  if (doc_is_skinned) attachCount = std::max(attachCount, static_cast<int>(lod.subobjectCount));
  const uint32_t attachCapacity = static_cast<uint32_t>(attachCount);
  const uint32_t centerCapacity = static_cast<uint32_t>(centerCount);
  const uint32_t collisionCapacity = static_cast<uint32_t>(collisionCount);
  const uint32_t userPointCapacity = static_cast<uint32_t>(userPointCount);
  lod.attachCount = attachCapacity;
  lod.centerCount = centerCapacity;
  lod.collisionCount = collisionCapacity;
  lod.userPointCount = userPointCapacity;
  lod.lightCount = static_cast<uint32_t>(doc.light_count);
  lod.materialCount = static_cast<uint32_t>(doc.material_count);
  lod.totalCollPlanes = 0;

  // Allocate arrays.
  if (lod.subobjectCount) lod.subobjects = new SubObject[lod.subobjectCount]{};
  if (lod.attachCount) lod.attachPoints = new Point92[lod.attachCount]{};
  if (lod.centerCount) lod.centerPoints = new Point92[lod.centerCount]{};
  if (lod.collisionCount) lod.collisions = new Collision[lod.collisionCount]{};
  if (lod.userPointCount) lod.userPoints = new UserPoint[lod.userPointCount]{};
  if (lod.lightCount) lod.lights = new Light[lod.lightCount]{};
  if (lod.materialCount) lod.materials = new Material[lod.materialCount]{};

  // Copy materials.
  for (uint32_t i = 0; i < lod.materialCount; ++i) {
    const auto& src = doc.materials[i];
    auto& dst = lod.materials[i];
    dst.flags = src.flags;
    copy_str(dst.name, sizeof(dst.name), src.name);
    for (int m = 0; m < 4; ++m) copy_str(dst.maps[m], sizeof(dst.maps[m]), src.maps[m]);
    std::memcpy(dst.uv_u_offset, src.uv_u_offset, sizeof(dst.uv_u_offset));
    std::memcpy(dst.uv_v_offset, src.uv_v_offset, sizeof(dst.uv_v_offset));
    std::memcpy(dst.uv_u_tiling, src.uv_u_tiling, sizeof(dst.uv_u_tiling));
    std::memcpy(dst.uv_v_tiling, src.uv_v_tiling, sizeof(dst.uv_v_tiling));
    dst.extra_flags = src.extra_flags;
  }

  populate_part_anim_from_project(opts.project, work, opts.lod_index);
  apply_lights_from_project(opts.project, work.lod, opts.lod_index);

  // Prepare stretch boxes.
  std::vector<StretchBox> stretchBoxes;
  stretchBoxes.reserve(stretchCount);
  for (int i = 0; i < doc.object_count; ++i) {
    const auto& obj = doc.objects[i];
    Classification c = classify_name(obj.name);
    if (c.objType != 3) continue;
    if (obj.vert_count <= 0 || !obj.verts) continue;
    StretchBox box{};
    box.subObjA = c.objectIndex >= 0 ? c.objectIndex : 0;
    box.subObjB = box.subObjA;
    if (obj.name && obj.name[3] == '#') {
      box.subObjB = parse_two_char_index(obj.name, 4);
    }
    // Use bbox of verts as the stretch box.
    Vec3 minv{9999999.0f, 9999999.0f, 9999999.0f};
    Vec3 maxv{-9999999.0f, -9999999.0f, -9999999.0f};
    for (int v = 0; v < obj.vert_count; ++v) {
      Vec3 p{obj.verts[v * 3 + 0] * opts.scale,
             obj.verts[v * 3 + 1] * opts.scale,
             obj.verts[v * 3 + 2] * opts.scale};
      update_minmax(minv, maxv, p);
    }
    box.min = minv;
    box.max = maxv;
    stretchBoxes.push_back(box);
  }

  // Aggregate subobjects.
  uint32_t globalVertTotal = 0;
  uint32_t globalUvTotal = 0;
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    auto& sub = lod.subobjects[s];
    {
      char tmp[16]{};
      std::snprintf(tmp, sizeof(tmp), "%02u ?????", s + 1);
      copy_str(sub.name, sizeof(sub.name), tmp);
    }
    const auto& members = subobject_members[s];
    auto is_bone_helper = [&](const ase::Object& obj) {
      return doc_is_skinned &&
             obj.node_id >= 0 &&
             (!obj.name || !std::isdigit(static_cast<unsigned char>(obj.name[0])));
    };
    auto effective_member_vert_count = [&](const ase::Object& obj) -> uint32_t {
      return obj.vert_count > 0 ? static_cast<uint32_t>(obj.vert_count) : 0u;
    };
    for (int idx : members) {
      const auto& obj = doc.objects[idx];
      if (is_bone_helper(obj)) continue;
      const uint32_t member_vert_count = effective_member_vert_count(obj);
      sub.vertCount += member_vert_count;
      sub.uvCount += static_cast<uint32_t>(obj.uv_count);
      sub.faceCount += static_cast<int32_t>(obj.face_count);
      if (obj.color_face_count > 0) sub.colorCount += static_cast<uint32_t>(obj.face_count);
      if (member_vert_count > 0) {
        if (s >= subobject_has_mesh.size()) subobject_has_mesh.resize(s + 1, 0);
        subobject_has_mesh[s] = 1;
      }
    }
    if (sub.faceCount < 0) sub.faceCount = 0;
    if (sub.colorCount && sub.colorCount < static_cast<uint32_t>(sub.faceCount)) {
      sub.colorCount = static_cast<uint32_t>(sub.faceCount);
    }
    if (sub.vertCount) sub.verts = new Vertex[sub.vertCount]{};
    if (sub.uvCount) sub.uvs = new Uv[sub.uvCount]{};
    if (sub.faceCount) sub.faces = new Face[sub.faceCount]{};
    if (sub.colorCount) sub.colors = new FaceColor[sub.colorCount]{};
    sub.preSmoothedNormals = nullptr;
    // Check if all members provide pre-computed face normals.
    bool all_have_normals = !members.empty();
    for (int idx : members) {
      const auto& obj = doc.objects[idx];
      if (is_bone_helper(obj)) continue;
      if (obj.face_normal_count != obj.face_count || !obj.face_normals) {
        all_have_normals = false;
        break;
      }
    }
    if (all_have_normals && sub.faceCount > 0) {
      sub.preSmoothedNormals = new float[static_cast<size_t>(sub.faceCount) * 9]{};
    }
    if (!members.empty()) {
      copy_str(sub.name, sizeof(sub.name), doc.objects[members.front()].name);
    }

    uint32_t vertBase = 0;
    uint32_t uvBase = 0;
    uint32_t faceBase = 0;
    for (int idx : members) {
      const auto& obj = doc.objects[idx];
      if (is_bone_helper(obj)) continue;
      const uint32_t member_vert_count = effective_member_vert_count(obj);
      // Copy verts.
      if (obj.vert_count > 0) {
        for (int v = 0; v < obj.vert_count; ++v) {
          const float* src = &obj.verts[v * 3];
          Vertex& dst = sub.verts[vertBase + v];
          dst.pos.x = src[0] * opts.scale;
          dst.pos.y = src[1] * opts.scale;
          dst.pos.z = src[2] * opts.scale;
          if (doc_is_skinned) {
            if (obj.skinned && obj.weight_count > v && obj.weights) {
              for (int m = 0; m < 4; ++m) {
                dst.boneIndex[m] = obj.weights[v].bone_index[m];
                dst.boneWeight[m] = obj.weights[v].weight[m];
                if (dst.boneIndex[m] == -1) {
                  dst.boneIndex[m] = dst.boneIndex[0];
                  dst.boneWeight[m] = 0.0f;
                }
              }
            } else {
              for (int n = 0; n < 4; ++n) {
                dst.boneIndex[n] = static_cast<int32_t>(s);
                dst.boneWeight[n] = (n == 0) ? 1.0f : 0.0f;
              }
            }
          } else {
            dst.boneIndex[0] = dst.boneIndex[1] = static_cast<int32_t>(s);
            dst.boneIndex[2] = dst.boneIndex[3] = 0;
            dst.boneWeight[0] = dst.boneWeight[1] = dst.boneWeight[2] = dst.boneWeight[3] = 0.0f;
          }
          dst.flags = 0;
        }
      }
      // Copy UVs (flip V).
      for (int t = 0; t < obj.uv_count; ++t) {
        const auto& src = obj.uvs[t];
        Uv& dst = sub.uvs[uvBase + t];
        dst.uv[0] = src.u;
        dst.uv[1] = 1.0f - src.v;
      }
      // Faces.
      for (int f = 0; f < obj.face_count; ++f) {
        const auto& src = obj.faces[f];
        Face& dst = sub.faces[faceBase + f];
        dst.srcMatId = src.material_index;
        dst.smoothingGroup = static_cast<int32_t>(src.smoothing_mask);
        for (int k = 0; k < 4; ++k) dst.vert[k] = static_cast<uint32_t>(src.vert[k] + vertBase);
        for (int k = 0; k < 4; ++k) dst.uv[k] = static_cast<uint32_t>(src.uv[k] + uvBase);
        dst.edge_visibility[0] = src.edge_visibility[0];
        dst.edge_visibility[1] = src.edge_visibility[1];
        dst.edge_visibility[2] = src.edge_visibility[2];
        dst.edge_visibility[3] = src.edge_visibility[3];
        dst.srcObjIndex = static_cast<uint32_t>(idx);
        dst.hasDollar = std::strchr(obj.name, '$') ? 1 : 0;
      }
      // Copy pre-computed face normals if available.
      if (sub.preSmoothedNormals && obj.face_normals && obj.face_normal_count == obj.face_count) {
        std::memcpy(&sub.preSmoothedNormals[faceBase * 9],
                     obj.face_normals,
                     static_cast<size_t>(obj.face_count) * 9 * sizeof(float));
      }
      // Colors: sentinel if none.
      if (sub.colorCount) {
        for (int f = 0; f < obj.face_count; ++f) {
          uint32_t ci = faceBase + f;
          if (obj.color_face_count > f && obj.colors) {
            sub.colors[ci].rgb.r = static_cast<int>(obj.colors[obj.faces[f].color[0]]);
            sub.colors[ci].rgb.g = static_cast<int>(obj.colors[obj.faces[f].color[1]]);
            sub.colors[ci].rgb.b = static_cast<int>(obj.colors[obj.faces[f].color[2]]);
          } else {
            sub.colors[ci].rgb.r = 0x80000000;
            sub.colors[ci].rgb.g = 0;
            sub.colors[ci].rgb.b = 0;
          }
        }
      }
      vertBase += member_vert_count;
      uvBase += static_cast<uint32_t>(obj.uv_count);
      faceBase += static_cast<uint32_t>(obj.face_count);
    }
    globalVertTotal += sub.vertCount;
    globalUvTotal += sub.uvCount;
  }

  // Centers keyed by subobject index; mirror IDA's clamp to subobjectCount.
  lod.centerCount = lod.subobjectCount;

  // Attach points: average position of verts, ordered case-insensitive by name like the original.
  if (!doc_is_skinned) {
    lod.attachCount = 0;
    if (!attach_indices.empty()) {
      std::stable_sort(attach_indices.begin(), attach_indices.end(),
                       [&](int lhs, int rhs) {
                         return icase_cmp(doc.objects[lhs].name, doc.objects[rhs].name) < 0;
                       });
      for (int idx : attach_indices) {
        if (lod.attachCount >= attachCapacity) break;
        const auto& obj = doc.objects[idx];
        auto& dst = lod.attachPoints[lod.attachCount++];
        copy_str(dst.name, sizeof(dst.name), obj.name);
        double ax = 0.0, ay = 0.0, az = 0.0;
        for (int v = 0; v < obj.vert_count; ++v) {
          ax += static_cast<double>(obj.verts[v * 3 + 0]) * opts.scale;
          ay += static_cast<double>(obj.verts[v * 3 + 1]) * opts.scale;
          az += static_cast<double>(obj.verts[v * 3 + 2]) * opts.scale;
        }
        Vec3 acc{};
        acc.x = static_cast<float>(ax / static_cast<double>(obj.vert_count));
        acc.y = static_cast<float>(ay / static_cast<double>(obj.vert_count));
        acc.z = static_cast<float>(az / static_cast<double>(obj.vert_count));
        std::memcpy(dst.pos, &acc.x, sizeof(dst.pos));
      }
    }
  }

  lod.userPointCount = 0;
  for (int i = 0; i < doc.object_count; ++i) {
    const auto& obj = doc.objects[i];
    Classification c = classify_name(obj.name);
    if (c.objType == 2) continue;
    if (c.objType == 1) {
      int parsed = parse_two_char_atol(obj.name, 1);
      uint32_t idx = parsed > 0 ? static_cast<uint32_t>(parsed - 1) : 0;
      if (idx >= lod.centerCount) {
        hasWarning = true;
        continue;
      }
      auto& dst = lod.centerPoints[idx];
      copy_str(dst.name, sizeof(dst.name), obj.name);
      // Original x87 accumulates at 80-bit extended precision; use double
      // to approximate, then truncate to float for the final result.
      double ax = 0.0, ay = 0.0, az = 0.0;
      for (int v = 0; v < obj.vert_count; ++v) {
        ax += static_cast<double>(obj.verts[v * 3 + 0]) * opts.scale;
        ay += static_cast<double>(obj.verts[v * 3 + 1]) * opts.scale;
        az += static_cast<double>(obj.verts[v * 3 + 2]) * opts.scale;
      }
      Vec3 acc{};
      acc.x = static_cast<float>(ax / static_cast<double>(obj.vert_count));
      acc.y = static_cast<float>(ay / static_cast<double>(obj.vert_count));
      acc.z = static_cast<float>(az / static_cast<double>(obj.vert_count));
      std::memcpy(dst.pos, &acc.x, sizeof(dst.pos));
      for (int r = 0; r < 3; ++r) {
        for (int cidx = 0; cidx < 3; ++cidx) {
          dst.axis[r][cidx] = obj.tm_row[r][cidx];
        }
        normalize_axis_row(dst.axis[r]);
      }
      for (int cidx = 0; cidx < 3; ++cidx) dst.axis[3][cidx] = obj.tm_row[3][cidx];
    } else if (c.objType == 6) {
      const char up0 = std::toupper(static_cast<unsigned char>(obj.name[0]));
      const char up1 = std::toupper(static_cast<unsigned char>(obj.name[1]));
      if (up0 != 'U' || up1 != 'P') continue;
      if (lod.userPointCount >= userPointCapacity) continue;
      auto& dst = lod.userPoints[lod.userPointCount++];
      // Name after first space.
      const char* space = std::strchr(obj.name, ' ');
      const char* upName = (space && *(space + 1)) ? space + 1 : "Noname";
      copy_str(dst.name, sizeof(dst.name), upName);
      double ax = 0.0, ay = 0.0, az = 0.0;
      for (int v = 0; v < obj.vert_count; ++v) {
        ax += static_cast<double>(obj.verts[v * 3 + 0]) * opts.scale;
        ay += static_cast<double>(obj.verts[v * 3 + 1]) * opts.scale;
        az += static_cast<double>(obj.verts[v * 3 + 2]) * opts.scale;
      }
      Vec3 acc{};
      acc.x = static_cast<float>(ax / static_cast<double>(obj.vert_count));
      acc.y = static_cast<float>(ay / static_cast<double>(obj.vert_count));
      acc.z = static_cast<float>(az / static_cast<double>(obj.vert_count));
      // USRP positions are stored in internal/swizzled space (same as
      // vertices and center points), NOT engine space.  No cyclic shift.
      std::memcpy(dst.pos, &acc.x, sizeof(dst.pos));
      dst.subObj = parse_two_char_index(obj.name, 3);
      char userType = obj.name[2];
      if (userType >= 'a' && userType <= 'z') userType -= 32;
      dst.type = static_cast<uint32_t>(static_cast<unsigned char>(userType));
      for (int r = 0; r < 3; ++r) {
        for (int cidx = 0; cidx < 3; ++cidx) {
          dst.axis[r][cidx] = obj.tm_row[r][cidx];
        }
        normalize_axis_row(dst.axis[r]);
      }
      for (int cidx = 0; cidx < 3; ++cidx) dst.axis[3][cidx] = obj.tm_row[3][cidx];
    }
  }

  // Preserve ASE object order for user points (matches fixture/reference order).

  // For skinned docs, mirror ConvertToInternalSkinned behavior: assign attachIndex from
  // the parent name of the first member, then write attach/center points from either a
  // digit-named object or any object with node_id >= 0 (IsAttachOrBoned).
  if (doc_is_skinned) {
    auto set_identity_axis = [](Point92& p) {
      for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) p.axis[r][c] = 0.0f;
      p.axis[0][0] = p.axis[1][1] = p.axis[2][2] = 1.0f;
    };
    // Attach index: match parent_name of the first member (case-insensitive).
    for (uint32_t s = 1; s < lod.subobjectCount; ++s) {
      if (subobject_members[s].empty()) continue;
      const auto& first_obj = doc.objects[subobject_members[s].front()];
      int attachIdx = 0;
      for (uint32_t j = 0; j < lod.subobjectCount; ++j) {
        if (icase_cmp(lod.subobjects[j].name, first_obj.parent_name) == 0) {
          attachIdx = static_cast<int>(j);
          break;
        }
      }
      lod.subobjects[s].attachIndex = attachIdx;
    }

    // Attach/center points from IsAttachOrBoned (leading digit or node_id >= 0).
    for (int i = 0; i < doc.object_count; ++i) {
      const auto& obj = doc.objects[i];
      int node_id = -1;
      const char* name = obj.name;
      if (name && std::isdigit(static_cast<unsigned char>(name[0]))) {
        node_id = parse_two_char_index(name, 0);
      } else if (obj.node_id >= 0) {
        node_id = obj.node_id;
      }
      if (node_id < 0 || node_id >= static_cast<int>(lod.attachCount)) continue;
      auto& ap = lod.attachPoints[static_cast<uint32_t>(node_id)];
      copy_str(ap.name, sizeof(ap.name), obj.name);
      ap.pos[0] = obj.tm_row[3][0] * opts.scale;
      ap.pos[1] = obj.tm_row[3][1] * opts.scale;
      ap.pos[2] = obj.tm_row[3][2] * opts.scale;
      set_identity_axis(ap);
      if (static_cast<uint32_t>(node_id) < lod.centerCount && !lod.centerPoints[node_id].name[0]) {
        std::memcpy(&lod.centerPoints[node_id], &ap, sizeof(Point92));
      }
    }
  }

  // Collisions.
  lod.collisionCount = 0;
  // Seed with the uninitialized stack value observed in hook dumps; carried across classifications.
  uint32_t lastSubIndex = 29945296;
  for (int i = 0; i < doc.object_count; ++i) {
    const auto& obj = doc.objects[i];
    Classification c = classify_name(obj.name, lastSubIndex);
    lastSubIndex = static_cast<uint32_t>(c.subIndex);
    if (c.objType != 5 || lod.collisionCount >= collisionCapacity) continue;
    auto& dst = lod.collisions[lod.collisionCount++];
    copy_str(dst.name, sizeof(dst.name), obj.name);
    dst.bvolFlags = c.bvolFlags;
    dst.collidableType = c.collidableType;
    dst.objectIndex = c.objectIndex >= 0 ? static_cast<uint32_t>(c.objectIndex) : 0;
    dst.sub_index = static_cast<uint32_t>(c.subIndex);

    const int vcount = obj.vert_count;
    const int fcount = obj.face_count;
    std::vector<uint32_t> facePlane(static_cast<size_t>(fcount), 0);

    // Original x87 accumulates at 80-bit; use double to approximate.
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (int v = 0; v < vcount; ++v) {
      cx += static_cast<double>(obj.verts[v * 3 + 0]) * opts.scale;
      cy += static_cast<double>(obj.verts[v * 3 + 1]) * opts.scale;
      cz += static_cast<double>(obj.verts[v * 3 + 2]) * opts.scale;
    }
    const float invCount = 1.0f / static_cast<float>(vcount);
    Vec3 center{};
    center.x = static_cast<float>(cx / static_cast<double>(vcount));
    center.y = static_cast<float>(cy / static_cast<double>(vcount));
    center.z = static_cast<float>(cz / static_cast<double>(vcount));
    dst.center = center;

    float radius3d = 0.0f;
    float radiusXY = 0.0f;
    for (int v = 0; v < vcount; ++v) {
      Vec3 p{obj.verts[v * 3 + 0] * opts.scale,
             obj.verts[v * 3 + 1] * opts.scale,
             obj.verts[v * 3 + 2] * opts.scale};
      Vec3 d = vec_sub(p, center);
      radius3d = std::max(radius3d, vec_length(d));
      radiusXY = std::max(radiusXY, std::sqrt(d.x * d.x + d.y * d.y));
    }
    dst.radius = radius3d;
    dst.radiusXY = radiusXY;

    float bboxMin[3] = {center.x, center.y, center.z};
    float bboxMax[3] = {center.x, center.y, center.z};
    for (int v = 0; v < vcount; ++v) {
      Vec3 p{obj.verts[v * 3 + 0] * opts.scale,
             obj.verts[v * 3 + 1] * opts.scale,
             obj.verts[v * 3 + 2] * opts.scale};
      update_minmax(bboxMin, bboxMax, p);
    }
    dst.bbox.minX = bboxMin[0]; dst.bbox.maxX = bboxMax[0];
    dst.bbox.minY = bboxMin[1]; dst.bbox.maxY = bboxMax[1];
    dst.bbox.minZ = bboxMin[2]; dst.bbox.maxZ = bboxMax[2];
    dst.halfExtents[0] = (dst.bbox.maxX - dst.bbox.minX) * 0.5f;
    dst.halfExtents[1] = (dst.bbox.maxY - dst.bbox.minY) * 0.5f;
    dst.halfExtents[2] = (dst.bbox.maxZ - dst.bbox.minZ) * 0.5f;
    dst.height = dst.bbox.maxZ - dst.bbox.minZ;
    dst.unk54 = 0;
    if (dst.collidableType == 3) dst.center.z = dst.bbox.minZ;

    std::array<CollisionPlane, 32> planes{};
    planes[0] = {Vec3{1.f, 0.f, 0.f}, -dst.bbox.maxX, 0u};
    planes[1] = {Vec3{-1.f, 0.f, 0.f}, dst.bbox.minX, 0u};
    planes[2] = {Vec3{0.f, 1.f, 0.f}, -dst.bbox.maxY, 0u};
    planes[3] = {Vec3{0.f, -1.f, 0.f}, dst.bbox.minY, 0u};
    planes[4] = {Vec3{0.f, 0.f, 1.f}, -dst.bbox.maxZ, 0u};
    planes[5] = {Vec3{0.f, 0.f, -1.f}, dst.bbox.minZ, 0u};
    uint32_t planeCount = 6;
    uint32_t lastPlaneIndex = 0;

    for (int f = 0; f < fcount; ++f) {
      const auto& srcFace = obj.faces[f];
      const float x0 = obj.verts[srcFace.vert[0] * 3 + 0];
      const float y0 = obj.verts[srcFace.vert[0] * 3 + 1];
      const float z0 = obj.verts[srcFace.vert[0] * 3 + 2];
      const float x1 = obj.verts[srcFace.vert[1] * 3 + 0];
      const float y1 = obj.verts[srcFace.vert[1] * 3 + 1];
      const float z1 = obj.verts[srcFace.vert[1] * 3 + 2];
      const float x2 = obj.verts[srcFace.vert[2] * 3 + 0];
      const float y2 = obj.verts[srcFace.vert[2] * 3 + 1];
      const float z2 = obj.verts[srcFace.vert[2] * 3 + 2];

      const float e1x = x1 - x0;
      const float e1y = y1 - y0;
      const float e1z = z1 - z0;
      const float e2x = x2 - x0;
      const float e2y = y2 - y0;
      const float e2z = z2 - z0;

      float nx = e1y * e2z - e1z * e2y;
      float ny = e1z * e2x - e1x * e2z;
      float nz = e1x * e2y - e1y * e2x;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      uint32_t planeIdx = 0;
      if (len > 0.0001f) {
        const float invLen = 1.0f / len;
        nx *= invLen; ny *= invLen; nz *= invLen;
        float d = -(nx * x0 + ny * y0 + nz * z0);
        d *= opts.scale;
        bool matched = false;
        for (uint32_t k = 0; k < planeCount && k < planes.size(); ++k) {
          if (std::fabs(planes[k].n.x - nx) <= 0.005f &&
              std::fabs(planes[k].n.y - ny) <= 0.005f &&
              std::fabs(planes[k].n.z - nz) <= 0.005f &&
              std::fabs(planes[k].d - d) <= 0.03f) {
            planeIdx = k;
            lastPlaneIndex = k;
            matched = true;
          }
        }
        if (!matched) {
          if (planeCount < planes.size()) {
            planes[planeCount].n.x = nx;
            planes[planeCount].n.y = ny;
            planes[planeCount].n.z = nz;
            planes[planeCount].d = d;
            planes[planeCount].flags = 0;
            planeIdx = planeCount;
            lastPlaneIndex = planeCount;
            ++planeCount;
          } else {
            hasError = true;
          }
        }
      }
      facePlane[static_cast<size_t>(f)] = planeIdx;
    }

    dst.planeCount = std::min<uint32_t>(planeCount, planes.size());
    if (dst.collidableType == 4 && lastPlaneIndex < dst.planeCount) {
      std::swap(planes[0], planes[lastPlaneIndex]);
    }
    for (uint32_t pidx = 0; pidx < dst.planeCount; ++pidx) {
      dst.planes[pidx] = planes[pidx];
    }

    dst.vertCount = static_cast<uint32_t>(vcount);
    if (dst.vertCount) dst.verts = new Vertex[dst.vertCount]{};
    for (uint32_t v = 0; v < dst.vertCount; ++v) {
      dst.verts[v].pos.x = obj.verts[v * 3 + 0] * opts.scale;
      dst.verts[v].pos.y = obj.verts[v * 3 + 1] * opts.scale;
      dst.verts[v].pos.z = obj.verts[v * 3 + 2] * opts.scale;
      dst.verts[v].boneIndex[0] = 0;
      dst.verts[v].boneIndex[1] = 0;
    }

    dst.faceCount = static_cast<uint32_t>(fcount);
    if (dst.faceCount) dst.faces = new Face[dst.faceCount]{};
    for (uint32_t f = 0; f < dst.faceCount; ++f) {
      const auto& src = obj.faces[f];
      auto& face = dst.faces[f];
      for (int k = 0; k < 4; ++k) face.vert[k] = static_cast<uint32_t>(src.vert[k]);
      for (int k = 0; k < 4; ++k) face.uv[k] = 0;
      face.edge_visibility[0] = src.edge_visibility[0];
      face.edge_visibility[1] = src.edge_visibility[1];
      face.edge_visibility[2] = src.edge_visibility[2];
      face.edge_visibility[3] = src.edge_visibility[3];
      face.planeIndex = facePlane[f];
    }
  }

  // Match IDA's late total plane count accumulation and collision sort.
  lod.totalCollPlanes = 0;
  std::stable_sort(lod.collisions, lod.collisions + lod.collisionCount,
                   [](const Collision& a, const Collision& b) { return a.objectIndex < b.objectIndex; });
  for (uint32_t i = 0; i < lod.collisionCount; ++i) {
    lod.totalCollPlanes += lod.collisions[i].planeCount;
  }

  // Lights: copy raw then recenter later.
  for (int i = 0; i < doc.light_count; ++i) {
    const auto& src = doc.lights[i];
    auto& dst = lod.lights[i];
    copy_str(dst.name, sizeof(dst.name), src.name);
    dst.type = src.type;
    dst.x = src.pos[0] * opts.scale;
    dst.y = src.pos[1] * opts.scale;
    dst.z = src.pos[2] * opts.scale;
    dst.color = Color{src.color[0], src.color[1], src.color[2]};
    dst.intensity = src.intensity;
    dst.near_atten_start = src.near_atten_start;
    dst.atten_end = src.atten_end;
    dst.subObj = static_cast<int32_t>(std::strtol(src.name + 2, nullptr, 10) - 1);
    dst.hotspot = src.hotspot;
    dst.falloff = src.falloff;
    dst.rotX = src.tm_row2[0];
    dst.rotY = src.tm_row2[1];
    dst.rotZ = src.tm_row2[2];
  }

  // Recenter around the first center point.
  Vec3 centerOffset{};
  if (lod.centerCount > 0) {
    centerOffset.x = lod.centerPoints[0].pos[0];
    centerOffset.y = lod.centerPoints[0].pos[1];
    centerOffset.z = lod.centerPoints[0].pos[2];
  }
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    auto& sub = lod.subobjects[s];
    for (uint32_t v = 0; v < sub.vertCount; ++v) {
      sub.verts[v].pos = vec_sub(sub.verts[v].pos, centerOffset);
    }
  }
  for (uint32_t i = 0; i < lod.attachCount; ++i) {
    lod.attachPoints[i].pos[0] -= centerOffset.x;
    lod.attachPoints[i].pos[1] -= centerOffset.y;
    lod.attachPoints[i].pos[2] -= centerOffset.z;
  }
  for (uint32_t i = 0; i < lod.centerCount; ++i) {
    lod.centerPoints[i].pos[0] -= centerOffset.x;
    lod.centerPoints[i].pos[1] -= centerOffset.y;
    lod.centerPoints[i].pos[2] -= centerOffset.z;
  }
  for (uint32_t i = 0; i < lod.userPointCount; ++i) {
    lod.userPoints[i].pos[0] -= centerOffset.x;
    lod.userPoints[i].pos[1] -= centerOffset.y;
    lod.userPoints[i].pos[2] -= centerOffset.z;
  }
  for (uint32_t i = 0; i < lod.lightCount; ++i) {
    lod.lights[i].x -= centerOffset.x;
    lod.lights[i].y -= centerOffset.y;
    lod.lights[i].z -= centerOffset.z;
  }
  for (auto& box : stretchBoxes) {
    box.min = vec_sub(box.min, centerOffset);
    box.max = vec_sub(box.max, centerOffset);
  }
  for (uint32_t cidx = 0; cidx < lod.collisionCount; ++cidx) {
    auto& c = lod.collisions[cidx];
    c.center = vec_sub(c.center, centerOffset);
    c.bbox.minX -= centerOffset.x; c.bbox.maxX -= centerOffset.x;
    c.bbox.minY -= centerOffset.y; c.bbox.maxY -= centerOffset.y;
    c.bbox.minZ -= centerOffset.z; c.bbox.maxZ -= centerOffset.z;
    for (uint32_t p = 0; p < c.planeCount && p < 32; ++p) {
      float delta = c.planes[p].n.x * centerOffset.x +
                    c.planes[p].n.y * centerOffset.y +
                    c.planes[p].n.z * centerOffset.z;
      c.planes[p].d += delta;
    }
    for (uint32_t v = 0; v < c.vertCount; ++v) {
      c.verts[v].pos = vec_sub(c.verts[v].pos, centerOffset);
    }
  }

  // Radii.
  float maxRadius = 0.0f;
  float maxRadiusXY = 0.0f;
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    const auto& sub = lod.subobjects[s];
    for (uint32_t v = 0; v < sub.vertCount; ++v) {
      const auto& p = sub.verts[v].pos;
      float r = vec_length(p);
      float rxy = std::sqrt(p.x * p.x + p.y * p.y);
      maxRadius = std::max(maxRadius, r);
      maxRadiusXY = std::max(maxRadiusXY, rxy);
    }
  }
  lod.maxRadius = maxRadius;
  lod.maxRadiusXY = maxRadiusXY;
  lod.maxRadiusZ = 0.0f;

  // Build material buckets: ensure buckets mirror materials; update bounds.
  const uint32_t kMaxBuckets = static_cast<uint32_t>(sizeof(table.slots) / sizeof(table.slots[0]));
  auto ensure_bucket = [&](uint32_t srcMatId) -> uint32_t {
    if (srcMatId >= lod.materialCount) srcMatId = 0;
    const auto& mat = lod.materials[srcMatId];
    for (uint32_t i = 0; i < table.count; ++i) {
      if (icase_cmp(table.slots[i].name, mat.name) == 0) {
        auto& slot = table.slots[i];
        // Match game engine's EnsureMaterialBucket: only populate offset/tiling
        // when the slot was not already initialized from the 3DP project.
        if (!slot.initialized) {
          slot.uv0_u_offset = mat.uv_u_offset[0];
          slot.uv1_u_offset = mat.uv_u_offset[1];
          slot.uv0_v_offset = mat.uv_v_offset[0];
          slot.uv1_v_offset = mat.uv_v_offset[1];
          slot.uv0_u_tiling = mat.uv_u_tiling[0];
          slot.uv1_u_tiling = mat.uv_u_tiling[1];
          slot.uv0_v_tiling = mat.uv_v_tiling[0];
          slot.uv1_v_tiling = mat.uv_v_tiling[1];
          slot.initialized = 1;
        }
        return i;
      }
    }
    if (table.count >= kMaxBuckets) {
      hasError = true;
      return table.count ? table.count - 1 : 0;
    }
    uint32_t idx = table.count++;
    auto& slot = table.slots[idx];
    std::memset(&slot, 0, sizeof(slot));
    slot.initialized = 1;
    slot.ptype = 9;
    slot.rattrib = mat.extra_flags;
    slot.pattrib = 0;
    slot.present = 1;
    // Seed generator defaults: if a 3rd texture map exists, set up for multi-stage
    // compositing; otherwise default mapfunc_u_style = 1.
    if (mat.maps[2][0] != '\0') {
      slot.tex3_stage_count = 2;
      slot.gens.u_params.style = 2;
      slot.tex3_enabled = 1;
    } else {
      slot.gens.u_params.style = 1;
    }
    // UV mapping: propagate both channels as-is from the ASE material.
    slot.uv0_u_offset = mat.uv_u_offset[0];
    slot.uv1_u_offset = mat.uv_u_offset[1];
    slot.uv0_v_offset = mat.uv_v_offset[0];
    slot.uv1_v_offset = mat.uv_v_offset[1];
    slot.uv0_u_tiling = mat.uv_u_tiling[0];
    slot.uv1_u_tiling = mat.uv_u_tiling[1];
    slot.uv0_v_tiling = mat.uv_v_tiling[0];
    slot.uv1_v_tiling = mat.uv_v_tiling[1];
    copy_str(slot.name, sizeof(slot.name), mat.name);
    copy_str(slot.tex1.path, sizeof(slot.tex1.path), mat.maps[0]);
    copy_str(slot.tex2.path, sizeof(slot.tex2.path), mat.maps[1]);
    const bool has_tex1 = slot.tex1.path[0] != '\0';
    const bool has_tex2 = slot.tex2.path[0] != '\0';
    const int map_count = static_cast<int>(has_tex1) + static_cast<int>(has_tex2);
    int mat_idx = find_material_index_by_flags(map_count, doc_is_skinned);
    // Override with shader code from material name if present
    const int name_idx = try_parse_shader_from_name(mat.name);
    if (name_idx >= 0) mat_idx = name_idx;
    copy_str(slot.shader_name, sizeof(slot.shader_name), kMaterialInfoTable[mat_idx].name);
    if ((kMaterialInfoTable[mat_idx].flags & MATERIAL_FLAG_GLASS) != 0) {
      slot.glass_reflect_hi = slot.glass_reflect_mid = slot.glass_reflect_lo = 0x80;
    }
    return idx;
  };

  // Initialize bucket bounds.
  const float kMinSentinel = 9999999.0f;
  const float kMaxSentinel = -9999999.0f;
  for (uint32_t b = 0; b < 64; ++b) {
    auto& bnd = work.buckets[b];
    bnd.slotCount = 0;
    for (float& v : bnd.bboxMin) v = kMinSentinel;
    for (float& v : bnd.bboxMax) v = kMaxSentinel;
    for (int i = 0; i < 128; ++i) {
      bnd.slotSubobjects[i] = -1;
      for (int k = 0; k < 3; ++k) {
        bnd.slotMin[i][k] = kMinSentinel;
        bnd.slotMax[i][k] = kMaxSentinel;
      }
    }
  }

  // Assign matIndex/matSlot and update AABBs.
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    auto& sub = lod.subobjects[s];
    for (int f = 0; f < sub.faceCount; ++f) {
      auto& face = sub.faces[f];
      uint32_t matId = static_cast<uint32_t>(face.srcMatId);
      uint32_t bucketId = ensure_bucket(matId);
      face.matIndex = bucketId;
      if (bucketId >= 64) continue;
      auto& bounds = work.buckets[bucketId];
      // slot lookup by srcObjIndex
      uint8_t slotIdx = 0;
      bool found = false;
      for (uint32_t si = 0; si < bounds.slotCount; ++si) {
        if (bounds.slotSubobjects[si] == static_cast<int32_t>(face.srcObjIndex)) {
          slotIdx = static_cast<uint8_t>(si);
          found = true;
          break;
        }
      }
      if (!found && bounds.slotCount < 128) {
        slotIdx = static_cast<uint8_t>(bounds.slotCount);
        bounds.slotSubobjects[bounds.slotCount++] = static_cast<int32_t>(face.srcObjIndex);
      }
      face.matSlot = slotIdx;
      // Update bucket bounds from verts[0..2].
      Vec3 v0 = sub.verts[face.vert[0]].pos;
      Vec3 v1 = sub.verts[face.vert[1]].pos;
      Vec3 v2 = sub.verts[face.vert[2]].pos;
      update_minmax(bounds.bboxMin, bounds.bboxMax, v0);
      update_minmax(bounds.bboxMin, bounds.bboxMax, v1);
      update_minmax(bounds.bboxMin, bounds.bboxMax, v2);
      update_minmax(bounds.slotMin[slotIdx], bounds.slotMax[slotIdx], v0);
      update_minmax(bounds.slotMin[slotIdx], bounds.slotMax[slotIdx], v1);
      update_minmax(bounds.slotMin[slotIdx], bounds.slotMax[slotIdx], v2);
    }
  }

  // Collapse min/max to centers.
  auto collapse_center = [kMinSentinel, kMaxSentinel](float (&minv)[3], float (&maxv)[3], float (&mid)[3]) {
    for (int i = 0; i < 3; ++i) {
      if (minv[i] > maxv[i]) { minv[i] = kMinSentinel; maxv[i] = kMaxSentinel; mid[i] = 0.0f; }
      else mid[i] = (minv[i] + maxv[i]) * 0.5f;
    }
  };
  for (uint32_t b = 0; b < table.count && b < 64; ++b) {
    auto& bnd = work.buckets[b];
    collapse_center(bnd.bboxMin, bnd.bboxMax, bnd.bboxMid);
    for (uint32_t si = 0; si < bnd.slotCount; ++si) {
      collapse_center(bnd.slotMin[si], bnd.slotMax[si], bnd.slotMid[si]);
    }
  }

  // Stretch boxes: flag verts contained; note flags on subobject/lod.
  if (!stretchBoxes.empty()) {
    bool hasStretchBoxes = false;
    for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
      auto& sub = lod.subobjects[s];
      bool objectHasStretchBoxes = false;
      for (uint32_t v = 0; v < sub.vertCount; ++v) {
        auto& vert = sub.verts[v];
        for (const auto& box : stretchBoxes) {
          if (vert.pos.x >= box.min.x && vert.pos.x <= box.max.x &&
              vert.pos.y >= box.min.y && vert.pos.y <= box.max.y &&
              vert.pos.z >= box.min.z && vert.pos.z <= box.max.z) {
            vert.flags = 1;
            vert.boneIndex[0] = box.subObjA;
            vert.boneIndex[1] = box.subObjB;
            objectHasStretchBoxes = true;
            hasStretchBoxes = true;
          }
        }
      }
      if (objectHasStretchBoxes) sub.flags |= 1;
    }
    if (hasStretchBoxes) lod.flags |= 2;
  }

  // For non-skinned docs, map center i>=1 to nearest attach point; write
  // attachIndex to subobject i.
  //
  // Importer-generated BulletLOD ASE for skinned meshes can be parsed as
  // non-skinned docs but still carry explicit parent intent in attach objects:
  //   attach name "~NNx" encodes parent subobject NN-1
  //   attach parent object name "MM..." encodes child subobject MM-1
  // Prefer this deterministic mapping first when the project forces skinned,
  // then fall back to legacy nearest-attach matching for any unassigned nodes.
  if (!doc_is_skinned && lod.attachCount > 0 && lod.centerCount > 1) {
    std::vector<uint8_t> attach_assigned(lod.subobjectCount, 0);

    if (project_forces_skinned) {
      for (int idx : attach_indices) {
        const auto& obj = doc.objects[idx];
        // Parent from "~NNx attach" name.
        int parent_idx = 0;
        const int parent_parsed = parse_two_char_atol(obj.name, 1);
        if (parent_parsed > 0) {
          const int candidate = parent_parsed - 1;
          if (candidate >= 0 && candidate < static_cast<int>(lod.subobjectCount)) {
            parent_idx = candidate;
          }
        }

        // Child from parent node name "MM..." (e.g. "14.001" -> subobject 13).
        const int child_parsed = parse_two_char_atol(obj.parent_name, 0);
        if (child_parsed <= 0) continue;
        const int child_idx = child_parsed - 1;
        if (child_idx <= 0 || child_idx >= static_cast<int>(lod.subobjectCount)) continue;

        lod.subobjects[static_cast<uint32_t>(child_idx)].attachIndex = parent_idx;
        attach_assigned[static_cast<size_t>(child_idx)] = 1;
      }
    }

    for (uint32_t i = 1; i < lod.subobjectCount && i < lod.centerCount; ++i) {
      if (attach_assigned[static_cast<size_t>(i)] != 0) continue;

      float bestDist = 1e30f;
      uint32_t bestAttach = 0;
      for (uint32_t aidx = 0; aidx < lod.attachCount; ++aidx) {
        Vec3 d{
            lod.attachPoints[aidx].pos[0] - lod.centerPoints[i].pos[0],
            lod.attachPoints[aidx].pos[1] - lod.centerPoints[i].pos[1],
            lod.attachPoints[aidx].pos[2] - lod.centerPoints[i].pos[2],
        };
        float dist = vec_length(d);
        if (dist < bestDist) {
          bestDist = dist;
          bestAttach = aidx;
        }
      }
      // Attach names are ~NN; default to 0 on parse failure.
      int parsed = 0;
      if (lod.attachPoints[bestAttach].name[0]) {
        parsed = parse_two_char_atol(lod.attachPoints[bestAttach].name, 1);
      }
      int attach_index = 0;
      if (parsed > 0) {
        const int candidate = parsed - 1;
        if (candidate >= 0 && candidate < static_cast<int>(lod.subobjectCount)) {
          attach_index = candidate;
        }
      }
      lod.subobjects[i].attachIndex = attach_index;
    }
  }

  if (!export_is_skinned) {
    // Importer-generated ASE can contain digit-named subobjects with no mesh
    // payload. Seed a deterministic placeholder vertex so ROBJ/CVRT are stable.
    for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
      auto& sub = lod.subobjects[s];
      const bool has_mesh = (s < subobject_has_mesh.size() && subobject_has_mesh[s] != 0);
      if (has_mesh || sub.vertCount != 0 || sub.faceCount != 0) continue;

      Vertex seed{};
      bool have_seed = false;
      int source_sub = -1;
      const uint32_t parent = static_cast<uint32_t>(sub.attachIndex);
      if (parent < lod.subobjectCount && parent != s &&
          lod.subobjects[parent].vertCount > 0) {
        source_sub = static_cast<int>(parent);
      } else {
        for (uint32_t si = 0; si < lod.subobjectCount; ++si) {
          if (si == s) continue;
          if (lod.subobjects[si].vertCount > 0) {
            source_sub = static_cast<int>(si);
            break;
          }
        }
      }
      const bool has_center = (s < lod.centerCount);
      const float center_x = has_center ? lod.centerPoints[s].pos[0] : 0.0f;
      const float center_y = has_center ? lod.centerPoints[s].pos[1] : 0.0f;
      const float center_z = has_center ? lod.centerPoints[s].pos[2] : 0.0f;
      const bool center_nonzero =
          (std::fabs(center_x) + std::fabs(center_y) + std::fabs(center_z)) > 1e-6f;

      if (s < center_seed_valid.size() && center_seed_valid[s]) {
        seed = center_seed_verts[s];
        have_seed = true;
      } else if (source_sub >= 0) {
        seed = lod.subobjects[static_cast<uint32_t>(source_sub)].verts[0];
        have_seed = true;
      } else if (center_nonzero) {
        seed.pos.x = center_x;
        seed.pos.y = center_y;
        seed.pos.z = center_z;
        have_seed = true;
      } else if (has_center) {
        seed.pos.x = center_x;
        seed.pos.y = center_y;
        seed.pos.z = center_z;
        have_seed = true;
      }
      if (!have_seed) continue;

      sub.verts = new Vertex[1]{};
      sub.verts[0] = seed;
      sub.verts[0].boneIndex[0] = static_cast<int32_t>(s);
      sub.verts[0].boneIndex[1] = static_cast<int32_t>(s);
      sub.verts[0].boneIndex[2] = 0;
      sub.verts[0].boneIndex[3] = 0;
      sub.verts[0].boneWeight[0] = 0.0f;
      sub.verts[0].boneWeight[1] = 0.0f;
      sub.verts[0].boneWeight[2] = 0.0f;
      sub.verts[0].boneWeight[3] = 0.0f;
      sub.verts[0].flags = 0;
      sub.vertCount = 1;
    }

    // Placeholder injection can change bounds/radius.
    float maxRadius = 0.0f;
    float maxRadiusXY = 0.0f;
    for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
      const auto& sub = lod.subobjects[s];
      for (uint32_t v = 0; v < sub.vertCount; ++v) {
        const auto& p = sub.verts[v].pos;
        const float r = vec_length(p);
        const float rxy = std::sqrt(p.x * p.x + p.y * p.y);
        maxRadius = std::max(maxRadius, r);
        maxRadiusXY = std::max(maxRadiusXY, rxy);
      }
    }
    lod.maxRadius = maxRadius;
    lod.maxRadiusXY = maxRadiusXY;
  }

  // Collision counts per subobject (exclude volume-only types 20-24).
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    lod.subobjects[s].collisionCount = 0;
  }
  for (uint32_t cidx = 0; cidx < lod.collisionCount; ++cidx) {
    const auto& c = lod.collisions[cidx];
    if (c.objectIndex < lod.subobjectCount && !(c.collidableType >= 20 && c.collidableType <= 24)) {
      ++lod.subobjects[c.objectIndex].collisionCount;
    }
  }

  // Collision overlap: mark faces whose shrunken bbox lies inside another CB box.
  for (uint32_t n = 0; n < lod.collisionCount; ++n) {
    auto& col = lod.collisions[n];
    for (uint32_t fi = 0; fi < col.faceCount; ++fi) {
      auto& face = col.faces[fi];
      Vec3 v0 = col.verts[face.vert[0]].pos;
      Vec3 v1 = col.verts[face.vert[1]].pos;
      Vec3 v2 = col.verts[face.vert[2]].pos;
      float minx = std::min({v0.x, v1.x, v2.x});
      float maxx = std::max({v0.x, v1.x, v2.x});
      float miny = std::min({v0.y, v1.y, v2.y});
      float maxy = std::max({v0.y, v1.y, v2.y});
      float minz = std::min({v0.z, v1.z, v2.z});
      float maxz = std::max({v0.z, v1.z, v2.z});
      const float pad = 0.0099999998f;
      float innerMinX = minx + pad;
      float innerMaxX = maxx - pad;
      float innerMinY = miny + pad;
      float innerMaxY = maxy - pad;
      float innerMinZ = minz + pad;
      float innerMaxZ = maxz - pad;
      face.collOverlap = 0;
      if (face.planeIndex < col.planeCount && face.planeIndex < 32) {
        col.planes[face.planeIndex].flags = 0;
      }
      for (uint32_t jj = 0; jj < lod.collisionCount; ++jj) {
        const auto& other = lod.collisions[jj];
        if (jj == n || other.collidableType != 1) continue;
        if (innerMinX >= other.bbox.minX && innerMaxX <= other.bbox.maxX &&
            innerMinY >= other.bbox.minY && innerMaxY <= other.bbox.maxY &&
            innerMinZ >= other.bbox.minZ && innerMaxZ <= other.bbox.maxZ) {
          face.collOverlap = 1;
        }
      }
      if (face.collOverlap != 0 && face.planeIndex < col.planeCount && face.planeIndex < 32) {
        col.planes[face.planeIndex].flags = 1;
      }
    }
  }

  // Propagate parse flag.
  lod.flags |= (doc.flags & 1);

  // Plane data (render faces): mirror BuildTransformMatrix/NegateFacePlanes.
  for (uint32_t s = 0; s < lod.subobjectCount; ++s) {
    auto& sub = lod.subobjects[s];
    for (int f = 0; f < sub.faceCount; ++f) {
      auto& face = sub.faces[f];
      const Vec3& v0 = sub.verts[face.vert[0]].pos;
      const Vec3& v1 = sub.verts[face.vert[1]].pos;
      const Vec3& v2 = sub.verts[face.vert[2]].pos;
      Uv uv0{0, 0}, uv1{0, 0}, uv2{0, 0};
      if (face.uv[0] < sub.uvCount) uv0 = sub.uvs[face.uv[0]];
      if (face.uv[1] < sub.uvCount) uv1 = sub.uvs[face.uv[1]];
      if (face.uv[2] < sub.uvCount) uv2 = sub.uvs[face.uv[2]];
      compute_face_plane(face.faceBasis, v0, v1, v2, uv0, uv1, uv2);
    }
  }

  // Sort lights by name (LPxx).
  if (lod.lightCount > 1) {
    std::sort(lod.lights, lod.lights + lod.lightCount,
              [](const Light& a, const Light& b) { return std::strcmp(a.name, b.name) < 0; });
  }

  if (hasError && err) *err = "Conversion logged errors";
  (void)hasWarning;
  return true;
}

}  // namespace oed
