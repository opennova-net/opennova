#include "oed/export_3di.h"

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <limits>
#include <algorithm>
#include <iterator>
#include <cstring>
#include <type_traits>
#include <fstream>
#include <cctype>
#include <array>
#include <unordered_map>

#include "threedi_model.h"
#include "threedi/threedi_panm.h"

#include "oed/types.h"
#include "oed/rdta.h"

namespace oed {

namespace {

// Control register table populated per-export; used for style>112 lookups.
static thread_local std::unordered_map<std::string, int> g_ctrl_reg_map;

// Copy a C string into a fixed-size destination buffer (null-terminated).
template <size_t N>
void copy_padded(const std::string &src, char (&dst)[N]) {
  std::snprintf(dst, N, "%.*s", static_cast<int>(N - 1),
                static_cast<int>(src.size()) > 0 ? src.c_str() : "");
}

// Convert a float to 16.16 fixed (truncated, matching ModSuperOED behavior).
inline int32_t to_fixed_16_16(float v) {
  return static_cast<int32_t>(v * 65536.0f);
}

inline float deg_to_rad(float degrees) {
  return degrees * 0.017453289f;  // Matches D3DX pi/180 constant in Export3DI.
}

void mul4x4(const float *a, const float *b, float (&out)[16]) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[i * 4 + k] * b[k * 4 + j];
      }
      out[i * 4 + j] = sum;
    }
  }
}

void build_light_view_proj(const Light &src, float (&out)[16]) {
  // Build a view matrix from the stored direction vector (-rotY, rotZ, rotX) and position (-Y, Z, X).
  float dir[3] = {-src.rotY, src.rotZ, src.rotX};
  normalize_vec3(dir);

  float right[3] = {dir[2], 0.0f, -dir[0]};
  normalize_vec3(right);

  float up[3]{};
  cross3(dir, right, up);
  normalize_vec3(up);

  float view[16]{};
  view[0] = right[0]; view[1] = up[0]; view[2] = dir[0];
  view[4] = right[1]; view[5] = up[1]; view[6] = dir[1];
  view[8] = right[2]; view[9] = up[2]; view[10] = dir[2];
  view[15] = 1.0f;

  const float pos[3] = {-src.y, src.z, src.x};
  view[12] = -(pos[0] * view[0] + pos[1] * view[4] + pos[2] * view[8]);
  view[13] = -(pos[0] * view[1] + pos[1] * view[5] + pos[2] * view[9]);
  view[14] = -(pos[0] * view[2] + pos[1] * view[6] + pos[2] * view[10]);

  // Perspective projection with aspect=1, near=0.1, far=atten_end, FOV=2*falloff degrees.
  float proj[16]{};
  const float fov_rad = (src.falloff + src.falloff) * 0.017453289f;
  const float zn = 0.1f;
  const float zf = src.atten_end;
  const float y_scale = 1.0f / std::tan(fov_rad * 0.5f);
  proj[0] = y_scale;
  proj[5] = y_scale;
  proj[10] = zf / (zf - zn);
  proj[11] = 1.0f;
  proj[14] = (-zn * zf) / (zf - zn);

  mul4x4(view, proj, out);
}

inline uint8_t clamp_byte(int v) { return static_cast<uint8_t>(v & 0xFF); }
inline uint8_t pack_color(float v) { return clamp_byte(static_cast<int32_t>(v * 255.0f)); }
inline uint8_t pack_phase(float v) { return clamp_byte(static_cast<int32_t>(v * 256.0f)); }
inline uint16_t pack_rate(float v) { return static_cast<uint16_t>(static_cast<int32_t>(v * 256.0f)); }

inline uint8_t find_control_register_index(const char *name) {
  if (!name || !*name) return 0;
  auto it = g_ctrl_reg_map.find(name);
  if (it == g_ctrl_reg_map.end()) return 0;
  return static_cast<uint8_t>(it->second);
}

inline bool is_occlusion_collidable(uint32_t collidableType);

struct CollisionNormalList {
  std::vector<Vec3> normals;
};

// --- Material helpers (MTRL) ---



static int compute_texture_type(const std::string &texture_name, int slot) {
  switch (slot) {
    case 1:
    case 2:
      return 0;
    case 3:
    case 4:
      if (texture_name.find(".tga") != std::string::npos ||
          texture_name.find(".TGA") != std::string::npos) {
        return 5;
      }
      return 4;
    default:
      return 0;
  }
}

static ThreediMaterialTexture *append_texture_slot(
    ThreediMaterialTexture *dst, const MaterialTexSlot *src_frames, int slot,
    uint32_t frame_count) {
  if (!dst || !src_frames) return dst;
  const uint32_t frames = frame_count > 0 ? frame_count : 1u;
  bool all_same = true;
  for (uint32_t f = 1; f < frames; ++f) {
    if (std::strcmp(src_frames[f].path, src_frames[0].path) != 0) {
      all_same = false;
      break;
    }
  }
  if (all_same) {
    const auto &src = src_frames[0];
    std::memset(dst->name, 0, sizeof(dst->name));
    std::memcpy(dst->name, src.path,
                std::min(sizeof(dst->name) - 1, sizeof(src.path)));
    dst->slot = static_cast<uint8_t>(slot);
    dst->type =
        static_cast<uint8_t>(compute_texture_type(dst->name, dst->slot));
    dst->frame = 0;
    dst->flags =
        static_cast<uint8_t>((src.flags & 1) ? (dst->flags | 2u)
                                             : (dst->flags & 0xFD));
    ++dst;
  } else {
    for (uint32_t f = 0; f < frames; ++f) {
      const auto &src = src_frames[f];
      std::memset(dst->name, 0, sizeof(dst->name));
      std::memcpy(dst->name, src.path,
                  std::min(sizeof(dst->name) - 1, sizeof(src.path)));
      dst->slot = static_cast<uint8_t>(slot);
      dst->type =
          static_cast<uint8_t>(compute_texture_type(dst->name, dst->slot));
      dst->frame = static_cast<uint8_t>(f);
      dst->flags |= 1u;
      dst->flags =
          static_cast<uint8_t>((src.flags & 1) ? (dst->flags | 2u)
                                               : (dst->flags & 0xFD));
      ++dst;
    }
  }
  return dst;
}

static void copy_texture_slot(ThreediMaterialTexture &dst,
                              const MaterialTexSlot &src, int slot) {
  std::memset(dst.name, 0, sizeof(dst.name));
  std::memcpy(dst.name, src.path,
              std::min(sizeof(dst.name) - 1, sizeof(src.path)));
  dst.slot = static_cast<uint8_t>(slot);
  dst.type =
      static_cast<uint8_t>(compute_texture_type(dst.name, dst.slot));
  dst.frame = 0;
  dst.flags = static_cast<uint8_t>((src.flags & 1) ? 2u : (dst.flags & 0xFD));
}

static void copy_texture_path(ThreediMaterialTexture &dst, const char *path,
                              int flags, int slot) {
  std::memset(dst.name, 0, sizeof(dst.name));
  if (path) {
    std::snprintf(dst.name, sizeof(dst.name), "%s", path);
  }
  dst.slot = static_cast<uint8_t>(slot);
  dst.type =
      static_cast<uint8_t>(compute_texture_type(dst.name, dst.slot));
  dst.frame = 0;
  dst.flags = static_cast<uint8_t>((flags & 1) ? 2u : (dst.flags & 0xFD));
}

// Overlay for the portion of MaterialBucketSlot we need for MTRL.
struct MaterialSlotView {
  const char *shader_name;
  const char *tex1_path;
  uint32_t rattrib;
  uint8_t alpha_test;
  int glass_reflect_lo;
  int glass_reflect_mid;
  int glass_reflect_hi;
};

struct MapFuncView {
  int style = 0;
  float rate = 0.0f;
  float phase = 0.0f;
  float start = 0.0f;
  float end = 0.0f;
  std::string ctrl_reg;
};
struct AlphaGenView {
  int style = 0;
  float rate = 0.0f;
  float phase = 0.0f;
  float start = 0.0f;
  float end = 0.0f;
  std::string ctrl_reg;
};
struct RgbGenView {
  int style = 0;
  float rate = 0.0f;
  float phase = 0.0f;
  float start[3] = {0.0f, 0.0f, 0.0f};
  float end[3] = {0.0f, 0.0f, 0.0f};
  std::string ctrl_reg;
};

static MapFuncView read_mapfunc(const MaterialBucketSlot &slot, bool is_v) {
  MapFuncView out{};
  const auto &mf = is_v ? slot.gens.v_params : slot.gens.u_params;
  out.style = mf.style;
  out.rate = mf.rate;
  out.phase = mf.phase;
  out.start = mf.start;
  out.end = mf.end;
  out.ctrl_reg.assign(mf.ctrlReg, strnlen(mf.ctrlReg, sizeof(mf.ctrlReg)));
  return out;
}

static AlphaGenView read_alpha_gen(const MaterialBucketSlot &slot) {
  AlphaGenView out{};
  const auto &ag = slot.gens.alpha_gen;
  out.style = ag.style;
  out.rate = ag.rate;
  out.phase = ag.phase;
  out.start = ag.start;
  out.end = ag.end;
  out.ctrl_reg.assign(ag.ctrlReg, strnlen(ag.ctrlReg, sizeof(ag.ctrlReg)));
  return out;
}

static RgbGenView read_rgb_gen(const MaterialBucketSlot &slot) {
  RgbGenView out{};
  const auto &rg = slot.gens.rgb_gen;
  out.style = rg.style;
  out.rate = rg.rate;
  out.phase = rg.phase;
  out.start[0] = static_cast<float>(rg.srgb.r) / 255.0f;
  out.start[1] = static_cast<float>(rg.srgb.g) / 255.0f;
  out.start[2] = static_cast<float>(rg.srgb.b) / 255.0f;
  out.end[0] = static_cast<float>(rg.ergb.r) / 255.0f;
  out.end[1] = static_cast<float>(rg.ergb.g) / 255.0f;
  out.end[2] = static_cast<float>(rg.ergb.b) / 255.0f;
  out.ctrl_reg.assign(rg.ctrlReg, strnlen(rg.ctrlReg, sizeof(rg.ctrlReg)));
  return out;
}

static std::vector<std::string> collect_control_registers(
    const std::vector<const LodBucketWorkspace *> &workspaces,
    size_t render_lod_count,
    const MaterialTable *materials) {
  g_ctrl_reg_map.clear();
  std::vector<std::string> regs;
  auto add_reg = [&](const std::string &name) {
    if (name.empty()) return;
    if (g_ctrl_reg_map.find(name) != g_ctrl_reg_map.end()) return;
    int idx = static_cast<int>(regs.size());
    regs.push_back(name);
    g_ctrl_reg_map[name] = idx;
  };

  if (materials) {
    for (uint32_t i = 0; i < materials->count; ++i) {
      const auto &slot = materials->slots[i];
      const auto u = read_mapfunc(slot, false);
      if (u.style > 112) add_reg(u.ctrl_reg);
      const auto v = read_mapfunc(slot, true);
      if (v.style > 112) add_reg(v.ctrl_reg);
      const auto alpha = read_alpha_gen(slot);
      if (alpha.style > 112) add_reg(alpha.ctrl_reg);
      const auto rgb = read_rgb_gen(slot);
      if (rgb.style > 112) add_reg(rgb.ctrl_reg);
      if (slot.anim_meta.anim_type == 1 && slot.anim_meta.ctrl_reg[0] != '\0') {
        add_reg(std::string(slot.anim_meta.ctrl_reg));
      }
    }
  }

  auto maybe_add_part_anim_reg = [&](const PartAnimFunc &func) {
    if (func.func == 113 && func.ctrlReg[0] != '\0') {
      add_reg(std::string(func.ctrlReg));
    }
  };

  for (size_t li = 0; li < render_lod_count; ++li) {
    const LodBucketWorkspace &workspace = *workspaces[li];
    const auto &pa = workspace.part_anim;
    const uint32_t part_count = std::min<uint32_t>(
        workspace.lod.subobjectCount,
        static_cast<uint32_t>(std::size(pa.slots)));
    for (uint32_t i = 0; i < part_count; ++i) {
      const PartAnimSubobject &slot = pa.slots[i];
      if (slot.rotate_type == 2) {
        maybe_add_part_anim_reg(slot.yaw_func);
        maybe_add_part_anim_reg(slot.pitch_func);
        maybe_add_part_anim_reg(slot.roll_func);
      }
      if (slot.scale_type == 1) {
        maybe_add_part_anim_reg(slot.scale_func);
      } else if (slot.scale_type == 2) {
        maybe_add_part_anim_reg(slot.scalex_func);
        maybe_add_part_anim_reg(slot.scaley_func);
        maybe_add_part_anim_reg(slot.scalez_func);
      }
      if (slot.trans_type == 1) {
        maybe_add_part_anim_reg(slot.transx_func);
      } else if (slot.trans_type == 2) {
        maybe_add_part_anim_reg(slot.transy_func);
      } else if (slot.trans_type == 3) {
        maybe_add_part_anim_reg(slot.transz_func);
      }
    }

    if (li == 0) {
      for (uint32_t i = 0; i < workspace.lod.lightCount; ++i) {
        const auto &cg = workspace.lod.lights[i].colorgen;
        if (cg.style > 112 && cg.ctrlReg[0] != '\0') {
          add_reg(std::string(cg.ctrlReg));
        }
      }
    }
  }

  return regs;
}

// Fallback helper: when MaterialTable texture flags are absent (e.g. JSON
// fixtures), derive a best-effort value from the source material flags.
static int material_texture_flags(const MaterialTable *materials,
                                  const LodHeader &, uint32_t mat_index,
                                  int slot) {
  int flags = 0;
  if (materials && mat_index < materials->count) {
    const auto &s = materials->slots[mat_index];
    const MaterialTexSlot *src = nullptr;
    if (slot == 1) src = &s.tex1;
    else if (slot == 2) src = &s.tex2;
    if (src) flags = src->flags;
  }
  return flags;
}

static bool build_materials_chunk(const LodBucketWorkspace &workspace,
                                  const MaterialTable *materials,
                                  ThreediModel &model,
                                  std::string &error) {
  const uint32_t count =
      materials ? materials->count
                : static_cast<uint32_t>(workspace.lod.materialCount);
  if (count == 0) {
    model.material_count = 0;
    model.materials = nullptr;
    model.material_record_size = 0;
    return true;
  }

  model.material_count = count;
  model.material_record_size = 0;  // default (584) in serializer
  model.materials =
      static_cast<ThreediMaterial *>(std::calloc(count, sizeof(ThreediMaterial)));
  if (!model.materials) {
    error = "allocating materials failed";
    return false;
  }

  for (uint32_t i = 0; i < count; ++i) {
    ThreediMaterial &dst = model.materials[i];
    dst.index = static_cast<int32_t>(i);

    const MaterialBucketSlot *slot_ptr = nullptr;
    MaterialSlotView view{};
    if (materials) {
      slot_ptr = &materials->slots[i];
      const auto &slot = *slot_ptr;
      view.shader_name = slot.shader_name[0] ? slot.shader_name : nullptr;
      view.tex1_path = slot.tex1.path;
      view.rattrib = slot.rattrib;
      view.alpha_test = slot.alphatestvalue;
      view.glass_reflect_lo = slot.glass_reflect_lo;
      view.glass_reflect_mid = slot.glass_reflect_mid;
      view.glass_reflect_hi = slot.glass_reflect_hi;
    } else if (workspace.lod.materials && i < static_cast<uint32_t>(workspace.lod.materialCount)) {
      const auto &mat = workspace.lod.materials[i];
      view.shader_name = nullptr;
      view.tex1_path = mat.maps[0];
      view.rattrib = mat.flags;
      view.alpha_test = 0;
      view.glass_reflect_lo = view.glass_reflect_mid = view.glass_reflect_hi = 0;
    }
    const char* tex1_src = view.tex1_path;
    const char* tex2_src = materials ? materials->slots[i].tex2.path : nullptr;
    // Prefer original material map names (workspace) to avoid truncated slot paths,
    // matching by material name instead of index (table order can differ).
    const char* src_tex1 = nullptr;
    const char* src_tex2 = nullptr;
    if (workspace.lod.materials) {
      for (int m = 0; m < workspace.lod.materialCount; ++m) {
        if (std::strcmp(workspace.lod.materials[m].name,
                        materials ? materials->slots[i].name : "") == 0) {
          src_tex1 = workspace.lod.materials[m].maps[0];
          src_tex2 = workspace.lod.materials[m].maps[1];
          break;
        }
      }
    }
    if (src_tex1 && src_tex1[0]) tex1_src = src_tex1;
    if (src_tex2 && src_tex2[0]) tex2_src = src_tex2;

    const bool has_tex1 = tex1_src && tex1_src[0] != '\0';
    const bool has_tex2 = tex2_src && tex2_src[0] != '\0';
    const char *shader = nullptr;
    if (view.shader_name &&
        static_cast<unsigned char>(view.shader_name[0]) == 0x80) {
      if (view.glass_reflect_mid == 0) view.glass_reflect_mid = 0x80;
      if (view.glass_reflect_hi == 0) view.glass_reflect_hi = 0x80;
    }
    const bool slot_has_shader =
        view.shader_name && view.shader_name[0] != '\0' &&
        static_cast<unsigned char>(view.shader_name[0]) != 0x80;
    if (slot_has_shader) {
      shader = view.shader_name;
    } else if ((view.glass_reflect_lo & 0xFFu) == 0x80u) {
      shader = "FFP_GLASS";
    } else if (has_tex2) {
      shader = "FF_MT_OP";
    } else {
      shader = "FF_ST_OP";
    }
    copy_padded(shader, dst.shader_name);

    const auto flags = lookup_material_info_flags(shader);
    const bool animated =
        materials && ((materials->slots[i].rattrib & 0x100) != 0);

    ThreediMaterialTexture *tex_cursor = dst.textures;
    if ((flags & MATERIAL_FLAG_DIFFUSE) != 0) {
      if (animated && materials) {
        tex_cursor = append_texture_slot(
            tex_cursor, materials->slots[i].anim_textures.anim_diffuse[0], 1,
            materials->slots[i].anim_meta.anim_frames);
      } else if (has_tex1) {
        const int flags_src = material_texture_flags(materials, workspace.lod,
                                                     i, 1);
        copy_texture_path(*tex_cursor, tex1_src, flags_src, 1);
        ++tex_cursor;
      }
    }
    if ((flags & MATERIAL_FLAG_SECONDARY) != 0) {
      if (animated && materials) {
        tex_cursor = append_texture_slot(
            tex_cursor, materials->slots[i].anim_textures.anim_diffuse[1], 2,
            materials->slots[i].anim_meta.anim_frames);
      } else if (has_tex2) {
        const int flags_src = material_texture_flags(materials, workspace.lod,
                                                     i, 2);
        copy_texture_path(*tex_cursor, tex2_src ? tex2_src : "", flags_src, 2);
        ++tex_cursor;
      }
    }
    if ((flags & MATERIAL_FLAG_NORMAL_A) != 0 && materials) {
      if (animated) {
        tex_cursor = append_texture_slot(
            tex_cursor, materials->slots[i].anim_textures.anim_normal[0], 3,
            materials->slots[i].anim_meta.anim_frames);
      } else {
        copy_texture_slot(*tex_cursor, materials->slots[i].anim_textures.normal[0], 3);
        ++tex_cursor;
      }
    }
    if ((flags & MATERIAL_FLAG_NORMAL_B) != 0 && materials) {
      if (animated) {
        tex_cursor = append_texture_slot(
            tex_cursor, materials->slots[i].anim_textures.anim_normal[1], 4,
            materials->slots[i].anim_meta.anim_frames);
      } else {
        copy_texture_slot(*tex_cursor, materials->slots[i].anim_textures.normal[1], 4);
        ++tex_cursor;
      }
    }
    dst.texture_count = static_cast<uint32_t>(tex_cursor - dst.textures);

    // Control register fields: seed with sentinel values and fill from generators.
    dst.alpha_gen.reg = -1;
    dst.rgb_gen.reg = -1;
    dst.u_params.reg = -1;
    dst.v_params.reg = -1;
    if (slot_ptr) {
      const AlphaGenView alpha = read_alpha_gen(*slot_ptr);
      dst.alpha_gen.style = static_cast<uint8_t>(alpha.style);
      dst.alpha_gen.phase = alpha.phase;
      dst.alpha_gen.rate = alpha.rate;
      dst.alpha_gen.start =
          static_cast<int16_t>(std::lround(static_cast<double>(alpha.start)));
      dst.alpha_gen.end =
          static_cast<int16_t>(std::lround(static_cast<double>(alpha.end)));
      dst.alpha_gen.reg = alpha.ctrl_reg.empty()
                              ? -1
                              : find_control_register_index(alpha.ctrl_reg.c_str());

      const RgbGenView rgb = read_rgb_gen(*slot_ptr);
      dst.rgb_gen.style = static_cast<uint8_t>(rgb.style);
      dst.rgb_gen.phase = rgb.phase;
      dst.rgb_gen.rate = rgb.rate;
      dst.rgb_gen.start_color[0] = rgb.start[0];
      dst.rgb_gen.start_color[1] = rgb.start[1];
      dst.rgb_gen.start_color[2] = rgb.start[2];
      dst.rgb_gen.end_color[0] = rgb.end[0];
      dst.rgb_gen.end_color[1] = rgb.end[1];
      dst.rgb_gen.end_color[2] = rgb.end[2];
      dst.rgb_gen.reg = rgb.ctrl_reg.empty()
                            ? -1
                            : find_control_register_index(rgb.ctrl_reg.c_str());

      const MapFuncView u = read_mapfunc(*slot_ptr, false);
      dst.u_params.style = static_cast<uint8_t>(u.style);
      dst.u_params.phase = u.phase;
      dst.u_params.gen_rate = u.rate;
      dst.u_params.start = u.start;
      dst.u_params.end = u.end;
      dst.u_params.reg = u.ctrl_reg.empty()
                             ? -1
                             : find_control_register_index(u.ctrl_reg.c_str());
      const MapFuncView v = read_mapfunc(*slot_ptr, true);
      dst.v_params.style = static_cast<uint8_t>(v.style);
      dst.v_params.phase = v.phase;
      dst.v_params.gen_rate = v.rate;
      dst.v_params.start = v.start;
      dst.v_params.end = v.end;
      dst.v_params.reg = v.ctrl_reg.empty()
                             ? -1
                             : find_control_register_index(v.ctrl_reg.c_str());
    }

    // Material flags: replicate WriteMTRL mapping.
    if (view.rattrib & 1) dst.material_flags |= 4u;
    if (view.rattrib & 0x400) dst.material_flags |= 1u;
    if (view.rattrib & 0x1000) dst.material_flags |= 2u;

    dst.alpha_test_value_byte = view.alpha_test;

    if ((flags & 1u) != 0) dst.emissive_type = 2;

    if ((flags & MATERIAL_FLAG_GLASS) != 0) {
      dst.reflect_color[2] =
          static_cast<float>(view.glass_reflect_lo & 0xFFu) / 255.0f;
      dst.reflect_color[1] =
          static_cast<float>(view.glass_reflect_mid & 0xFFu) / 255.0f;
      dst.reflect_color[0] =
          static_cast<float>(view.glass_reflect_hi & 0xFFu) / 255.0f;
      dst.reflect_color[3] = 0.0f;
      // Only mark as glass when reflect values are actually set.
      if ((view.glass_reflect_lo | view.glass_reflect_mid |
           view.glass_reflect_hi) & 0xFFu)
        dst.is_glass = 1;
    }

    if (animated && slot_ptr) {
      dst.animation.num_frames =
          static_cast<uint8_t>(slot_ptr->anim_meta.anim_frames);
      if (slot_ptr->anim_meta.anim_type != 0) {
        dst.animation.animation_type = 1;
        dst.animation.cycle_frame_time = static_cast<int16_t>(
            find_control_register_index(slot_ptr->anim_meta.ctrl_reg));
      } else {
        dst.animation.animation_type = 0;
        dst.animation.cycle_frame_time =
            static_cast<int16_t>(slot_ptr->anim_meta.anim_frametime);
      }
    } else {
      dst.animation.num_frames = 0;
      dst.animation.animation_type = 0;
      dst.animation.cycle_frame_time = 0;
    }
  }

  return true;
}

inline float dot3_legacy(const Vec3 &a, const Vec3 &b) {
  // Match BuildCollisionNormals/UnkFunc accumulation order:
  // z*z + y*y + x*x and z*a2[2] + y*a2[1] + x*a2[0].
  // Original x87 computes entire dot at 80-bit extended precision with no
  // intermediate truncation.  Use double to approximate that behaviour.
  return static_cast<float>(
      static_cast<double>(a.z) * b.z +
      static_cast<double>(a.y) * b.y +
      static_cast<double>(a.x) * b.x);
}

// Matches NormalizeVec3 (0x4215e0): sqrt(x*x+y*y+z*z), zero check, then scale.
// Original x87 computes sum-of-squares at 80-bit, passes as double to sqrt,
// then stores len as float.  1.0/len and component multiplies at 80-bit,
// each stored as float.  Use double to approximate.
static void normalize_vec3_legacy(Vec3 &v) {
  const double dz = v.z, dy = v.y, dx = v.x;
  const double sum = dz * dz + dy * dy + dx * dx;
  const float len = static_cast<float>(std::sqrt(sum));
  if (len == 0.0f) {
    v.x = v.y = v.z = 0.0f;
    return;
  }
  const float inv = static_cast<float>(1.0 / static_cast<double>(len));
  v.x = static_cast<float>(static_cast<double>(inv) * dx);
  v.y = static_cast<float>(static_cast<double>(inv) * dy);
  v.z = static_cast<float>(static_cast<double>(inv) * dz);
}

static void build_collision_normals(const LodHeader &lod,
                                    std::vector<CollisionNormalList> &out) {
  out.clear();
  out.resize(static_cast<size_t>(lod.subobjectCount));
  // Mirrors BuildCollisionNormals (0x4542f0): for each face, grabs
  // faceBasis[0] (the face normal), re-normalizes with x87 NormalizeVec3,
  // then deduplicates with dot > 0.999.  The j<3 vertex loop is redundant
  // (same normal 3 times) but we replicate it for exact parity.
  constexpr float kDotThresh = 0.99900001f;
  for (int32_t sub = 0; sub < lod.subobjectCount; ++sub) {
    auto &list = out[static_cast<size_t>(sub)].normals;
    const SubObject &so = lod.subobjects[sub];
    for (int32_t fi = 0; fi < so.faceCount; ++fi) {
      for (int j = 0; j < 3; ++j) {
        (void)so.faces[fi].vert[j];  // matches unused load in original
        Vec3 n{so.faces[fi].faceBasis[0][0], so.faces[fi].faceBasis[0][1],
               so.faces[fi].faceBasis[0][2]};
        // Original re-normalizes with x87 NormalizeVec3 even though the
        // vector is already nearly unit-length.  This must be replicated
        // because the dedup threshold is tight enough that the tiny
        // precision difference changes which normals group together.
        normalize_vec3_legacy(n);
        bool found = false;
        for (const auto &existing : list) {
          if (dot3_legacy(n, existing) > kDotThresh) {
            found = true;
            break;
          }
        }
        if (!found) list.push_back(n);
      }
    }
  }
}

struct LodBounds {
  float minX;
  float minZ;
  float maxY;
  float minY;
  float maxX;
  float maxZ;
  float maxRadius;
  float maxRadiusXY;
  float maxRadiusZ;
};

static LodBounds compute_lod_bounds(const LodHeader &lod,
                                    const MaterialTable *materials) {
  float minX = 10000.0f, minY = 10000.0f, minZ = 10000.0f;
  float maxX = -10000.0f, maxY = -10000.0f, maxZ = -10000.0f;
  float maxRadius = 0.0f;
  float maxRadiusXY = 0.0f;

  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    for (int32_t fi = 0; fi < so.faceCount; ++fi) {
      uint32_t matIndex = static_cast<uint32_t>(so.faces[fi].matIndex);
      const MaterialBucketSlot *slot =
          (materials && matIndex < materials->count)
              ? &materials->slots[matIndex]
              : nullptr;
      if (slot && (slot->rattrib & 0x20) != 0) {
        continue;
      }
      for (int v = 0; v < 3; ++v) {
        const Vec3 &p = so.verts[so.faces[fi].vert[v]].pos;
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.y > maxY) maxY = p.y;
        if (p.z < minZ) minZ = p.z;
        if (p.z > maxZ) maxZ = p.z;
        const float r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (r > maxRadius) maxRadius = r;
        const float rxy = std::sqrt(p.x * p.x + p.y * p.y);
        if (rxy > maxRadiusXY) maxRadiusXY = rxy;
      }
    }
  }
  LodBounds b{};
  b.minX = minX;
  b.minZ = minZ;
  b.maxY = maxY;
  b.minY = minY;
  b.maxX = maxX;
  b.maxZ = maxZ;
  b.maxRadius = maxRadius;
  b.maxRadiusXY = maxRadiusXY;
  b.maxRadiusZ = maxZ - minZ;
  return b;
}

inline int32_t to_fp16_16_round(float v) {
  return static_cast<int32_t>(std::lrint(static_cast<double>(v) * 65536.0));
}

inline int32_t to_fp16_16_round(double v) {
  return static_cast<int32_t>(std::lrint(v * 65536.0));
}

// Legacy truncation helper retained for occlusion planes/radii that used
// integer truncation in the original writer.
inline int32_t to_fp16_16_trunc(float v) {
  return static_cast<int32_t>(static_cast<double>(v) * 65536.0);
}

inline float quantize_collision_coord(float v) {
  // Collision CVRT stores s16 scaled by 256 with truncation in the original
  // writer. Quantize to that grid before handing off to the serializer (which
  // rounds) so we reproduce the truncated values.
  const int16_t raw = static_cast<int16_t>(v * 256.0f);
  return static_cast<float>(raw) / 256.0f;
}

inline int16_t dominant_axis(const Vec3 &n) {
  // Mirror WriteCNRM: truncate to s1.14 and compare absolute ints.
  const int ax = std::abs(static_cast<int>(n.x * 16384.0f));
  const int ay = std::abs(static_cast<int>(n.y * 16384.0f));
  const int az = std::abs(static_cast<int>(n.z * 16384.0f));
  if (az > ax && az > ay) return 1;  // Z is largest
  if (ay > ax && ay > az) return 2;  // Y is largest
  return 4;                          // X is largest or tie
}

inline uint32_t material_flags(const MaterialBucketSlot *slot) {
  if (!slot) return 0;
  uint32_t out = 0;
  if ((slot->rattrib & 1) != 0) out |= 1u;
  if ((slot->rattrib & 0x20) != 0) out |= 2u;
  if ((slot->pattrib & 0x100) != 0) out |= 0x100u;
  if ((slot->pattrib & 0x2000) != 0) out |= 0x800u;
  if ((slot->pattrib & 0x1000) != 0) out |= 0x400u;
  return out;
}

inline uint8_t clamp_u8_from_float(float v) {
  const int iv = static_cast<int>(v);
  if (iv < 0) return 0;
  if (iv > 255) return 255;
  return static_cast<uint8_t>(iv);
}

inline int16_t clamp_s16_from_float(float v) {
  const int iv = static_cast<int>(v);
  if (iv < std::numeric_limits<int16_t>::min()) {
    return std::numeric_limits<int16_t>::min();
  }
  if (iv > std::numeric_limits<int16_t>::max()) {
    return std::numeric_limits<int16_t>::max();
  }
  return static_cast<int16_t>(iv);
}

inline uint8_t material_poly_type(const MaterialBucketSlot *slot) {
  if (!slot) return 0;
  switch (slot->ptype) {
    case 0: return 0x0E;  // Generic
    case 1: return 0x0D;  // Dirt
    case 2: return 0x0C;  // Grass
    case 3: return 0x11;  // Snow
    case 4: return 0x12;  // Cement
    case 5: return 0x10;  // Sand
    case 6: return 0x0F;  // PackedDirt
    case 7: return 0x07;  // Water
    case 8: return 0x13;  // Railroad
    case 9: return 0x01;  // Mud
    default: return 0;
  }
}

inline uint32_t map_collidable_type(uint32_t t) {
  switch (t) {
    case 0x00: return 0;
    case 0x01: return 1;   // CB
    case 0x04: return 4;   // CL
    case 0x05: return 5;   // CV
    case 0x06: return 6;   // CA
    case 0x07: return 7;   // VC
    case 0x08: return 8;   // BB
    case 0x09: return 9;   // CD
    case 0x0A: return 10;  // CT
    case 0x0B: return 11;  // CM
    case 0x0C: return 12;  // VK
    case 0x0D: return 13;  // CF
    case 0x0E: return 14;  // LP
    case 0x10: return 16;  // DH
    case 0x11: return 17;  // DM
    case 0x12: return 18;  // DL
    case 0x13: return 19;  // CP
    default: return t;
  }
}

static void build_collision_model(const LodBucketWorkspace &workspace,
                                  const LodBucketWorkspace *lod0_workspace,
                                  const MaterialTable *materials,
                                  ThreediModel &model) {
  const auto &lod = workspace.lod;
  const bool is_skinned_mesh = (lod.flags & 4u) != 0;

  // Collision data is stored in internal/workspace space (no coordinate
  // conversion), unlike RDTA which applies (-y, z, x) to engine space.
  // Confirmed via IDA decompilation of the original game's WriteCVRT/WriteCOBJ.

  std::vector<CollisionNormalList> per_sub_normals;
  build_collision_normals(lod, per_sub_normals);

  // CVRT: vertices in subobject order, in internal space.
  std::vector<ThreediCollisionVertex> vertices;
  vertices.reserve(static_cast<size_t>(lod.subobjectCount) * 32);
  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    for (uint32_t vi = 0; vi < so.vertCount; ++vi) {
      ThreediCollisionVertex v{};
      v.position[0] = quantize_collision_coord(so.verts[vi].pos.x);
      v.position[1] = quantize_collision_coord(so.verts[vi].pos.y);
      v.position[2] = quantize_collision_coord(so.verts[vi].pos.z);
      vertices.push_back(v);
    }
  }

  // CNRM: flatten normals (in internal space).
  std::vector<ThreediCollisionNormal> normals;
  normals.reserve(per_sub_normals.size() * 8);
  for (size_t i = 0; i < per_sub_normals.size(); ++i) {
    for (const auto &n : per_sub_normals[i].normals) {
      ThreediCollisionNormal dst{};
      int16_t qx = static_cast<int16_t>(n.x * 16384.0f);
      int16_t qy = static_cast<int16_t>(n.y * 16384.0f);
      int16_t qz = static_cast<int16_t>(n.z * 16384.0f);
      dst.normal[0] = static_cast<float>(qx) / 16384.0f;
      dst.normal[1] = static_cast<float>(qy) / 16384.0f;
      dst.normal[2] = static_cast<float>(qz) / 16384.0f;
      Vec3 quant{dst.normal[0], dst.normal[1], dst.normal[2]};
      dst.dominate_axis = dominant_axis(quant);
      normals.push_back(dst);
    }
  }

  // CFAC: faces.  Normals and vertex positions in internal space.
  std::vector<ThreediCollisionFace> faces;
  faces.reserve(128);
  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    for (int32_t fi = 0; fi < so.faceCount; ++fi) {
      const Face &src = so.faces[fi];
      const MaterialBucketSlot *mat_slot =
          (materials && static_cast<uint32_t>(src.matIndex) < materials->count)
              ? &materials->slots[src.matIndex]
              : nullptr;
      const auto &normal_list = per_sub_normals[static_cast<size_t>(si)].normals;
      int normal_idx = 0;
      constexpr float kFaceNormalDot = 0.99900001f;
      for (size_t ni = 0; ni < normal_list.size(); ++ni) {
        const Vec3 face_basis{
            src.faceBasis[0][0], src.faceBasis[0][1], src.faceBasis[0][2]};
        const float dot = dot3_legacy(normal_list[ni], face_basis);
        if (dot > kFaceNormalDot) {
          normal_idx = static_cast<int>(ni);
          break;
        }
      }
      const Vec3 &v0 = so.verts[src.vert[0]].pos;
      const Vec3 &v1 = so.verts[src.vert[1]].pos;
      const Vec3 &v2 = so.verts[src.vert[2]].pos;
      // WriteCFAC truncates bbox coords to 16.16 (via unsigned __int64 cast).
      const int32_t x0 = to_fp16_16_trunc(v0.x);
      const int32_t y0 = to_fp16_16_trunc(v0.y);
      const int32_t z0 = to_fp16_16_trunc(v0.z);
      const int32_t x1 = to_fp16_16_trunc(v1.x);
      const int32_t y1 = to_fp16_16_trunc(v1.y);
      const int32_t z1 = to_fp16_16_trunc(v1.z);
      const int32_t x2 = to_fp16_16_trunc(v2.x);
      const int32_t y2 = to_fp16_16_trunc(v2.y);
      const int32_t z2 = to_fp16_16_trunc(v2.z);
      int32_t minX = std::min({x0, x1, x2});
      int32_t maxX = std::max({x0, x1, x2});
      int32_t minY = std::min({y0, y1, y2});
      int32_t maxY = std::max({y0, y1, y2});
      int32_t minZ = std::min({z0, z1, z2});
      int32_t maxZ = std::max({z0, z1, z2});
      const Vec3 &normal = normal_list[static_cast<size_t>(normal_idx)];
      const double planeDist_d =
          -(static_cast<double>(v0.x) * normal.x +
            static_cast<double>(v0.y) * normal.y +
            static_cast<double>(v0.z) * normal.z);
      const int32_t planeDist_fp = static_cast<int32_t>(planeDist_d * 65536.0);

      ThreediCollisionFace dst{};
      dst.vert_index[0] =
          static_cast<int16_t>(src.vert[0]);  // local indices per subobject
      dst.vert_index[1] = static_cast<int16_t>(src.vert[1]);
      dst.vert_index[2] = static_cast<int16_t>(src.vert[2]);
      dst.normal_index = static_cast<int16_t>(normal_idx);
      dst.plane_dist_fp16 = planeDist_fp;
      dst.min_x_fp16 = minX;
      dst.min_y_fp16 = minY;
      dst.min_z_fp16 = minZ;
      dst.max_x_fp16 = maxX;
      dst.max_y_fp16 = maxY;
      dst.max_z_fp16 = maxZ;
      dst.material_flags = static_cast<uint32_t>(material_flags(mat_slot));
      uint8_t poly_type = material_poly_type(mat_slot);
      // Legacy skinned exports use generic collision poly type.
      if (is_skinned_mesh) poly_type = 0x0E;
      dst.poly_type = poly_type;
      dst.pad[0] = dst.pad[1] = dst.pad[2] = 0;
      faces.push_back(dst);
    }
  }

  // BPLN/BVOL from collisions (non-occlusion).
  // Collision volume (-colonly) meshes live on LOD0, not the BulletLOD.
  // Use LOD0's collision data when available.
  const auto &bpln_lod = lod0_workspace ? lod0_workspace->lod : lod;
  std::vector<ThreediBoundingPlane> planes;
  std::vector<ThreediBoundingVolume> volumes;
  planes.reserve(bpln_lod.totalCollPlanes);
  volumes.reserve(bpln_lod.collisionCount);
  uint32_t coll_count = 0;
  uint32_t plane_count = 0;
  auto quantize_fp14_trunc = [](float v) -> float {
    const int32_t raw = static_cast<int32_t>(v * 16384.0f);
    return static_cast<float>(static_cast<int16_t>(raw)) / 16384.0f;
  };
  for (uint32_t i = 0; i < bpln_lod.collisionCount; ++i) {
    const Collision &c = bpln_lod.collisions[i];
    if (is_occlusion_collidable(c.collidableType)) continue;
    ++coll_count;
    plane_count += c.planeCount;
    for (uint32_t p = 0; p < c.planeCount && p < 32; ++p) {
      const CollisionPlane &src_plane = c.planes[p];
      ThreediBoundingPlane bp{};
      bp.flags = static_cast<int16_t>(src_plane.flags & 0xFFu);
      bp.normal[0] = quantize_fp14_trunc(src_plane.n.x);
      bp.normal[1] = quantize_fp14_trunc(src_plane.n.y);
      bp.normal[2] = quantize_fp14_trunc(src_plane.n.z);
      // Radius matches WriteCollisionBPlanes quantization (truncate to fp16.16).
      const int32_t radius_fp = to_fp16_16_trunc(src_plane.d);
      bp.radius = static_cast<float>(radius_fp) / 65536.0f;
      planes.push_back(bp);
    }
    ThreediBoundingVolume bv{};
    bv.collidable_type =
        static_cast<int32_t>(map_collidable_type(c.collidableType));
    bv.flags = static_cast<int32_t>(c.bvolFlags);
    bv.min_x_fp16 = to_fp16_16_trunc(c.bbox.minX);
    bv.min_y_fp16 = to_fp16_16_trunc(c.bbox.minY);
    bv.min_z_fp16 = to_fp16_16_trunc(c.bbox.minZ);
    bv.max_x_fp16 = to_fp16_16_trunc(c.bbox.maxX);
    bv.max_y_fp16 = to_fp16_16_trunc(c.bbox.maxY);
    bv.max_z_fp16 = to_fp16_16_trunc(c.bbox.maxZ);
    bv.plane_count = static_cast<int32_t>(c.planeCount);
    volumes.push_back(bv);
  }

  // COBJ: one per subobject.  All positions in internal space.
  std::vector<ThreediCollisionObject> objects;
  objects.reserve(static_cast<size_t>(lod.subobjectCount));
  const LodHeader *skinned_source_lod =
      (is_skinned_mesh && lod0_workspace) ? &lod0_workspace->lod : &lod;
  auto vertex_influences_subobject = [](const Vertex &v, int32_t sub_idx) {
    for (int bi = 0; bi < 4; ++bi) {
      if (v.boneWeight[bi] > 0.0f && v.boneIndex[bi] == sub_idx) {
        return true;
      }
    }
    return false;
  };
  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    float minX = 10000.0f, minY = 10000.0f, minZ = 10000.0f;
    float maxX = -10000.0f, maxY = -10000.0f, maxZ = -10000.0f;
    auto accumulate_vert = [&](const Vec3 &p) {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minY = std::min(minY, p.y);
      maxY = std::max(maxY, p.y);
      minZ = std::min(minZ, p.z);
      maxZ = std::max(maxZ, p.z);
    };
    if (is_skinned_mesh) {
      for (int32_t sj = 0; sj < skinned_source_lod->subobjectCount; ++sj) {
        const SubObject &other = skinned_source_lod->subobjects[sj];
        for (uint32_t vi = 0; vi < other.vertCount; ++vi) {
          if (vertex_influences_subobject(other.verts[vi], si)) {
            accumulate_vert(other.verts[vi].pos);
          }
        }
      }
    } else {
      for (uint32_t vi = 0; vi < so.vertCount; ++vi) {
        accumulate_vert(so.verts[vi].pos);
      }
      for (uint32_t ci = 0; ci < bpln_lod.collisionCount; ++ci) {
        const Collision &c = bpln_lod.collisions[ci];
        if (c.objectIndex != static_cast<uint32_t>(si)) continue;
        accumulate_vert(Vec3{c.bbox.minX, c.bbox.minY, c.bbox.minZ});
        accumulate_vert(Vec3{c.bbox.maxX, c.bbox.maxY, c.bbox.maxZ});
      }
    }
    const float midX = (minX + maxX) * 0.5f;
    const float midY = (minY + maxY) * 0.5f;
    const float midZ = (minZ + maxZ) * 0.5f;
    float maxRadius = 0.0f;
    auto accumulate_radius = [&](const Vec3 &p) {
      const float dx = p.x - midX;
      const float dy = p.y - midY;
      const float dz = p.z - midZ;
      const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
      maxRadius = std::max(maxRadius, r);
    };
    if (is_skinned_mesh) {
      for (int32_t sj = 0; sj < skinned_source_lod->subobjectCount; ++sj) {
        const SubObject &other = skinned_source_lod->subobjects[sj];
        for (uint32_t vi = 0; vi < other.vertCount; ++vi) {
          if (vertex_influences_subobject(other.verts[vi], si)) {
            accumulate_radius(other.verts[vi].pos);
          }
        }
      }
    } else {
      for (uint32_t vi = 0; vi < so.vertCount; ++vi) {
        accumulate_radius(so.verts[vi].pos);
      }
    }
    ThreediCollisionObject obj{};
    obj.unk0 = 0;
    obj.num_vertices = so.vertCount;
    obj.num_faces = so.faceCount;
    obj.num_normals = static_cast<int32_t>(per_sub_normals[static_cast<size_t>(si)].normals.size());
    const uint32_t sub_collisions =
        (si < bpln_lod.subobjectCount)
            ? bpln_lod.subobjects[si].collisionCount
            : 0u;
    obj.num_bounding_volumes = static_cast<int32_t>(sub_collisions);
    obj.parent_subobject_index = static_cast<int32_t>(so.attachIndex);
    obj.unk3 = obj.unk4 = obj.unk5 = 0;
    if (lod.centerCount > static_cast<uint32_t>(si)) {
      obj.offset[0] = static_cast<float>(to_fp16_16_trunc(lod.centerPoints[si].pos[0]));
      obj.offset[1] = static_cast<float>(to_fp16_16_trunc(lod.centerPoints[si].pos[1]));
      obj.offset[2] = static_cast<float>(to_fp16_16_trunc(lod.centerPoints[si].pos[2]));
    } else {
      obj.offset[0] = obj.offset[1] = obj.offset[2] = 0.0f;
    }
    const int32_t min_x_fp = to_fp16_16_trunc(minX);
    const int32_t min_y_fp = to_fp16_16_trunc(minY);
    const int32_t min_z_fp = to_fp16_16_trunc(minZ);
    const int32_t max_x_fp = to_fp16_16_trunc(maxX);
    const int32_t max_y_fp = to_fp16_16_trunc(maxY);
    const int32_t max_z_fp = to_fp16_16_trunc(maxZ);
    const int32_t mid_x_fp = (min_x_fp + max_x_fp) / 2;
    const int32_t mid_y_fp = (min_y_fp + max_y_fp) / 2;
    const int32_t mid_z_fp = (min_z_fp + max_z_fp) / 2;
    obj.min[0] = static_cast<float>(min_x_fp);
    obj.min[1] = static_cast<float>(min_y_fp);
    obj.min[2] = static_cast<float>(min_z_fp);
    obj.max[0] = static_cast<float>(max_x_fp);
    obj.max[1] = static_cast<float>(max_y_fp);
    obj.max[2] = static_cast<float>(max_z_fp);
    obj.med[0] = static_cast<float>(mid_x_fp);
    obj.med[1] = static_cast<float>(mid_y_fp);
    obj.med[2] = static_cast<float>(mid_z_fp);
    obj.radius = static_cast<float>(to_fp16_16_trunc(maxRadius));
    objects.push_back(obj);
  }

  // CXLT: attach points, in internal space.
  // IDA WriteCDTA calls WriteCXLT with the selected collision LOD, not LOD0.
  const LodHeader *translation_lod = &lod;
  std::vector<ThreediCollisionTranslation> translations;
  translations.reserve(translation_lod->attachCount);
  for (uint32_t i = 0; i < translation_lod->attachCount; ++i) {
    ThreediCollisionTranslation t{};
    t.translation[0] = static_cast<float>(to_fp16_16_trunc(translation_lod->attachPoints[i].pos[0]));
    t.translation[1] = static_cast<float>(to_fp16_16_trunc(translation_lod->attachPoints[i].pos[1]));
    t.translation[2] = static_cast<float>(to_fp16_16_trunc(translation_lod->attachPoints[i].pos[2]));
    translations.push_back(t);
  }

  // CMDL header values: compute bounds from internal-space positions.
  // Original WriteCDTA calls ComputeLodBounds on BOTH collision LOD and LOD0,
  // then takes the min/max envelope of both bboxes.
  float cmdl_minX = 10000.0f, cmdl_minY = 10000.0f, cmdl_minZ = 10000.0f;
  float cmdl_maxX = -10000.0f, cmdl_maxY = -10000.0f, cmdl_maxZ = -10000.0f;
  float cmdl_maxR = 0.0f, cmdl_maxRxy = 0.0f;
  auto accumulate_lod_bounds = [&](const LodHeader &scan_lod) {
    for (int32_t si = 0; si < scan_lod.subobjectCount; ++si) {
      const SubObject &so = scan_lod.subobjects[si];
      for (int32_t fi = 0; fi < so.faceCount; ++fi) {
        uint32_t matIndex = static_cast<uint32_t>(so.faces[fi].matIndex);
        const MaterialBucketSlot *slot =
            (materials && matIndex < materials->count)
                ? &materials->slots[matIndex]
                : nullptr;
        if (slot && (slot->rattrib & 0x20) != 0) continue;
        for (int v = 0; v < 3; ++v) {
          const Vec3 &p = so.verts[so.faces[fi].vert[v]].pos;
          cmdl_minX = std::min(cmdl_minX, p.x);
          cmdl_maxX = std::max(cmdl_maxX, p.x);
          cmdl_minY = std::min(cmdl_minY, p.y);
          cmdl_maxY = std::max(cmdl_maxY, p.y);
          cmdl_minZ = std::min(cmdl_minZ, p.z);
          cmdl_maxZ = std::max(cmdl_maxZ, p.z);
          const float r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
          cmdl_maxR = std::max(cmdl_maxR, r);
          const float rxy = std::sqrt(p.x * p.x + p.y * p.y);
          cmdl_maxRxy = std::max(cmdl_maxRxy, rxy);
        }
      }
    }
  };
  accumulate_lod_bounds(lod);
  if (lod0_workspace) {
    accumulate_lod_bounds(lod0_workspace->lod);
  }

  if (vertices.empty() && faces.empty() && volumes.empty()) {
    model.collision = nullptr;
    return;
  }

  model.collision = static_cast<ThreediCollisionModel *>(std::calloc(1, sizeof(ThreediCollisionModel)));
  if (!model.collision) return;

  auto copy_vec = [](auto &dst_ptr, auto &vec) {
    using T = typename std::remove_reference<decltype(vec.front())>::type;
    if (vec.empty()) {
      dst_ptr = nullptr;
      return;
    }
    dst_ptr = static_cast<T *>(std::calloc(vec.size(), sizeof(T)));
    if (dst_ptr) {
      std::memcpy(dst_ptr, vec.data(), vec.size() * sizeof(T));
    }
  };

  model.collision->vertex_count = vertices.size();
  copy_vec(model.collision->vertices, vertices);
  model.collision->normal_count = normals.size();
  copy_vec(model.collision->normals, normals);
  model.collision->face_count = faces.size();
  copy_vec(model.collision->faces, faces);
  model.collision->plane_count = planes.size();
  copy_vec(model.collision->planes, planes);
  model.collision->volume_count = volumes.size();
  copy_vec(model.collision->volumes, volumes);
  model.collision->object_count = objects.size();
  copy_vec(model.collision->objects, objects);
  model.collision->translation_count = translations.size();
  copy_vec(model.collision->translations, translations);

  model.collision->model_data.num_vertices = static_cast<int32_t>(model.collision->vertex_count);
  model.collision->model_data.num_normals = static_cast<int32_t>(model.collision->normal_count);
  model.collision->model_data.num_faces = static_cast<int32_t>(model.collision->face_count);
  model.collision->model_data.num_objects = static_cast<int32_t>(model.collision->object_count);
  model.collision->model_data.num_transforms = static_cast<int32_t>(model.collision->translation_count);
  model.collision->model_data.num_bounding_planes = static_cast<int32_t>(plane_count);
  model.collision->model_data.num_bounding_volumes = static_cast<int32_t>(coll_count);
  auto fp_exact = [](float v) {
    return static_cast<float>(to_fp16_16_trunc(v)) / 65536.0f;
  };
  // bbox: {minX, minY, minZ, maxX, maxY, maxZ} ? simple non-interleaved.
  model.collision->model_data.bbox[0] = fp_exact(cmdl_minX);
  model.collision->model_data.bbox[1] = fp_exact(cmdl_minY);
  model.collision->model_data.bbox[2] = fp_exact(cmdl_minZ);
  model.collision->model_data.bbox[3] = fp_exact(cmdl_maxX);
  model.collision->model_data.bbox[4] = fp_exact(cmdl_maxY);
  model.collision->model_data.bbox[5] = fp_exact(cmdl_maxZ);
  model.collision->model_data.radii[0] = fp_exact(cmdl_maxR);
  model.collision->model_data.radii[1] = fp_exact(cmdl_maxRxy);
  model.collision->model_data.radii[2] = fp_exact(cmdl_maxZ - cmdl_minZ);
}

// Map ModSuperOED collidableType to occlusion object type.
// WriteOCCL treats NodeType_OB (0x14) through (NodeType_DH|NodeType_BB)
// (0x18) as occlusion entries. Types: OB=0, CB=1, CS=2/3, CC=4; unknowns
// default to type 0 but still count as occlusion.
inline bool occlusion_type_from_collidable(uint32_t collidableType,
                                           uint32_t sub_index,
                                           uint8_t &out_type) {
  constexpr uint32_t kOcclusionCollidableMin = 0x14;
  constexpr uint32_t kOcclusionCollidableMax = 0x18;
  if (collidableType < kOcclusionCollidableMin ||
      collidableType > kOcclusionCollidableMax) {
    return false;
  }
  switch (collidableType - kOcclusionCollidableMin) {
    case 0:  // OB*
      out_type = 0;
      return true;
    case 1:  // CB*
      out_type = 1;
      return true;
    case 2:  // CS*
      out_type = (sub_index != 0) ? 3 : 2;
      return true;
    case 3:  // CC*
      out_type = 4;
      return true;
    default:
      out_type = 0;
      return true;
  }
}

inline bool is_occlusion_collidable(uint32_t collidableType) {
  uint8_t dummy = 0;
  return occlusion_type_from_collidable(collidableType, 0, dummy);
}

// Append occlusion data from a Collision entry (for OB/CB/CS/CC collisions).
static void append_occlusion(const Collision &src, detail::OcclusionBuffers &out) {
  uint8_t occ_type = 0;
  if (!occlusion_type_from_collidable(src.collidableType, src.sub_index,
                                      occ_type)) {
    return;
  }

  const size_t vert_base = out.vertices.size();
  const size_t plane_base = out.planes.size();

  // Vertices (axis swap: Y -> -Y, Z -> Z, X -> X).
  for (uint32_t i = 0; i < src.vertCount; ++i) {
    ThreediOcclusionVertex v{};
    v.position[0] = -src.verts[i].pos.y;
    v.position[1] = src.verts[i].pos.z;
    v.position[2] = src.verts[i].pos.x;
    out.vertices.push_back(v);
  }

  // Planes (axis swap on normal, radius kept).
  for (uint32_t i = 0; i < src.planeCount; ++i) {
    ThreediOcclusionPlane p{};
    p.normal[0] = -src.planes[i].n.y;
    p.normal[1] = src.planes[i].n.z;
    p.normal[2] = src.planes[i].n.x;
    p.radius = src.planes[i].d;
    out.planes.push_back(p);
  }

  // Faces: pack indices exactly as WriteOCCL.
  for (uint32_t i = 0; i < src.faceCount; ++i) {
    const Face &f = src.faces[i];
    ThreediOcclusionFace face{};
    const uint8_t v0 = static_cast<uint8_t>(f.vert[0]);
    const uint8_t v1 = static_cast<uint8_t>(f.vert[1]);
    const uint8_t v2 = static_cast<uint8_t>(f.vert[2]);
    const uint8_t plane_idx = static_cast<uint8_t>(f.planeIndex);
    face.raw_indices = v0 | (v1 << 8) | (v2 << 16) | (plane_idx << 24);

    const uint16_t edge01 =
        (v0 >= v1) ? static_cast<uint16_t>(v1 | (v0 << 8)) | 0x8000
                   : static_cast<uint16_t>(v0 | (v1 << 8));
    const uint16_t edge12 =
        (v1 >= v2) ? static_cast<uint16_t>(v2 | (v1 << 8)) | 0x8000
                   : static_cast<uint16_t>(v1 | (v2 << 8));
    const uint16_t edge20 =
        (v2 >= v0) ? static_cast<uint16_t>(v0 | (v2 << 8)) | 0x8000
                   : static_cast<uint16_t>(v2 | (v0 << 8));

    face.edge_data = edge01 | (edge12 << 16);
    face.other_edge_data = edge20;
    out.faces.push_back(face);
  }

  // Object header.
  ThreediOcclusionObject obj{};
  obj.type = occ_type;
  obj.parent_subobject_index = static_cast<uint8_t>(src.objectIndex);
  obj.connecting_subobject = static_cast<uint8_t>(src.sub_index);
  obj.position[0] = -src.center.y;
  obj.position[1] = src.center.z;
  obj.position[2] = src.center.x;
  obj.radius = src.radius;
  obj.glow_scale = 0.0f;
  obj.num_vertices = static_cast<int32_t>(src.vertCount);
  obj.num_planes = static_cast<int32_t>(src.planeCount);
  obj.face_count = static_cast<int32_t>(src.faceCount);
  out.objects.push_back(obj);
}

static void build_occlusion_buffers_internal(const LodHeader &lod,
                                             detail::OcclusionBuffers &out) {
  out.vertices.clear();
  out.planes.clear();
  out.faces.clear();
  out.objects.clear();
  for (uint32_t i = 0; i < lod.collisionCount; ++i) {
    append_occlusion(lod.collisions[i], out);
  }
}

static void build_occlusion(const LodBucketWorkspace &workspace,
                            ThreediModel &model) {
  detail::OcclusionBuffers accum{};
  build_occlusion_buffers_internal(workspace.lod, accum);

  model.occlusion_vertex_count = accum.vertices.size();
  model.occlusion_vertex_record_size = 12;
  model.occlusion_vertices = nullptr;
  if (!accum.vertices.empty()) {
    model.occlusion_vertices = static_cast<ThreediOcclusionVertex *>(
        std::calloc(accum.vertices.size(), sizeof(ThreediOcclusionVertex)));
    if (model.occlusion_vertices) {
      std::memcpy(model.occlusion_vertices, accum.vertices.data(),
                  accum.vertices.size() * sizeof(ThreediOcclusionVertex));
    }
  }

  model.occlusion_plane_count = accum.planes.size();
  model.occlusion_plane_record_size = 16;
  model.occlusion_planes = nullptr;
  if (!accum.planes.empty()) {
    model.occlusion_planes = static_cast<ThreediOcclusionPlane *>(
        std::calloc(accum.planes.size(), sizeof(ThreediOcclusionPlane)));
    if (model.occlusion_planes) {
      std::memcpy(model.occlusion_planes, accum.planes.data(),
                  accum.planes.size() * sizeof(ThreediOcclusionPlane));
    }
  }

  model.occlusion_face_count = accum.faces.size();
  model.occlusion_face_record_size = 12;
  model.occlusion_faces = nullptr;
  if (!accum.faces.empty()) {
    model.occlusion_faces = static_cast<ThreediOcclusionFace *>(
        std::calloc(accum.faces.size(), sizeof(ThreediOcclusionFace)));
    if (model.occlusion_faces) {
      std::memcpy(model.occlusion_faces, accum.faces.data(),
                  accum.faces.size() * sizeof(ThreediOcclusionFace));
    }
  }

  model.occlusion_object_count = accum.objects.size();
  model.occlusion_object_record_size = 36;
  model.occlusion_objects = nullptr;
  if (!accum.objects.empty()) {
    model.occlusion_objects = static_cast<ThreediOcclusionObject *>(
        std::calloc(accum.objects.size(), sizeof(ThreediOcclusionObject)));
    if (model.occlusion_objects) {
      std::memcpy(model.occlusion_objects, accum.objects.data(),
                  accum.objects.size() * sizeof(ThreediOcclusionObject));
    }
  }
}

}  // namespace

namespace detail {

void build_occlusion_buffers(const LodHeader &lod, OcclusionBuffers &out) {
  build_occlusion_buffers_internal(lod, out);
}

enum LightFlagBits : uint8_t {
  kLightDisableCorona = 1 << 0,
  kLightDisableTerrain = 1 << 1,
  kLightDisableObjects = 1 << 2,
  kLightIsSpot = 1 << 3,
};

uint8_t compute_light_flags(const Light &src) {
  uint8_t flags = 0;
  if (src.disable_corona & 1) flags |= kLightDisableCorona;
  if (src.disable_lightterrain & 1) flags |= kLightDisableTerrain;
  if (src.disable_lightobjects & 1) flags |= kLightDisableObjects;
  if (src.type != 0) flags |= kLightIsSpot;
  return flags;
}

}  // namespace detail

static bool build_3di_model_multi_impl(
    const std::vector<const LodBucketWorkspace *> &workspaces,
    const MaterialTable *materials,
    const std::vector<float> &thresholds,
    int poly_collision_lod,
    const Export3diOptions &options,
    ThreediModel &out_model,
    std::string &error) {
  std::memset(&out_model, 0, sizeof(out_model));
  if (workspaces.empty()) {
    error = "no workspaces provided";
    return false;
  }

  // LOD 0 workspace is the source for all global properties.
  const LodBucketWorkspace &ws0 = *workspaces[0];
  const LodHeader &lod0 = ws0.lod;

  // When poly_collision_lod > 0 it designates a dedicated collision-only
  // workspace (BulletLOD) that should NOT produce an RLOD.  The render LOD
  // count is then poly_collision_lod (indices 0..poly_collision_lod-1).
  const size_t render_lod_count =
      (poly_collision_lod > 0 &&
       static_cast<size_t>(poly_collision_lod) < workspaces.size())
          ? static_cast<size_t>(poly_collision_lod)
          : workspaces.size();

  bool any_skinned = false;
  float max_render_radius = 0.0f;
  for (size_t li = 0; li < render_lod_count; ++li) {
    const LodHeader &lod = workspaces[li]->lod;
    any_skinned = any_skinned || ((lod.flags & 4u) != 0);
    max_render_radius = std::max(max_render_radius, lod.maxRadius);
  }

  ThreediModel model{};
  const auto ctrl_regs =
      collect_control_registers(workspaces, render_lod_count, materials);

  model.version = 259;
  model.header.has_header = 1;
  const std::string ghdr_name =
      options.model_name.empty() ? "preview" : options.model_name;
  copy_padded(ghdr_name, model.header.name);
  model.header.mesh_type =
      any_skinned ? THREEDI_MESH_SKINNED : THREEDI_MESH_BASIC;
  model.header.lod_count_decl = static_cast<int32_t>(render_lod_count);
  model.header.lod_distance = to_fixed_16_16(max_render_radius);

  // User points (USRP): from LOD 0 only.
  model.user_point_count = lod0.userPointCount;
  if (model.user_point_count > 0) {
    model.user_points = static_cast<ThreediUserPoint *>(
        std::calloc(model.user_point_count, sizeof(ThreediUserPoint)));
    if (!model.user_points) {
      error = "allocating user_points failed";
      return false;
    }
    for (size_t i = 0; i < model.user_point_count; ++i) {
      const UserPoint &src = lod0.userPoints[i];
      ThreediUserPoint &dst = model.user_points[i];
      dst.x = to_fixed_16_16(src.pos[0]);
      dst.y = to_fixed_16_16(src.pos[1]);
      dst.z = to_fixed_16_16(src.pos[2]);
      dst.rot_x = to_fixed_16_16(src.axis[2][0]);
      dst.rot_y = to_fixed_16_16(src.axis[2][1]);
      dst.rot_z = to_fixed_16_16(src.axis[2][2]);
      dst.subobject_index = src.subObj;
      dst.userpoint_type = static_cast<int32_t>(src.type);
      copy_padded(src.name, dst.name);
    }
  }

  // CTRL: populate from collected control registers (materials, part anims, lights).
  model.ctrl.count = static_cast<uint32_t>(ctrl_regs.size());
  model.ctrl.record_size = ctrl_regs.empty() ? 0u : 24u;
  model.ctrl.registers = nullptr;
  if (!ctrl_regs.empty()) {
    model.ctrl.registers = static_cast<ThreediControlRegister *>(
        std::calloc(ctrl_regs.size(), sizeof(ThreediControlRegister)));
    if (!model.ctrl.registers) {
      error = "allocating control registers failed";
      return false;
    }
    for (size_t i = 0; i < ctrl_regs.size(); ++i) {
      copy_padded(ctrl_regs[i], model.ctrl.registers[i].name);
    }
  }

  // Matrices (MTRX): accumulate from ALL LODs.  Within each LOD, deduplicate
  // center axes, but across LODs each LOD gets its own matrix entries.  This
  // matches the original OED behavior where matrix_count equals the total
  // number of unique axes across all LODs.
  struct MatrixEntry {
    float axis[3][3];
    float mat[16];
  };
  std::vector<MatrixEntry> mtrx_entries;

  auto axis_dot = [](const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };

  auto build_matrix_from_axis = [](const float axis[3][3], float (&mat)[16]) {
    std::memset(mat, 0, sizeof(float) * 16);
    float m[3][3]{};
    m[0][0] = axis[1][1];
    m[0][1] = -axis[1][2];
    m[0][2] = -axis[1][0];
    m[1][0] = -axis[2][1];
    m[1][1] = axis[2][2];
    m[1][2] = axis[2][0];
    m[2][0] = -axis[0][1];
    m[2][1] = axis[0][2];
    m[2][2] = axis[0][0];
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        mat[r * 4 + c] = m[c][r];  // transpose = inverse
      }
    }
    // Original's matrix inversion negates the translation row (0.0 ? -0.0).
    mat[12] = -0.0f;
    mat[13] = -0.0f;
    mat[14] = -0.0f;
    mat[15] = 1.0f;
  };

  auto is_zero_axis = [](const float axis[3][3]) {
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        if (axis[r][c] != 0.0f) return false;
    return true;
  };

  auto build_nan_sentinel_matrix = [](float (&mat)[16]) {
    // Reproduce the original's sub_420E30 inversion of an all-zero axis matrix.
    // The NaN sign bits are not uniform: the 3x3 block alternates sign in a
    // checkerboard pattern, while the translation row is negative NaN.
    uint32_t nan_neg_bits = 0xFFC00000u;  // negative qNaN
    uint32_t nan_pos_bits = 0x7FC00000u;  // positive qNaN
    float nan_neg, nan_pos;
    std::memcpy(&nan_neg, &nan_neg_bits, 4);
    std::memcpy(&nan_pos, &nan_pos_bits, 4);
    // 3x3 rotation block: alternating NaN sign bits.
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        mat[r * 4 + c] = ((r + c) & 1) ? nan_pos : nan_neg;
      }
      mat[r * 4 + 3] = 0.0f;  // homogeneous column
    }
    // Translation row = negative NaN.
    mat[12] = nan_neg;
    mat[13] = nan_neg;
    mat[14] = nan_neg;
    mat[15] = 1.0f;
  };

  // Accumulate matrices from render LODs only (matches original ComputeMTRX).
  // Iteration is driven by subobjectCount, not centerCount.
  for (size_t li = 0; li < render_lod_count; ++li) {
    const LodHeader &ws_lod = workspaces[li]->lod;
    if (ws_lod.centerCount < static_cast<uint32_t>(ws_lod.subobjectCount)) {
      error = "centerCount < subobjectCount for LOD " + std::to_string(li);
      threedi_model_free(&model);
      return false;
    }
    for (uint32_t ci = 0; ci < static_cast<uint32_t>(ws_lod.subobjectCount);
         ++ci) {
      const auto &cp = ws_lod.centerPoints[ci];
      float axis[3][3]{};
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
          axis[r][c] = cp.axis[r][c];

      // Dedup against the global mtrx_entries table (matches original's
      // g_Matrices[]).  Dot product comparison only ? zero axes will have
      // dot product 0 which is < 0.999, so they never dedup with anything.
      int found = -1;
      for (size_t idx = 0; idx < mtrx_entries.size(); ++idx) {
        const auto &entry = mtrx_entries[idx];
        if (axis_dot(axis[0], entry.axis[0]) >= 0.999f &&
            axis_dot(axis[1], entry.axis[1]) >= 0.999f &&
            axis_dot(axis[2], entry.axis[2]) >= 0.999f) {
          found = static_cast<int>(idx);
          break;
        }
      }
      if (found == -1) {
        MatrixEntry entry{};
        std::memcpy(entry.axis, axis, sizeof(axis));
        if (is_zero_axis(axis)) {
          build_nan_sentinel_matrix(entry.mat);
        } else {
          build_matrix_from_axis(axis, entry.mat);
        }
        found = static_cast<int>(mtrx_entries.size());
        mtrx_entries.push_back(entry);
      }
      (void)found;  // matrix index computed per-LOD in PANM section below
    }
  }

  model.mtrx.count = static_cast<uint32_t>(mtrx_entries.size());
  model.mtrx.record_size = sizeof(float) * 16;
  if (model.mtrx.count > 0) {
    model.mtrx.matrices =
        static_cast<ThreediMatrix4x4 *>(
            std::calloc(model.mtrx.count, sizeof(ThreediMatrix4x4)));
    if (!model.mtrx.matrices) {
      error = "allocating matrices failed";
      return false;
    }
    for (size_t idx = 0; idx < mtrx_entries.size(); ++idx) {
      std::memcpy(model.mtrx.matrices[idx].m, mtrx_entries[idx].mat,
                  sizeof(float) * 16);
    }
  }

  // Materials (MTRL): build from the material table if supplied, otherwise fall
  // back to the LOD material list.
  if (!build_materials_chunk(ws0, materials, model, error)) {
    threedi_model_free(&model);
    return false;
  }

  // Lights (LGHT): from LOD 0 only.
  model.light_count = lod0.lightCount;
  if (model.light_count > 0) {
    model.lights = static_cast<ThreediLight *>(
        std::calloc(model.light_count, sizeof(ThreediLight)));
    if (!model.lights) {
      error = "allocating lights failed";
      return false;
    }
    for (size_t i = 0; i < model.light_count; ++i) {
      const Light &src = lod0.lights[i];
      ThreediLight &dst = model.lights[i];

      dst.offset[0] = -src.y;
      dst.offset[1] = src.z;
      dst.offset[2] = src.x;

      dst.atten_start = src.near_atten_start;
      dst.atten_end = src.atten_end;

      if (src.colorgen.style != 0) {
        dst.style = static_cast<uint8_t>(src.colorgen.style);
        dst.rate = pack_rate(src.colorgen.rate);
        dst.phase = (src.colorgen.style <= 0x70)
                        ? pack_phase(src.colorgen.phase)
                        : find_control_register_index(src.colorgen.ctrlReg);
        dst.color_start[0] = clamp_byte(src.colorgen.start.b);
        dst.color_start[1] = clamp_byte(src.colorgen.start.g);
        dst.color_start[2] = clamp_byte(src.colorgen.start.r);
        dst.color_start[3] = 0;
        dst.color_end[0] = clamp_byte(src.colorgen.end.b);
        dst.color_end[1] = clamp_byte(src.colorgen.end.g);
        dst.color_end[2] = clamp_byte(src.colorgen.end.r);
        dst.color_end[3] = 0;
      } else {
        dst.style = 24;
        dst.phase = 0;
        dst.rate = 0;
        dst.color_start[0] = pack_color(src.color.b);
        dst.color_start[1] = pack_color(src.color.g);
        dst.color_start[2] = pack_color(src.color.r);
        dst.color_start[3] = 0;
        dst.color_end[0] = dst.color_end[1] = dst.color_end[2] = dst.color_end[3] = 0;
      }

      dst.subobj_index = static_cast<uint8_t>(src.subObj);

      dst.unknown1 = 0;
      dst.falloff_byte = clamp_byte(static_cast<int32_t>(src.falloff));

      dst.rotation[0] = -src.rotY;
      dst.rotation[1] = src.rotZ;
      dst.rotation[2] = src.rotX;
      dst.rotation[3] = std::cos(deg_to_rad(src.falloff));

      build_light_view_proj(src, dst.view_proj);

      dst.flags = detail::compute_light_flags(src);
    }
  } else {
    model.lights = nullptr;
  }

  // LOD data: allocate render LODs only (exclude dedicated collision workspace).
  const size_t lod_count = render_lod_count;
  model.lod_count = lod_count;
  model.lods = static_cast<ThreediLod *>(
      std::calloc(lod_count, sizeof(ThreediLod)));
  if (!model.lods) {
    error = "allocating lods failed";
    return false;
  }
  for (size_t li = 0; li < lod_count; ++li) {
    const LodHeader &ws_lod = workspaces[li]->lod;
    std::memcpy(model.lods[li].model_type, ws_lod.render_function, 4);
    model.lods[li].model_type[4] = '\0';
    model.lods[li].lod_threshold =
        (li < thresholds.size())
            ? static_cast<int32_t>(thresholds[li])
            : 0;
    model.lods[li].rmdl_render_object_count = ws_lod.subobjectCount;

    if (!build_render_geometry(ws_lod, materials, any_skinned,
                               model.lods[li], error)) {
      threedi_model_free(&model);
      return false;
    }
  }

  // PANM: per-LOD, mirroring original WritePANMChunk which iterates each LOD's
  // own subobjects and calls GetPartAnimAxisIndex on each LOD's centerPoints.
  model.part_animation_count = 0;
  model.part_animation_record_size = 0;
  model.part_animations = nullptr;

  auto fill_transform = [&](const PartAnimFunc &f, bool is_rotation,
                            ThreediTransform &out) {
    out.control = static_cast<uint8_t>(f.func);
    if (f.func > 0x70) {
      out.control_param = find_control_register_index(f.ctrlReg);
    } else {
      out.control_param = clamp_u8_from_float(f.param1 * 256.0f);
    }
    out.rate = clamp_s16_from_float(f.param0 * 256.0f);
    const float scale = is_rotation ? (16384.0f / 360.0f) : 256.0f;
    out.start = clamp_s16_from_float(f.param2 * scale);
    out.end = clamp_s16_from_float(f.param3 * scale);
  };

  for (size_t li = 0; li < lod_count; ++li) {
    const LodHeader &ws_lod = workspaces[li]->lod;
    const auto &ws_pa = workspaces[li]->part_anim;
    const uint32_t sub_count = ws_lod.subobjectCount;
    if (sub_count > static_cast<uint32_t>(std::size(ws_pa.slots))) {
      error = "subobjectCount exceeds part animation slot capacity";
      threedi_model_free(&model);
      return false;
    }
    if (ws_lod.centerCount < sub_count) {
      error = "centerCount < subobjectCount during PANM export";
      threedi_model_free(&model);
      return false;
    }

    model.lods[li].part_animation_count = sub_count;
    model.lods[li].part_animation_record_size = 68;
    if (sub_count > 0) {
      model.lods[li].part_animations = static_cast<ThreediPartAnimation *>(
          std::calloc(sub_count, sizeof(ThreediPartAnimation)));
      if (!model.lods[li].part_animations) {
        error = "allocating per-LOD part_animations failed";
        threedi_model_free(&model);
        return false;
      }
      for (uint32_t i = 0; i < sub_count; ++i) {
        const PartAnimSubobject &src = ws_pa.slots[i];
        ThreediPartAnimation &dst = model.lods[li].part_animations[i];
        memset(&dst, 0, sizeof(dst));
        const uint8_t scale_type = static_cast<uint8_t>(src.scale_type);
        const uint8_t rotation_type = static_cast<uint8_t>(src.rotate_type);
        const uint8_t rotation_reversed = static_cast<uint8_t>(src.reverse_rotate);
        const uint8_t translate_type = static_cast<uint8_t>(src.trans_type);
        dst.flags = threedi_panm_pack_flags(scale_type, rotation_type, rotation_reversed, translate_type);
        dst.parent_subobject =
            static_cast<uint8_t>(ws_lod.subobjects[i].attachIndex);
        dst.subobject_index = static_cast<uint8_t>(src.transform_as);

        // GetPartAnimAxisIndex: search global mtrx_entries for matching axis.
        uint8_t matrix_idx = 0xFF;
        const auto &cp = ws_lod.centerPoints[i];
        for (size_t mi = 0; mi < mtrx_entries.size(); ++mi) {
          const auto &entry = mtrx_entries[mi];
          if (axis_dot(cp.axis[0], entry.axis[0]) >= 0.999f &&
              axis_dot(cp.axis[1], entry.axis[1]) >= 0.999f &&
              axis_dot(cp.axis[2], entry.axis[2]) >= 0.999f) {
            matrix_idx = static_cast<uint8_t>(mi);
            break;
          }
        }
        dst.matrix_index = matrix_idx;
        dst.matrix_offset = 0;
        dst.bind_matrix_index = 0;

        if (rotation_type == 2) {
          fill_transform(src.yaw_func, true, dst.rotation_x);
          fill_transform(src.pitch_func, true, dst.rotation_y);
          fill_transform(src.roll_func, true, dst.rotation_z);
        }
        if (scale_type == 1) {
          fill_transform(src.scale_func, false, dst.scale_x);
        } else if (scale_type == 2) {
          fill_transform(src.scalex_func, false, dst.scale_x);
          fill_transform(src.scaley_func, false, dst.scale_y);
          fill_transform(src.scalez_func, false, dst.scale_z);
        }
        if (translate_type > 0) {
          const PartAnimFunc *tf = nullptr;
          if (translate_type == 1) tf = &src.transx_func;
          else if (translate_type == 2) tf = &src.transy_func;
          else if (translate_type == 3) tf = &src.transz_func;
          if (tf) {
            fill_transform(*tf, false, dst.translation);
          }
        }
      }
    } else {
      model.lods[li].part_animations = nullptr;
    }
  }

  // Collision model: from the poly_collision_lod workspace (defaults to 0).
  // BPLN/BVOL collision volumes come from LOD0 (-colonly meshes live there).
  {
    const int requested_collision_lod = poly_collision_lod;
    size_t coll_idx = static_cast<size_t>(std::clamp(
        requested_collision_lod, 0, static_cast<int>(workspaces.size()) - 1));
    const LodBucketWorkspace *lod0_ws =
        (coll_idx != 0 && !workspaces.empty()) ? workspaces[0] : nullptr;
    build_collision_model(*workspaces[coll_idx], lod0_ws, materials, model);
  }

  // Occlusion: from LOD 0 only.
  build_occlusion(ws0, model);

  out_model = model;
  std::memset(&model, 0, sizeof(model));
  return true;
}

static bool export_3di_multi_impl(
    const std::vector<const LodBucketWorkspace *> &workspaces,
    const MaterialTable *materials,
    const std::vector<float> &thresholds,
    int poly_collision_lod,
    const std::string &output_path,
    const Export3diOptions &options,
    std::string &error) {
  Export3diOptions build_options = options;
  if (build_options.model_name.empty()) {
    build_options.model_name = std::filesystem::path(output_path).stem().string();
  }

  ThreediModel model{};
  if (!build_3di_model_multi_impl(workspaces,
                                  materials,
                                  thresholds,
                                  poly_collision_lod,
                                  build_options,
                                  model,
                                  error)) {
    return false;
  }

  if (threedi_model_write(output_path.c_str(), &model) != 0) {
    error = "threedi_model_write failed";
    threedi_model_free(&model);
    return false;
  }

  threedi_model_free(&model);
  return true;
}

// Single-workspace convenience: wraps export_3di_multi_impl with one LOD.
static bool export_3di_impl(const LodBucketWorkspace &workspace,
                            const MaterialTable *materials,
                            const std::string &output_path,
                            std::string &error) {
  std::vector<const LodBucketWorkspace *> workspaces = {&workspace};
  std::vector<float> thresholds = {0.0f};
  const Export3diOptions options{};
  return export_3di_multi_impl(workspaces, materials, thresholds,
                               /*poly_collision_lod=*/0,
                               output_path,
                               options,
                               error);
}

bool export_3di(const LodBucketWorkspace &workspace,
                const std::string &output_path,
                std::string &error) {
  return export_3di_impl(workspace, nullptr, output_path, error);
}

bool export_3di(const InternalState &state,
                const std::string &output_path,
                std::string &error) {
  return export_3di_impl(state.workspace, &state.material_table, output_path, error);
}

bool export_3di(const LodWorkSlot &workspace,
                const std::string &output_path,
                std::string &error) {
  return export_3di_impl(workspace.work, nullptr, output_path, error);
}

bool export_3di(const std::vector<const LodBucketWorkspace *> &workspaces,
                const MaterialTable *materials,
                const std::vector<float> &thresholds,
                int poly_collision_lod,
                const std::string &output_path,
                const Export3diOptions &options,
                std::string &error) {
  return export_3di_multi_impl(workspaces, materials, thresholds,
                               poly_collision_lod,
                               output_path,
                               options,
                               error);
}

bool build_3di_model(const LodBucketWorkspace &workspace,
                     const MaterialTable *materials,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error) {
  std::vector<const LodBucketWorkspace *> workspaces = {&workspace};
  std::vector<float> thresholds = {0.0f};
  return build_3di_model_multi_impl(workspaces,
                                    materials,
                                    thresholds,
                                    /*poly_collision_lod=*/0,
                                    options,
                                    out_model,
                                    error);
}

bool build_3di_model(const InternalState &state,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error) {
  return build_3di_model(state.workspace,
                         &state.material_table,
                         options,
                         out_model,
                         error);
}

bool build_3di_model(const std::vector<const LodBucketWorkspace *> &workspaces,
                     const MaterialTable *materials,
                     const std::vector<float> &thresholds,
                     int poly_collision_lod,
                     const Export3diOptions &options,
                     ThreediModel &out_model,
                     std::string &error) {
  return build_3di_model_multi_impl(workspaces,
                                    materials,
                                    thresholds,
                                    poly_collision_lod,
                                    options,
                                    out_model,
                                    error);
}

bool export_3di(const std::vector<const LodBucketWorkspace *> &workspaces,
                const MaterialTable *materials,
                const std::vector<float> &thresholds,
                const std::string &output_path,
                std::string &error) {
  const Export3diOptions options{};
  return export_3di_multi_impl(workspaces, materials, thresholds,
                               /*poly_collision_lod=*/0,
                               output_path,
                               options,
                               error);
}

}  // namespace oed
