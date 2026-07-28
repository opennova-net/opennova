#include "ase/ase_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#include <io/strutil.h>
#include <vector>

namespace ase {

namespace {

// Helpers --------------------------------------------------------------------

// Match the game engine's float parsing: atof() returns double, then fstp dword
// truncates to float.  This double-rounding (string→double→float) can differ from
// strtof's single-rounding (string→float) by 1 ULP on boundary values.
static inline float parse_float(const char* s) {
  return static_cast<float>(std::strtod(s, nullptr));
}

using opennova::strutil::iequals;

// Tokenize a line similarly to AseTokenizeLine: splits on space/comma/tab,
// respects quotes, trims comments starting with ';' or '//'.
static std::vector<std::string> tokenize(const std::string& line_in) {
  std::vector<std::string> out;
  if (line_in.empty()) return out;

  char buf[1024]{};
  size_t n = std::min<size_t>(line_in.size(), sizeof(buf) - 1);
  for (size_t i = 0; i < n; ++i) {
    char c = line_in[i];
    if (c == '\r') c = '\0';
    buf[i] = c;
  }
  buf[n] = '\0';

  bool in_quote = false;
  for (size_t i = 0; buf[i]; ++i) {
    if (!in_quote && buf[i] == ';') {
      buf[i] = '\0';
      break;
    }
    if (!in_quote && buf[i] == '/' && buf[i + 1] == '/') {
      buf[i] = '\0';
      break;
    }
    if (buf[i] == '"') {
      in_quote = !in_quote;
      buf[i] = '\0';
    } else if (!in_quote && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == ',')) {
      buf[i] = '\0';
    }
  }

  for (size_t i = 0; i < sizeof(buf) && out.size() < 30;) {
    while (i < sizeof(buf) && buf[i] == '\0') {
      ++i;
    }
    if (i >= sizeof(buf) || !buf[i]) break;
    out.emplace_back(&buf[i]);
    while (i < sizeof(buf) && buf[i]) ++i;
  }
  return out;
}

struct MaterialSlot {
  Material mats[64]{};
  char name[32]{};
  bool top_defined = false;
  bool top_used = false;
  bool top_present = false;
  bool has_sub_declared = false;
  int top_flat_index = -1;
  int sub_flat_index[64]{};
};

// Parser state mirroring the legacy globals.
struct ParserState {
  Document* doc = nullptr;
  int scope_depth = 0;
  int count_objects = 0;
  int count_lights = 0;
  int count_materials = 0;
  int max_material_slot = 0;
  bool parentless_weight_warn = true;
  bool submaterial_overflow_warn = true;

  // Current context
  int root_section = 0;
  int block_tag = 0;
  int cur_normal_face = -1;     // face index from last *MESH_FACENORMAL
  int cur_normal_vert = 0;      // 0..2 counter within current face
  int sub_tag = 0;
  int map_tag = 0;
  int nested_map_tag = 0;
  int current_material_ref = 0;
  int current_sub_material = 0;
  int current_map_subno = 0;
  Object* cur_obj = nullptr;
  Light* cur_light = nullptr;
  bool light_tm_matches = false;

  std::vector<MaterialSlot> material_table = std::vector<MaterialSlot>(128);
};

static inline MaterialSlot& ensure_slot(ParserState& st, int idx) {
  if (idx < 0) idx = 0;
  if (idx >= static_cast<int>(st.material_table.size())) {
    st.material_table.resize(idx + 1);
  }
  return st.material_table[idx];
}

static void update_scope_depth(ParserState& st, const std::vector<std::string>& toks) {
  for (const auto& t : toks) {
    if (iequals(t, "{")) ++st.scope_depth;
    else if (iequals(t, "}")) --st.scope_depth;
  }
}

// Coordinate swizzle used by the legacy importer: x' = -y, y' = x, z' = z.
static inline void swizzle_vec3(float in_x, float in_y, float in_z, float out[3]) {
  out[0] = -in_y;
  out[1] = in_x;
  out[2] = in_z;
}

// Pass 1: count top-level sections to size allocations.
static bool count_pass(ParserState& st, const std::vector<std::string>& toks) {
  if (toks.empty()) {
    update_scope_depth(st, toks);
    return true;
  }
  if (st.scope_depth == 0) {
    st.root_section = 0;
    st.block_tag = 0;
    st.sub_tag = 0;
    st.map_tag = 0;
    st.nested_map_tag = 0;
    const auto& tag = toks[0];
    if (iequals(tag, "*GEOMOBJECT")) {
      st.root_section = 1;
      ++st.count_objects;
      ++st.count_materials;
    } else if (iequals(tag, "*MATERIAL_LIST")) {
      st.root_section = 4;
      ++st.count_materials;
    } else if (iequals(tag, "*LIGHTOBJECT")) {
      st.root_section = 16;
      ++st.count_lights;
    } else if (!tag.empty() && tag[0] == '*') {
      st.root_section = 0;
      ++st.count_materials;
    }
  }
  update_scope_depth(st, toks);
  return true;
}

// Apply material flattening and face material_index remap.
static void finalize_materials(ParserState& st) {
  st.doc->material_count = 0;
  int slot_limit = std::max(st.max_material_slot, static_cast<int>(st.material_table.size()) - 1);
  for (int slot = 0; slot <= slot_limit; ++slot) {
    auto& slotRec = ensure_slot(st, slot);
    if (slotRec.top_present && slotRec.name[0] && slotRec.mats[0].name[0] == '\0') {
      std::strncpy(slotRec.mats[0].name, slotRec.name, sizeof(slotRec.mats[0].name) - 1);
    }
    if (slotRec.top_present) {
      slotRec.top_flat_index = st.doc->material_count;
      st.doc->material_count++;
    }
    for (int j = 0; j < 64; ++j) {
      if (slotRec.mats[j].has_submaterials) {
        slotRec.sub_flat_index[j] = st.doc->material_count;
        ++st.doc->material_count;
      }
    }
  }
  if (st.doc->material_count > 0) {
    st.doc->materials = new Material[st.doc->material_count]{};
  }
  int flat_idx = 0;
  for (int slot = 0; slot <= slot_limit; ++slot) {
    auto& slotRec = ensure_slot(st, slot);
    if (slotRec.top_present) {
      Material top = slotRec.mats[0];
      if (slotRec.name[0]) {
        std::memset(top.name, 0, sizeof(top.name));
        std::strncpy(top.name, slotRec.name, sizeof(top.name) - 1);
      }
      st.doc->materials[flat_idx++] = top;
    }
    for (int j = 0; j < 64; ++j) {
      if (slotRec.mats[j].has_submaterials) {
        st.doc->materials[flat_idx++] = slotRec.mats[j];
      }
    }
  }
  // Remap face material_index.
  for (int i = 0; i < st.doc->object_count; ++i) {
    auto& obj = st.doc->objects[i];
    for (int f = 0; f < obj.face_count; ++f) {
      int mat_id = obj.faces[f].material_id;
      int mat_ref = obj.material_ref;
      auto& slotRec = ensure_slot(st, mat_ref);
      if (mat_id >= 0 && mat_id < 64 && slotRec.mats[mat_id].has_submaterials) {
        obj.faces[f].material_index = slotRec.sub_flat_index[mat_id];
      } else {
        obj.faces[f].material_index = slotRec.top_flat_index;
      }
    }
  }
}

// Depth 3 helpers -----------------------------------------------------------

static void handle_mesh_vertex(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 5) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->vert_count) return;
  float x = parse_float(t[2].c_str());
  float y = parse_float(t[3].c_str());
  float z = parse_float(t[4].c_str());
  float swz[3];
  swizzle_vec3(x, y, z, swz);
  float* vptr = &st.cur_obj->verts[idx * 3];
  vptr[0] = swz[0];
  vptr[1] = swz[1];
  vptr[2] = swz[2];
}

static void handle_mesh_tvert(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 5) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->uv_count) return;
  st.cur_obj->uvs[idx].u = parse_float(t[2].c_str());
  st.cur_obj->uvs[idx].v = parse_float(t[3].c_str());
  st.cur_obj->uvs[idx].w = parse_float(t[4].c_str());
}

static void handle_mesh_face(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 11) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->face_count) return;
  Face& f = st.cur_obj->faces[idx];
  f.vert[0] = std::strtol(t[3].c_str(), nullptr, 10);
  f.vert[1] = std::strtol(t[5].c_str(), nullptr, 10);
  f.vert[2] = std::strtol(t[7].c_str(), nullptr, 10);
  f.vert[3] = f.vert[0];
  f.edge_visibility[0] = static_cast<uint8_t>(std::strtol(t[9].c_str(), nullptr, 10));
  f.edge_visibility[1] = static_cast<uint8_t>(std::strtol(t[11].c_str(), nullptr, 10));
  f.edge_visibility[2] = static_cast<uint8_t>(std::strtol(t[13].c_str(), nullptr, 10));
  f.edge_visibility[3] = f.edge_visibility[0];
  for (size_t i = 0; i < t.size(); ++i) {
    if (iequals(t[i], "*MESH_SMOOTHING")) {
      for (size_t j = i + 1; j < t.size(); ++j) {
        if (!t[j].empty() && t[j][0] == '*') break;
        int sm = std::strtol(t[j].c_str(), nullptr, 10);
        // ASE smoothing groups are 1-indexed (1..32)
        if (sm > 0 && sm <= 32) f.smoothing_mask |= (1u << (sm - 1));
      }
      break;
    }
  }
  f.material_id = std::strtol(t.back().c_str(), nullptr, 10);
}

static void handle_mesh_tface(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 5) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->face_count) return;
  Face& f = st.cur_obj->faces[idx];
  f.uv[0] = std::strtol(t[2].c_str(), nullptr, 10);
  f.uv[1] = std::strtol(t[3].c_str(), nullptr, 10);
  f.uv[2] = std::strtol(t[4].c_str(), nullptr, 10);
  f.uv[3] = f.uv[0];
}

static void handle_mesh_vertcol(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 5) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->color_count) return;
  uint32_t r = static_cast<uint32_t>(parse_float(t[2].c_str()) * 255.0f);
  uint32_t g = static_cast<uint32_t>(parse_float(t[3].c_str()) * 255.0f);
  uint32_t b = static_cast<uint32_t>(parse_float(t[4].c_str()) * 255.0f);
  st.cur_obj->colors[idx] = (r << 16) | (g << 8) | b;
}

static void handle_mesh_cface(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 5) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->face_count) return;
  Face& f = st.cur_obj->faces[idx];
  f.color[0] = std::strtol(t[2].c_str(), nullptr, 10);
  f.color[1] = std::strtol(t[3].c_str(), nullptr, 10);
  f.color[2] = std::strtol(t[4].c_str(), nullptr, 10);
}

static void handle_mesh_weight(ParserState& st, const std::vector<std::string>& t) {
  if (!st.cur_obj || t.size() < 10) return;
  int idx = std::strtol(t[1].c_str(), nullptr, 10);
  if (idx < 0 || idx >= st.cur_obj->weight_count) return;
  Weight& w = st.cur_obj->weights[idx];
  for (int i = 0; i < 4; ++i) {
    w.bone_index[i] = std::strtol(t[2 + i].c_str(), nullptr, 10);
  }
  for (int i = 0; i < 4; ++i) {
    w.weight[i] = parse_float(t[6 + i].c_str());
  }
}

static void handle_tm_row(float in_x, float in_y, float in_z, float row[3]) {
  swizzle_vec3(in_x, in_y, in_z, row);
}

static char* basename_inplace(char* s) {
  size_t len = std::strlen(s);
  for (size_t i = len; i > 0; --i) {
    if (s[i - 1] == '\\' || s[i - 1] == '/') {
      return &s[i];
    }
  }
  return s;
}

static void store_bitmap(MaterialSlot& slot, int sub, const std::string& path, int map_index) {
  if (sub < 0 || sub >= 64) return;
  Material& m = slot.mats[sub];
  char buf[260]{};
  std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
  char* base = basename_inplace(buf);
  std::strncpy(m.maps[map_index], base, sizeof(m.maps[map_index]) - 1);
}

static inline int current_uv_channel(const ParserState& st) {
  if (st.map_tag == 11) return 0;
  if (st.map_tag == 12) {
    return (st.current_map_subno < 2) ? st.current_map_subno : -1;
  }
  return -1;
}

// Main pass2 parser ----------------------------------------------------------

static void parse_node(ParserState& st, const std::vector<std::string>& t) {
  if (t.empty()) {
    update_scope_depth(st, t);
    return;
  }

  switch (st.scope_depth) {
    case 0: {
      st.root_section = 0;
      st.block_tag = st.sub_tag = st.map_tag = st.nested_map_tag = 0;
      const auto& tag = t[0];
      if (iequals(tag, "*GEOMOBJECT")) {
        st.root_section = 1;
        st.cur_obj = &st.doc->objects[st.count_objects++];
        st.cur_obj->node_id = -1;
      } else if (iequals(tag, "*MATERIAL_LIST")) {
        st.root_section = 4;
      } else if (iequals(tag, "*LIGHTOBJECT")) {
        st.root_section = 16;
        st.cur_light = &st.doc->lights[st.count_lights++];
      }
      break;
    }
    case 1: {
      st.block_tag = st.sub_tag = st.map_tag = st.nested_map_tag = 0;
      if (st.root_section == 1 && !t.empty()) {
        if (iequals(t[0], "*NODE_NAME") && t.size() > 1) {
          std::strncpy(st.cur_obj->name, t[1].c_str(), sizeof(st.cur_obj->name) - 1);
        } else if (iequals(t[0], "*NODE_PARENT") && t.size() > 1) {
          std::strncpy(st.cur_obj->parent_name, t[1].c_str(), sizeof(st.cur_obj->parent_name) - 1);
        } else if (iequals(t[0], "*NODE_BONENUMBER") && t.size() > 1) {
          st.cur_obj->node_id = std::strtol(t[1].c_str(), nullptr, 10);
        } else if (iequals(t[0], "*MATERIAL_REF") && t.size() > 1) {
          st.cur_obj->material_ref = std::strtol(t[1].c_str(), nullptr, 10);
        } else if (iequals(t[0], "*MESH")) {
          st.block_tag = 3;
        } else if (iequals(t[0], "*NODE_TM")) {
          st.block_tag = 2;
        }
      } else if (st.root_section == 4) {
        if (iequals(t[0], "*MATERIAL") && t.size() > 1) {
          st.block_tag = 9;
          st.current_material_ref = std::strtol(t[1].c_str(), nullptr, 10);
          if (st.current_material_ref > st.max_material_slot) {
            st.max_material_slot = st.current_material_ref;
          }
          st.current_sub_material = 0;
          ensure_slot(st, st.current_material_ref).top_defined = true;
        }
      } else if (st.root_section == 16) {
        if (iequals(t[0], "*NODE_NAME") && t.size() > 1) {
          std::strncpy(st.cur_light->name, t[1].c_str(), sizeof(st.cur_light->name) - 1);
        } else if (iequals(t[0], "*LIGHT_TYPE") && t.size() > 1) {
          st.cur_light->type = iequals(t[1], "Omni") ? 0 : 1;
        } else if (iequals(t[0], "*LIGHT_SETTINGS")) {
          st.block_tag = 17;
        } else if (iequals(t[0], "*NODE_TM")) {
          st.block_tag = 18;
          st.light_tm_matches = false;
        }
      }
      break;
    }
    case 2: {
      if (st.root_section == 1 && iequals(t[0], "*MESH")) {
        st.block_tag = 3;
      } else if (st.root_section == 4 && iequals(t[0], "*MATERIAL") && t.size() > 1) {
        st.block_tag = 9;
        st.current_material_ref = std::strtol(t[1].c_str(), nullptr, 10);
        if (st.current_material_ref > st.max_material_slot) {
          st.max_material_slot = st.current_material_ref;
        }
        st.current_sub_material = 0;
        ensure_slot(st, st.current_material_ref).top_defined = true;
      }
      st.sub_tag = 0;
      st.map_tag = st.nested_map_tag = 0;
      if (st.block_tag == 9) {
        MaterialSlot& slot = ensure_slot(st, st.current_material_ref);
        Material& mat = slot.mats[st.current_sub_material];
        if (iequals(t[0], "*MATERIAL_NAME") && t.size() > 1) {
          if (st.current_sub_material == 0) {
            std::strncpy(slot.name, t[1].c_str(), sizeof(slot.name) - 1);
          } else {
            std::strncpy(mat.name, t[1].c_str(), sizeof(mat.name) - 1);
          }
          mat.uv_u_offset[0] = mat.uv_v_offset[0] = 0.0f;
          mat.uv_u_tiling[0] = mat.uv_v_tiling[0] = 1.0f;
        } else if (iequals(t[0], "*MAP_DIFFUSE")) {
          st.sub_tag = 11;
          st.map_tag = 11;
        } else if (iequals(t[0], "*MAP_OPACITY")) {
          st.sub_tag = 15;
          st.map_tag = 15;
        } else if (iequals(t[0], "*MAP_BUMP")) {
          st.sub_tag = 0;
          st.map_tag = 0;
          st.current_map_subno = 0;
        } else if (iequals(t[0], "*SUBMATERIAL") && t.size() > 1) {
          st.sub_tag = 10;
          st.current_sub_material = std::strtol(t[1].c_str(), nullptr, 10);
          ensure_slot(st, st.current_material_ref).has_sub_declared = true;
          if (st.current_sub_material >= 64 && st.submaterial_overflow_warn) {
            st.submaterial_overflow_warn = false;
          }
        } else if (iequals(t[0], "*MATERIAL_TWOSIDED")) {
          mat.extra_flags |= 1u;
        }
      } else if (st.block_tag == 3) {
        if (iequals(t[0], "*MESH_NUMVERTEX") && t.size() > 1) {
          st.cur_obj->vert_count = std::strtol(t[1].c_str(), nullptr, 10);
          st.cur_obj->verts = new float[st.cur_obj->vert_count * 3]();
          st.cur_obj->weights = new Weight[st.cur_obj->vert_count]();
          st.cur_obj->weight_count = st.cur_obj->vert_count;
        } else if (iequals(t[0], "*MESH_NUMTVERTEX") && t.size() > 1) {
          st.cur_obj->uv_count = std::strtol(t[1].c_str(), nullptr, 10);
          st.cur_obj->uvs = new UV[st.cur_obj->uv_count]();
        } else if (iequals(t[0], "*MESH_NUMFACES") && t.size() > 1) {
          st.cur_obj->face_count = std::strtol(t[1].c_str(), nullptr, 10);
          st.cur_obj->faces = new Face[st.cur_obj->face_count]();
        } else if (iequals(t[0], "*MESH_NUMCVERTEX") && t.size() > 1) {
          st.cur_obj->color_count = std::strtol(t[1].c_str(), nullptr, 10);
          st.cur_obj->colors = new uint32_t[st.cur_obj->color_count]();
        } else if (iequals(t[0], "*MESH_NUMCVFACES") && t.size() > 1) {
          st.cur_obj->color_face_count = std::strtol(t[1].c_str(), nullptr, 10);
        } else if (iequals(t[0], "*MESH_VERTEX_LIST")) {
          st.sub_tag = 5;
        } else if (iequals(t[0], "*MESH_FACE_LIST")) {
          st.sub_tag = 6;
        } else if (iequals(t[0], "*MESH_TVERTLIST")) {
          st.sub_tag = 7;
        } else if (iequals(t[0], "*MESH_TFACELIST")) {
          st.sub_tag = 8;
        } else if (iequals(t[0], "*MESH_CVERTLIST")) {
          st.sub_tag = 13;
        } else if (iequals(t[0], "*MESH_CFACELIST")) {
          st.sub_tag = 14;
        } else if (iequals(t[0], "*MESH_WEIGHTS")) {
          st.sub_tag = 19;
          st.cur_obj->skinned = 1;
        } else if (iequals(t[0], "*MESH_NORMALS")) {
          st.sub_tag = 20;
          st.cur_normal_face = -1;
          st.cur_normal_vert = 0;
          // Allocate face_normals if not already allocated
          if (st.cur_obj->face_count > 0 && !st.cur_obj->face_normals) {
            st.cur_obj->face_normal_count = st.cur_obj->face_count;
            st.cur_obj->face_normals = new float[static_cast<size_t>(st.cur_obj->face_count) * 9]();
          }
        }
      } else if (st.block_tag == 17) {
        if (iequals(t[0], "*LIGHT_COLOR") && t.size() > 3) {
          st.cur_light->color[0] = parse_float(t[1].c_str());
          st.cur_light->color[1] = parse_float(t[2].c_str());
          st.cur_light->color[2] = parse_float(t[3].c_str());
        } else if (iequals(t[0], "*LIGHT_INTENS") && t.size() > 1) {
          st.cur_light->intensity = parse_float(t[1].c_str());
        } else if ((iequals(t[0], "*LIGHT_FAR_ATTNSTART") || iequals(t[0], "*LIGHT_ATTNSTART")) && t.size() > 1) {
          st.cur_light->atten_start = parse_float(t[1].c_str());
        } else if ((iequals(t[0], "*LIGHT_FAR_ATTNEND") || iequals(t[0], "*LIGHT_ATTNEND")) && t.size() > 1) {
          st.cur_light->atten_end = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*LIGHT_NEAR_ATTNSTART") && t.size() > 1) {
          st.cur_light->near_atten_start = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*LIGHT_NEAR_ATTNEND") && t.size() > 1) {
          st.cur_light->near_atten_end = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*LIGHT_HOTSPOT") && t.size() > 1) {
          st.cur_light->hotspot = 0.5f * parse_float(t[1].c_str());
        } else if (iequals(t[0], "*LIGHT_FALLOFF") && t.size() > 1) {
          st.cur_light->falloff = 0.5f * parse_float(t[1].c_str());
        }
      } else if (st.block_tag == 2 || st.block_tag == 18) {
        if (st.block_tag == 18) {
          if (iequals(t[0], "*NODE_NAME") && t.size() > 1) {
            st.light_tm_matches = st.cur_light && iequals(st.cur_light->name, t[1]);
          }
          if (!st.light_tm_matches) break;
          if (iequals(t[0], "*TM_ROW2") && t.size() > 3) {
            float x = parse_float(t[1].c_str());
            float y = parse_float(t[2].c_str());
            float z = parse_float(t[3].c_str());
            st.cur_light->tm_row2[0] = y;
            st.cur_light->tm_row2[1] = -x;
            st.cur_light->tm_row2[2] = -z;
          } else if (iequals(t[0], "*TM_POS") && t.size() > 3) {
            float x = parse_float(t[1].c_str());
            float y = parse_float(t[2].c_str());
            float z = parse_float(t[3].c_str());
            st.cur_light->pos[0] = -y;
            st.cur_light->pos[1] = x;
            st.cur_light->pos[2] = z;
          }
        } else {
          if (!st.cur_obj) break;
          if (iequals(t[0], "*TM_ROW0") && t.size() > 3) {
            handle_tm_row(parse_float(t[1].c_str()),
                          parse_float(t[2].c_str()),
                          parse_float(t[3].c_str()),
                          st.cur_obj->tm_row[1]);
          } else if (iequals(t[0], "*TM_ROW1") && t.size() > 3) {
            float x = parse_float(t[1].c_str());
            float y = parse_float(t[2].c_str());
            float z = parse_float(t[3].c_str());
            st.cur_obj->tm_row[0][0] = y;
            st.cur_obj->tm_row[0][1] = -x;
            st.cur_obj->tm_row[0][2] = -z;
          } else if (iequals(t[0], "*TM_ROW2") && t.size() > 3) {
            handle_tm_row(parse_float(t[1].c_str()),
                          parse_float(t[2].c_str()),
                          parse_float(t[3].c_str()),
                          st.cur_obj->tm_row[2]);
          } else if (iequals(t[0], "*TM_ROW3") && t.size() > 3) {
            handle_tm_row(parse_float(t[1].c_str()),
                          parse_float(t[2].c_str()),
                          parse_float(t[3].c_str()),
                          st.cur_obj->tm_row[3]);
          }
        }
      }
      break;
    }
    case 3: {
      if (iequals(t[0], "*MAP_DIFFUSE")) {
        st.sub_tag = 11;
        st.map_tag = 11;
        st.current_map_subno = 0;
      } else if (iequals(t[0], "*MAP_OPACITY")) {
        st.sub_tag = 15;
        st.map_tag = 15;
        st.current_map_subno = 0;
      } else if (iequals(t[0], "*MAP_BUMP")) {
        st.sub_tag = 0;
        st.map_tag = 0;
        st.current_map_subno = 0;
      } else if (iequals(t[0], "*MAP_CLASS") && t.size() > 1 &&
                 (iequals(t[1], "RGB Multiply") || iequals(t[1], "RGB"))) {
        st.map_tag = 12;
        ensure_slot(st, st.current_material_ref).mats[st.current_sub_material].flags |= 2u;
      }
      if (st.block_tag == 3) {
        if (st.sub_tag == 5 && iequals(t[0], "*MESH_VERTEX")) {
          handle_mesh_vertex(st, t);
        } else if (st.sub_tag == 7 && iequals(t[0], "*MESH_TVERT")) {
          handle_mesh_tvert(st, t);
        } else if (st.sub_tag == 6 && iequals(t[0], "*MESH_FACE")) {
          handle_mesh_face(st, t);
        } else if (st.sub_tag == 8 && iequals(t[0], "*MESH_TFACE")) {
          handle_mesh_tface(st, t);
        } else if (st.sub_tag == 13 && iequals(t[0], "*MESH_VERTCOL")) {
          handle_mesh_vertcol(st, t);
        } else if (st.sub_tag == 14 && iequals(t[0], "*MESH_CFACE")) {
          handle_mesh_cface(st, t);
        } else if (st.sub_tag == 19 && iequals(t[0], "*MESH_WEIGHTSVERTEX")) {
          handle_mesh_weight(st, t);
        } else if (st.sub_tag == 20 && iequals(t[0], "*MESH_FACENORMAL") && t.size() > 1) {
          st.cur_normal_face = std::strtol(t[1].c_str(), nullptr, 10);
          st.cur_normal_vert = 0;
        } else if (st.sub_tag == 20 && iequals(t[0], "*MESH_VERTEXNORMAL") && t.size() > 4) {
          if (st.cur_obj && st.cur_obj->face_normals &&
              st.cur_normal_face >= 0 && st.cur_normal_face < st.cur_obj->face_count &&
              st.cur_normal_vert < 3) {
            float nx = parse_float(t[2].c_str());
            float ny = parse_float(t[3].c_str());
            float nz = parse_float(t[4].c_str());
            float swz[3];
            swizzle_vec3(nx, ny, nz, swz);
            float* dst = &st.cur_obj->face_normals[
                static_cast<size_t>(st.cur_normal_face) * 9 +
                static_cast<size_t>(st.cur_normal_vert) * 3];
            dst[0] = swz[0];
            dst[1] = swz[1];
            dst[2] = swz[2];
            ++st.cur_normal_vert;
          }
        } else if (st.sub_tag == 11 && iequals(t[0], "*BITMAP") && t.size() > 1) {
          auto& slot = ensure_slot(st, st.current_material_ref);
          store_bitmap(slot, st.current_sub_material, t[1], 0);
        } else if (st.sub_tag == 15 && iequals(t[0], "*BITMAP") && t.size() > 1) {
          auto& slot = ensure_slot(st, st.current_material_ref);
          store_bitmap(slot, st.current_sub_material, t[1], 2);
          if (slot.mats[st.current_sub_material].maps[0][0] == '\0') {
            store_bitmap(slot, st.current_sub_material, t[1], 0);
          }
        } else if (iequals(t[0], "*MAP_CLASS") && t.size() > 1 && iequals(t[1], "RGB")) {
          st.map_tag = 12;
        } else if (iequals(t[0], "*UVW_U_OFFSET") && t.size() > 1) {
          auto& mat = ensure_slot(st, st.current_material_ref).mats[st.current_sub_material];
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_u_offset[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_V_OFFSET") && t.size() > 1) {
          auto& mat = ensure_slot(st, st.current_material_ref).mats[st.current_sub_material];
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_v_offset[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_U_TILING") && t.size() > 1) {
          auto& mat = ensure_slot(st, st.current_material_ref).mats[st.current_sub_material];
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_u_tiling[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_V_TILING") && t.size() > 1) {
          auto& mat = ensure_slot(st, st.current_material_ref).mats[st.current_sub_material];
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_v_tiling[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*MAP_CLASS") && t.size() > 1 && iequals(t[1], "RGB Multiply")) {
          st.map_tag = 12;
        } else if (iequals(t[0], "*MAP_SUBNO") && t.size() > 1) {
          st.current_map_subno = std::strtol(t[1].c_str(), nullptr, 10);
        }
      } else if (st.block_tag == 9) {
        auto& slot = ensure_slot(st, st.current_material_ref);
        auto& mat = slot.mats[st.current_sub_material];
        if (st.sub_tag == 10 && iequals(t[0], "*MATERIAL_NAME") && t.size() > 1) {
          std::strncpy(mat.name, t[1].c_str(), sizeof(mat.name) - 1);
        } else if (st.sub_tag == 10 && iequals(t[0], "*MATERIAL_TWOSIDED")) {
          mat.extra_flags |= 1u;
        }
        if (iequals(t[0], "*BITMAP") && t.size() > 1) {
          if (st.map_tag != 0) {
            int base = (st.map_tag == 15) ? 2 : 0;
            int idx = (st.map_tag == 12) ? base + st.current_map_subno : base;
            if (idx < 4) {
              store_bitmap(slot, st.current_sub_material, t[1], idx);
            }
          }
        } else if (iequals(t[0], "*MAP_SUBNO") && t.size() > 1) {
          st.current_map_subno = std::strtol(t[1].c_str(), nullptr, 10);
        } else if (iequals(t[0], "*UVW_U_OFFSET") && t.size() > 1) {
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_u_offset[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_V_OFFSET") && t.size() > 1) {
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_v_offset[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_U_TILING") && t.size() > 1) {
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_u_tiling[idx] = parse_float(t[1].c_str());
        } else if (iequals(t[0], "*UVW_V_TILING") && t.size() > 1) {
          int idx = current_uv_channel(st);
          if (idx >= 0) mat.uv_v_tiling[idx] = parse_float(t[1].c_str());
        }
      }
      break;
    }
    case 4:
    case 5: {
      auto& slot = ensure_slot(st, st.current_material_ref);
      auto& mat = slot.mats[st.current_sub_material];
      if (iequals(t[0], "*MAP_CLASS") && t.size() > 1 &&
          (iequals(t[1], "RGB Multiply") || iequals(t[1], "RGB"))) {
        st.map_tag = 12;
        mat.flags |= 2u;
      }
      if (iequals(t[0], "*MAP_SUBNO") && t.size() > 1) {
        st.current_map_subno = std::strtol(t[1].c_str(), nullptr, 10);
      } else if (iequals(t[0], "*BITMAP") && t.size() > 1) {
        if (st.map_tag != 0) {
          int baseIndex = (st.map_tag == 15) ? 2 : 0;
          int idx = (st.map_tag == 12) ? baseIndex + st.current_map_subno : baseIndex;
          if (idx < 4) store_bitmap(slot, st.current_sub_material, t[1], idx);
        }
      } else if (iequals(t[0], "*UVW_U_OFFSET") && t.size() > 1) {
        int idx = current_uv_channel(st);
        if (idx >= 0) mat.uv_u_offset[idx] = parse_float(t[1].c_str());
      } else if (iequals(t[0], "*UVW_V_OFFSET") && t.size() > 1) {
        int idx = current_uv_channel(st);
        if (idx >= 0) mat.uv_v_offset[idx] = parse_float(t[1].c_str());
      } else if (iequals(t[0], "*UVW_U_TILING") && t.size() > 1) {
        int idx = current_uv_channel(st);
        if (idx >= 0) mat.uv_u_tiling[idx] = parse_float(t[1].c_str());
      } else if (iequals(t[0], "*UVW_V_TILING") && t.size() > 1) {
        int idx = current_uv_channel(st);
        if (idx >= 0) mat.uv_v_tiling[idx] = parse_float(t[1].c_str());
      }
      break;
    }
    default:
      break;
  }

  update_scope_depth(st, t);
}

static void free_object(Object& o) {
  delete[] o.verts;
  delete[] o.weights;
  delete[] o.uvs;
  delete[] o.faces;
  delete[] o.colors;
  delete[] o.mapping_channels;
  delete[] o.face_normals;
  o.verts = nullptr;
  o.weights = nullptr;
  o.uvs = nullptr;
  o.faces = nullptr;
  o.colors = nullptr;
  o.mapping_channels = nullptr;
  o.face_normals = nullptr;
}

}  // namespace

bool parse_file(const std::string& path, Document& out_doc, std::string& error) {
  error.clear();
  std::ifstream fin(path);
  if (!fin) {
    error = "File not found or unreadable: " + path;
    return false;
  }

  free_document(out_doc);
  ParserState st{};
  st.doc = &out_doc;

  // Pass 1: counting
  st.scope_depth = 0;
  std::string line;
  int line_no = 1;
  while (std::getline(fin, line)) {
    auto toks = tokenize(line);
    if (!count_pass(st, toks)) {
      error = "Count pass failed at line " + std::to_string(line_no);
      return false;
    }
    ++line_no;
  }
  if (st.scope_depth != 0) {
    error = "Unbalanced braces in ASE file";
    return false;
  }

  if (st.count_objects == 0) {
    error = "No objects found in ASE";
    return false;
  }

  // Alloc doc structures.
  st.doc->object_count = st.count_objects;
  st.doc->objects = new Object[st.doc->object_count]();
  st.doc->light_count = st.count_lights;
  if (st.doc->light_count > 0) st.doc->lights = new Light[st.doc->light_count]();

  // Pass 2
  fin.clear();
  fin.seekg(0, std::ios::beg);
  st.scope_depth = 0;
  st.count_objects = 0;
  st.count_lights = 0;
  line_no = 1;
  while (std::getline(fin, line)) {
    auto toks = tokenize(line);
    parse_node(st, toks);
    ++line_no;
  }

  // Mark materials present based on face material IDs.
  for (int i = 0; i < st.doc->object_count; ++i) {
    const Object& obj = st.doc->objects[i];
    for (int f = 0; f < obj.face_count; ++f) {
      int mat_id = obj.faces[f].material_id;
      int mat_ref = obj.material_ref;
      auto& slot = ensure_slot(st, mat_ref);
      if (slot.has_sub_declared && mat_id >= 0 && mat_id < 64 && slot.mats[mat_id].name[0]) {
        slot.mats[mat_id].has_submaterials = 1;
      } else {
        slot.top_used = true;
      }
    }
  }

  for (int slot_idx = 0; slot_idx <= st.max_material_slot && slot_idx < static_cast<int>(st.material_table.size()); ++slot_idx) {
    auto& slot = ensure_slot(st, slot_idx);
    slot.top_present = slot.top_used;
  }

  finalize_materials(st);

  st.doc->skinned_flags = 0;
  for (int i = 0; i < st.doc->object_count; ++i) {
    st.doc->skinned_flags |= st.doc->objects[i].skinned;
  }

  st.doc->flags |= 1u;
  return true;
}

void free_document(Document& doc) {
  if (doc.objects) {
    for (int i = 0; i < doc.object_count; ++i) free_object(doc.objects[i]);
    delete[] doc.objects;
  }
  if (doc.materials) {
    for (int i = 0; i < doc.material_count; ++i)
      delete[] doc.materials[i].submaterials;
    delete[] doc.materials;
  }
  delete[] doc.lights;
  std::memset(&doc, 0, sizeof(Document));
}

}  // namespace ase

// ---------------------------------------------------------------------------
// C API (for FFI — Python ctypes, etc.)
// ---------------------------------------------------------------------------

#include "ase/ase.h"

extern "C" {

int ase_parse(const char* path, ase_Document* out) {
  if (!path || !out) return -1;
  std::string error;
  return ase::parse_file(path, *out, error) ? 0 : -1;
}

void ase_free(ase_Document* doc) {
  if (!doc) return;
  ase::free_document(*doc);
}

void ase_alloc(ase_Document* doc, int objects, int materials, int lights) {
  if (!doc) return;
  std::memset(doc, 0, sizeof(ase_Document));

  doc->object_count = objects;
  if (objects > 0) doc->objects = new ase_Object[objects]();

  doc->material_count = materials;
  if (materials > 0) doc->materials = new ase_Material[materials]();

  doc->light_count = lights;
  if (lights > 0) doc->lights = new ase_Light[lights]();
}

void ase_alloc_object(ase_Object* obj, int verts, int uvs, int faces,
                      int colors, int weights) {
  if (!obj) return;

  obj->vert_count = verts;
  if (verts > 0) obj->verts = new float[verts * 3]();

  obj->uv_count = uvs;
  if (uvs > 0) obj->uvs = new ase_UV[uvs]();

  obj->face_count = faces;
  if (faces > 0) obj->faces = new ase_Face[faces]();

  obj->color_count = colors;
  if (colors > 0) obj->colors = new uint32_t[colors]();

  obj->weight_count = weights;
  if (weights > 0) {
    obj->weights = new ase_Weight[weights]();
    obj->skinned = 1;
  }
}

void ase_alloc_submaterials(ase_Material* mat, int count) {
  if (!mat || count <= 0) return;
  mat->submaterial_count = count;
  mat->submaterials = new ase_Material[count]();
}

}  // extern "C"
