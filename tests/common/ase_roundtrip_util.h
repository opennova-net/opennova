#pragma once

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "ase/ase_parser.h"
#include "test_paths.h"

namespace ase_test {

inline bool nearly_equal(float a, float b, float eps = 1e-5f) {
  return std::fabs(a - b) <= eps;
}

inline bool eq_str(const char* a, const char* b) {
  if (!a && !b) return true;
  if (!a || !b) return false;
  return std::strcmp(a, b) == 0;
}

inline bool compare_uv(const ase::UV& a, const ase::UV& b) {
  return nearly_equal(a.u, b.u) && nearly_equal(a.v, b.v) && nearly_equal(a.w, b.w);
}

inline bool compare_mapping_channel(const ase::MappingChannel& a,
                                    const ase::MappingChannel& b) {
  if (a.channel_id != b.channel_id || a.tv_count != b.tv_count ||
      a.face_count != b.face_count) {
    return false;
  }
  for (int i = 0; i < a.tv_count; ++i) {
    if (!compare_uv(a.tverts[i], b.tverts[i])) return false;
  }
  for (int i = 0; i < a.face_count; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (a.faces[i][j] != b.faces[i][j]) return false;
    }
  }
  return true;
}

inline bool compare_weight(const ase::Weight& a, const ase::Weight& b) {
  for (int i = 0; i < 4; ++i) {
    if (a.bone_index[i] != b.bone_index[i]) return false;
    if (!nearly_equal(a.weight[i], b.weight[i])) return false;
  }
  return true;
}

inline bool compare_face(const ase::Face& a, const ase::Face& b) {
  if (a.material_id != b.material_id ||
      a.smoothing_mask != b.smoothing_mask) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (a.vert[i] != b.vert[i]) return false;
    if (a.uv[i] != b.uv[i]) return false;
    if (a.edge_visibility[i] != b.edge_visibility[i]) return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (a.color[i] != b.color[i]) return false;
  }
  return true;
}

inline bool compare_object(const ase::Object& a, const ase::Object& b, std::string& why) {
  auto fail = [&](const std::string& msg) {
    why = msg;
    return false;
  };
  if (!eq_str(a.name, b.name)) return fail("object name mismatch");
  if (!eq_str(a.parent_name, b.parent_name)) return fail("parent name mismatch");
  if (a.node_id != b.node_id) return fail("node_id mismatch");
  if (a.material_ref != b.material_ref) return fail("material_ref mismatch");
  if (a.vert_count != b.vert_count) return fail("vert_count mismatch");
  if (a.weight_count != b.weight_count) return fail("weight_count mismatch");
  if (a.uv_count != b.uv_count) return fail("uv_count mismatch");
  if (a.face_count != b.face_count) return fail("face_count mismatch");
  if (a.color_count != b.color_count) return fail("color_count mismatch");
  if (a.skinned != b.skinned) return fail("skinned flag mismatch");

  if (a.vert_count && (!a.verts || !b.verts)) return fail("verts null");
  for (int i = 0; i < a.vert_count * 3; ++i) {
    if (!nearly_equal(a.verts[i], b.verts[i])) return fail("verts differ at " + std::to_string(i));
  }
  if (a.weight_count && (!a.weights || !b.weights)) return fail("weights null");
  for (int i = 0; i < a.weight_count; ++i) {
    if (!compare_weight(a.weights[i], b.weights[i])) return fail("weights differ at " + std::to_string(i));
  }
  if (a.uv_count && (!a.uvs || !b.uvs)) return fail("uvs null");
  for (int i = 0; i < a.uv_count; ++i) {
    if (!compare_uv(a.uvs[i], b.uvs[i])) return fail("uvs differ at " + std::to_string(i));
  }
  if (a.face_count && (!a.faces || !b.faces)) return fail("faces null");
  for (int i = 0; i < a.face_count; ++i) {
    if (!compare_face(a.faces[i], b.faces[i])) return fail("faces differ at " + std::to_string(i));
  }
  if (a.color_count && (!a.colors || !b.colors)) return fail("colors null");
  for (int i = 0; i < a.color_count; ++i) {
    if (a.colors[i] != b.colors[i]) return fail("colors differ at " + std::to_string(i));
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (!nearly_equal(a.tm_row[r][c], b.tm_row[r][c])) return fail("tm_row differ");
    }
  }
  return true;
}

inline bool compare_light(const ase::Light& a, const ase::Light& b) {
  if (!eq_str(a.name, b.name) || a.type != b.type) return false;
  for (int i = 0; i < 3; ++i) {
    if (!nearly_equal(a.pos[i], b.pos[i])) return false;
    if (!nearly_equal(a.color[i], b.color[i])) return false;
    if (!nearly_equal(a.tm_row2[i], b.tm_row2[i])) return false;
  }
  return nearly_equal(a.intensity, b.intensity) &&
         nearly_equal(a.atten_start, b.atten_start) &&
         nearly_equal(a.atten_end, b.atten_end) &&
         nearly_equal(a.near_atten_start, b.near_atten_start) &&
         nearly_equal(a.near_atten_end, b.near_atten_end) &&
         nearly_equal(a.hotspot, b.hotspot) &&
         nearly_equal(a.falloff, b.falloff);
}

inline bool compare_material(const ase::Material& a, const ase::Material& b) {
  if (a.flags != b.flags || a.extra_flags != b.extra_flags ||
      a.has_submaterials != b.has_submaterials) {
    return false;
  }
  if (!eq_str(a.name, b.name)) return false;
  for (int i = 0; i < 4; ++i) {
    if (!eq_str(a.maps[i], b.maps[i])) return false;
  }
  for (int i = 0; i < 2; ++i) {
    if (!nearly_equal(a.uv_u_offset[i], b.uv_u_offset[i])) return false;
    if (!nearly_equal(a.uv_v_offset[i], b.uv_v_offset[i])) return false;
    if (!nearly_equal(a.uv_u_tiling[i], b.uv_u_tiling[i])) return false;
    if (!nearly_equal(a.uv_v_tiling[i], b.uv_v_tiling[i])) return false;
  }
  return true;
}

inline bool compare_document(const ase::Document& a,
                             const ase::Document& b,
                             std::string& why) {
  if (a.object_count != b.object_count) { why = "object count"; return false; }
  if (a.light_count != b.light_count) { why = "light count"; return false; }
  if (a.material_count != b.material_count) { why = "material count"; return false; }
  if (a.flags != b.flags) { why = "flags"; return false; }
  if (a.skinned_flags != b.skinned_flags) { why = "skinned_flags"; return false; }

  for (int i = 0; i < a.material_count; ++i) {
    if (!compare_material(a.materials[i], b.materials[i])) { why = "material " + std::to_string(i); return false; }
  }
  for (int i = 0; i < a.light_count; ++i) {
    if (!compare_light(a.lights[i], b.lights[i])) { why = "light " + std::to_string(i); return false; }
  }
  for (int i = 0; i < a.object_count; ++i) {
    std::string objWhy;
    if (!compare_object(a.objects[i], b.objects[i], objWhy)) {
      why = "object " + std::to_string(i) + ": " + objWhy;
      return false;
    }
  }
  return true;
}

}  // namespace ase_test
