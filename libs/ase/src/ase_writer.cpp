#include "ase/ase_writer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace ase {

namespace {

class AseWriter {
public:
  explicit AseWriter(const WriteOptions& opts) : opts_(opts) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%%.%df", opts.precision);
    fmt_ = buf;
  }

  bool write(std::string& out, const Document& doc) {
    out.clear();
    out_ = &out;

    write_header();
    write_scene();

    if (doc.material_count > 0 && doc.materials) {
      write_material_list(doc);
    }

    for (int i = 0; i < doc.object_count; ++i) {
      write_geom_object(doc.objects[i]);
    }

    for (int i = 0; i < doc.light_count; ++i) {
      write_light_object(doc.lights[i]);
    }

    out_ = nullptr;
    return true;
  }

private:
  const WriteOptions& opts_;
  std::string fmt_;
  std::string* out_ = nullptr;

  void emit(const char* s) { out_->append(s); }
  void emit(const std::string& s) { out_->append(s); }

  void line(const std::string& s) {
    out_->append(s);
    out_->push_back('\n');
  }

  std::string ff(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt_.c_str(), v);
    return buf;
  }

  std::string p3(float x, float y, float z) {
    return ff(x) + "\t" + ff(y) + "\t" + ff(z);
  }

  std::string p3(const float* v) {
    return p3(v[0], v[1], v[2]);
  }

  void write_header() {
    line("*3DSMAX_ASCIIEXPORT\t200");
    line("*COMMENT \"Written by opennova ASE writer\"");
  }

  void write_scene() {
    line("*SCENE {");
    line("\t*SCENE_FILENAME \"scene\"");
    line("\t*SCENE_FIRSTFRAME 0");
    line("\t*SCENE_LASTFRAME 100");
    line("\t*SCENE_FRAMESPEED 30");
    line("\t*SCENE_TICKSPERFRAME 160");
    line("\t*SCENE_BACKGROUND_STATIC 0.0000\t0.0000\t0.0000");
    line("\t*SCENE_AMBIENT_STATIC 0.0000\t0.0000\t0.0000");
    line("}");
  }

  void write_material_list(const Document& doc) {
    line("*MATERIAL_LIST {");
    line("\t*MATERIAL_COUNT " + std::to_string(doc.material_count));

    for (int i = 0; i < doc.material_count; ++i) {
      write_material(doc.materials[i], i);
    }

    line("}");
  }

  void write_material(const Material& mat, int idx) {
    write_material_body(mat, idx, -1, "\t");
  }

  void write_material_body(const Material& mat, int idx, int sub_idx,
                           const std::string& indent) {
    if (sub_idx < 0) {
      // Top-level material
      if (mat.submaterial_count > 0 && mat.submaterials) {
        line(indent + "*MATERIAL " + std::to_string(idx) + " {");
        std::string name = mat.name[0] ? mat.name : ("Material_" + std::to_string(idx));
        line(indent + "\t*MATERIAL_NAME \"" + name + "\"");
        line(indent + "\t*MATERIAL_CLASS \"Multi/Sub-Object\"");
        write_material_properties(mat, indent);
        line(indent + "\t*NUMSUBMTLS " + std::to_string(mat.submaterial_count));
        for (int i = 0; i < mat.submaterial_count; ++i) {
          write_material_body(mat.submaterials[i], 0, i, indent + "\t");
        }
        line(indent + "}");
        return;
      }
      line(indent + "*MATERIAL " + std::to_string(idx) + " {");
    } else {
      line(indent + "*SUBMATERIAL " + std::to_string(sub_idx) + " {");
    }

    std::string name = mat.name[0] ? mat.name : ("Material_" + std::to_string(idx));
    line(indent + "\t*MATERIAL_NAME \"" + name + "\"");
    line(indent + "\t*MATERIAL_CLASS \"Standard (Legacy)\"");
    write_material_properties(mat, indent);

    if (mat.extra_flags & 1u) {
      line(indent + "\t*MATERIAL_TWOSIDED");
    }

    // Diffuse map
    if (mat.maps[0][0] && mat.maps[1][0]) {
      // RGB Multiply composite: two textures blended via Multiply
      write_rgb_multiply_block((indent + "\t").c_str(), mat);
    } else if (mat.maps[0][0]) {
      write_map_block((indent + "\t").c_str(), "*MAP_DIFFUSE", mat.maps[0], 1,
                      mat.uv_u_offset[0], mat.uv_v_offset[0],
                      mat.uv_u_tiling[0], mat.uv_v_tiling[0]);
    }

    // Opacity map (maps[2])
    if (mat.maps[2][0]) {
      write_map_block((indent + "\t").c_str(), "*MAP_OPACITY", mat.maps[2], 3,
                      0.0f, 0.0f, 1.0f, 1.0f);
    }

    line(indent + "}");
  }

  void write_material_properties(const Material& mat, const std::string& indent) {
    line(indent + "\t*MATERIAL_AMBIENT " + p3(mat.ambient));
    line(indent + "\t*MATERIAL_DIFFUSE " + p3(mat.diffuse));
    line(indent + "\t*MATERIAL_SPECULAR " + p3(mat.specular));
    line(indent + "\t*MATERIAL_SHINE " + ff(mat.shine));
    line(indent + "\t*MATERIAL_SHINESTRENGTH " + ff(mat.shine_strength));
    line(indent + "\t*MATERIAL_TRANSPARENCY " + ff(mat.transparency));
    line(indent + "\t*MATERIAL_WIRESIZE " + ff(mat.wiresize > 0.0f ? mat.wiresize : 1.0f));

    const char* shading_str = "Blinn";
    switch (mat.shading) {
    case 1: shading_str = "Phong"; break;
    case 2: shading_str = "Metal"; break;
    case 3: shading_str = "Constant"; break;
    }
    line(indent + "\t*MATERIAL_SHADING " + shading_str);
    line(indent + "\t*MATERIAL_XP_FALLOFF " + ff(0.0f));
    line(indent + "\t*MATERIAL_SELFILLUM " + ff(0.0f));
    line(indent + "\t*MATERIAL_FALLOFF In");
    line(indent + "\t*MATERIAL_XP_TYPE Filter");
  }

  void write_map_block(const char* indent, const char* token, const char* bitmap,
                       int subno, float u_off, float v_off, float u_tile, float v_tile) {
    std::string ind(indent);
    line(ind + token + " {");
    // Use bitmap basename as map name
    line(ind + "\t*MAP_NAME \"" + std::string(bitmap) + "\"");
    line(ind + "\t*MAP_CLASS \"Bitmap\"");
    line(ind + "\t*MAP_SUBNO " + std::to_string(subno));
    line(ind + "\t*MAP_AMOUNT " + ff(1.0f));
    line(ind + "\t*BITMAP \"" + std::string(bitmap) + "\"");
    line(ind + "\t*MAP_TYPE Screen");
    line(ind + "\t*UVW_U_OFFSET " + ff(u_off));
    line(ind + "\t*UVW_V_OFFSET " + ff(v_off));
    line(ind + "\t*UVW_U_TILING " + ff(u_tile));
    line(ind + "\t*UVW_V_TILING " + ff(v_tile));
    line(ind + "\t*UVW_ANGLE " + ff(0.0f));
    line(ind + "\t*UVW_BLUR " + ff(1.0f));
    line(ind + "\t*UVW_BLUR_OFFSET " + ff(0.0f));
    line(ind + "\t*UVW_NOUSE_AMT " + ff(1.0f));
    line(ind + "\t*UVW_NOISE_SIZE " + ff(1.0f));
    line(ind + "\t*UVW_NOISE_LEVEL 1");
    line(ind + "\t*UVW_NOISE_PHASE " + ff(0.0f));
    line(ind + "\t*BITMAP_FILTER Pyramidal");
    line(ind + "}");
  }

  void write_rgb_multiply_block(const char* indent, const Material& mat) {
    std::string ind(indent);
    line(ind + "*MAP_DIFFUSE {");
    line(ind + "\t*MAP_NAME \"RGB_Multiply\"");
    line(ind + "\t*MAP_CLASS \"RGB Multiply\"");
    line(ind + "\t*MAP_SUBNO 1");
    line(ind + "\t*MAP_AMOUNT " + ff(1.0f));
    // Sub-map 0: diffuse texture
    write_map_generic(ind + "\t", mat.maps[0], 0,
                      mat.uv_u_offset[0], mat.uv_v_offset[0],
                      mat.uv_u_tiling[0], mat.uv_v_tiling[0]);
    // Sub-map 1: detail/lightmap texture
    write_map_generic(ind + "\t", mat.maps[1], 1,
                      mat.uv_u_offset[1], mat.uv_v_offset[1],
                      mat.uv_u_tiling[1], mat.uv_v_tiling[1]);
    line(ind + "}");
  }

  void write_map_generic(const std::string& indent, const char* bitmap, int subno,
                         float u_off, float v_off, float u_tile, float v_tile) {
    line(indent + "*MAP_GENERIC {");
    line(indent + "\t*MAP_NAME \"" + std::string(bitmap) + "\"");
    line(indent + "\t*MAP_CLASS \"Bitmap\"");
    line(indent + "\t*MAP_SUBNO " + std::to_string(subno));
    line(indent + "\t*MAP_AMOUNT " + ff(1.0f));
    line(indent + "\t*BITMAP \"" + std::string(bitmap) + "\"");
    line(indent + "\t*MAP_TYPE Screen");
    line(indent + "\t*UVW_U_OFFSET " + ff(u_off));
    line(indent + "\t*UVW_V_OFFSET " + ff(v_off));
    line(indent + "\t*UVW_U_TILING " + ff(u_tile));
    line(indent + "\t*UVW_V_TILING " + ff(v_tile));
    line(indent + "\t*UVW_ANGLE " + ff(0.0f));
    line(indent + "\t*UVW_BLUR " + ff(1.0f));
    line(indent + "\t*UVW_BLUR_OFFSET " + ff(0.0f));
    line(indent + "\t*UVW_NOUSE_AMT " + ff(1.0f));
    line(indent + "\t*UVW_NOISE_SIZE " + ff(1.0f));
    line(indent + "\t*UVW_NOISE_LEVEL 1");
    line(indent + "\t*UVW_NOISE_PHASE " + ff(0.0f));
    line(indent + "\t*BITMAP_FILTER Pyramidal");
    line(indent + "}");
  }

  void write_geom_object(const Object& obj) {
    line("*GEOMOBJECT {");

    line("\t*NODE_NAME \"" + std::string(obj.name) + "\"");
    if (obj.parent_name[0]) {
      line("\t*NODE_PARENT \"" + std::string(obj.parent_name) + "\"");
    }
    if (obj.node_id >= 0) {
      line("\t*NODE_BONENUMBER " + std::to_string(obj.node_id));
    }

    write_node_tm(obj.name, obj.tm_row);

    if (obj.vert_count > 0 || obj.face_count > 0) {
      write_mesh(obj);
    }

    line("\t*PROP_MOTIONBLUR 0");
    line("\t*PROP_CASTSHADOW 1");
    line("\t*PROP_RECVSHADOW 1");

    if (obj.material_ref >= 0) {
      line("\t*MATERIAL_REF " + std::to_string(obj.material_ref));
    }

    line("}");
  }

  void write_node_tm(const char* name, const float tm[4][3]) {
    line("\t*NODE_TM {");
    line("\t\t*NODE_NAME \"" + std::string(name) + "\"");
    line("\t\t*INHERIT_POS 0 0 0");
    line("\t\t*INHERIT_ROT 0 0 0");
    line("\t\t*INHERIT_SCL 0 0 0");
    line("\t\t*TM_ROW0 " + p3(tm[0]));
    line("\t\t*TM_ROW1 " + p3(tm[1]));
    line("\t\t*TM_ROW2 " + p3(tm[2]));
    line("\t\t*TM_ROW3 " + p3(tm[3]));
    line("\t\t*TM_POS " + p3(tm[3]));
    line("\t\t*TM_ROTAXIS 0.0000\t0.0000\t0.0000");
    line("\t\t*TM_ROTANGLE 0.0000");
    line("\t\t*TM_SCALE 1.0000\t1.0000\t1.0000");
    line("\t\t*TM_SCALEAXIS 0.0000\t0.0000\t0.0000");
    line("\t\t*TM_SCALEAXISANG 0.0000");
    line("\t}");
  }

  void write_mesh(const Object& obj) {
    line("\t*MESH {");
    line("\t\t*TIMEVALUE 0");
    line("\t\t*MESH_NUMVERTEX " + std::to_string(obj.vert_count));
    line("\t\t*MESH_NUMFACES " + std::to_string(obj.face_count));

    // Vertices
    if (obj.vert_count > 0 && obj.verts) {
      line("\t\t*MESH_VERTEX_LIST {");
      for (int i = 0; i < obj.vert_count; ++i) {
        const float* v = &obj.verts[i * 3];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_VERTEX %4d\t%s",
                      i, p3(v).c_str());
        line(buf);
      }
      line("\t\t}");
    }

    // Faces
    if (obj.face_count > 0 && obj.faces) {
      line("\t\t*MESH_FACE_LIST {");
      for (int i = 0; i < obj.face_count; ++i) {
        const Face& f = obj.faces[i];
        char buf[512];
        std::snprintf(buf, sizeof(buf),
          "\t\t\t*MESH_FACE %4d:    A: %4d B: %4d C: %4d AB: %4d BC: %4d CA: %4d",
          i, f.vert[0], f.vert[1], f.vert[2],
          (int)f.edge_visibility[0], (int)f.edge_visibility[1], (int)f.edge_visibility[2]);

        std::string ln(buf);
        ln += "\t *MESH_SMOOTHING ";

        // Emit smoothing group bit positions (1-indexed per ASE spec)
        bool has_smooth = false;
        for (int bit = 0; bit < 32; ++bit) {
          if (f.smoothing_mask & (1u << bit)) {
            if (has_smooth) ln += ",";
            ln += std::to_string(bit + 1);
            has_smooth = true;
          }
        }
        ln += " ";

        ln += "\t*MESH_MTLID " + std::to_string(f.material_id);
        line(ln);
      }
      line("\t\t}");
    }

    // UVs
    line("\t\t*MESH_NUMTVERTEX " + std::to_string(obj.uv_count));
    if (obj.uv_count > 0 && obj.uvs) {
      line("\t\t*MESH_TVERTLIST {");
      for (int i = 0; i < obj.uv_count; ++i) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_TVERT %d\t%s\t%s\t%s",
                      i, ff(obj.uvs[i].u).c_str(), ff(obj.uvs[i].v).c_str(),
                      ff(obj.uvs[i].w).c_str());
        line(buf);
      }
      line("\t\t}");

      // TFaces
      line("\t\t*MESH_NUMTVFACES " + std::to_string(obj.face_count));
      line("\t\t*MESH_TFACELIST {");
      for (int i = 0; i < obj.face_count; ++i) {
        const Face& f = obj.faces[i];
        char buf[128];
        std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_TFACE %d\t%d\t%d\t%d",
                      i, f.uv[0], f.uv[1], f.uv[2]);
        line(buf);
      }
      line("\t\t}");
    }

    // Vertex colors
    line("\t\t*MESH_NUMCVERTEX " + std::to_string(obj.color_count));
    if (obj.color_count > 0 && obj.colors) {
      line("\t\t*MESH_CVERTLIST {");
      for (int i = 0; i < obj.color_count; ++i) {
        uint32_t c = obj.colors[i];
        float r = ((c >> 16) & 0xFF) / 255.0f;
        float g = ((c >> 8) & 0xFF) / 255.0f;
        float b = (c & 0xFF) / 255.0f;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_VERTCOL %d\t%s\t%s\t%s",
                      i, ff(r).c_str(), ff(g).c_str(), ff(b).c_str());
        line(buf);
      }
      line("\t\t}");

      int cface_count = obj.color_face_count > 0 ? obj.color_face_count : obj.face_count;
      line("\t\t*MESH_NUMCVFACES " + std::to_string(cface_count));
      line("\t\t*MESH_CFACELIST {");
      for (int i = 0; i < obj.face_count; ++i) {
        const Face& f = obj.faces[i];
        char buf[128];
        std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_CFACE %d\t%d\t%d\t%d",
                      i, f.color[0], f.color[1], f.color[2]);
        line(buf);
      }
      line("\t\t}");
    }

    // Weights
    if (obj.skinned && obj.weight_count > 0 && obj.weights) {
      line("\t\t*MESH_WEIGHTS {");
      for (int i = 0; i < obj.weight_count; ++i) {
        const Weight& w = obj.weights[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
          "\t\t\t*MESH_WEIGHTSVERTEX %d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s",
          i, w.bone_index[0], w.bone_index[1], w.bone_index[2], w.bone_index[3],
          ff(w.weight[0]).c_str(), ff(w.weight[1]).c_str(),
          ff(w.weight[2]).c_str(), ff(w.weight[3]).c_str());
        line(buf);
      }
      line("\t\t}");
    }

    // Normals
    if (obj.vert_count > 0 && obj.face_count > 0 && obj.verts && obj.faces) {
      write_normals(obj);
    }

    line("\t}");
  }

  // Write *MESH_NORMALS block.
  // If obj.face_normals is provided, use pre-computed per-face-vertex normals.
  // Otherwise compute from geometry + smoothing groups.
  void write_normals(const Object& obj) {
    struct Vec3 { float x, y, z; };

    auto sub = [](const Vec3& a, const Vec3& b) -> Vec3 {
      return {a.x - b.x, a.y - b.y, a.z - b.z};
    };
    auto cross = [](const Vec3& a, const Vec3& b) -> Vec3 {
      return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
    };
    auto normalize = [](Vec3 v) -> Vec3 {
      float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
      if (len > 1e-12f) { v.x /= len; v.y /= len; v.z /= len; }
      return v;
    };
    auto vert_at = [&](int idx) -> Vec3 {
      return {obj.verts[idx*3], obj.verts[idx*3+1], obj.verts[idx*3+2]};
    };

    // Compute face normals (always needed for MESH_FACENORMAL lines)
    std::vector<Vec3> face_normals(obj.face_count);
    for (int i = 0; i < obj.face_count; ++i) {
      const Face& f = obj.faces[i];
      Vec3 v0 = vert_at(f.vert[0]);
      Vec3 v1 = vert_at(f.vert[1]);
      Vec3 v2 = vert_at(f.vert[2]);
      face_normals[i] = normalize(cross(sub(v1, v0), sub(v2, v0)));
    }

    const bool has_pre = (obj.face_normals && obj.face_normal_count == obj.face_count);

    // Build per-vertex face adjacency only if computing from smoothing groups
    std::vector<std::vector<int>> vert_faces;
    if (!has_pre) {
      vert_faces.resize(obj.vert_count);
      for (int i = 0; i < obj.face_count; ++i) {
        const Face& f = obj.faces[i];
        for (int j = 0; j < 3; ++j) {
          int vi = f.vert[j];
          if (vi >= 0 && vi < obj.vert_count)
            vert_faces[vi].push_back(i);
        }
      }
    }

    // Use high precision for vertex normals to preserve float32 fidelity.
    auto hp3 = [](float x, float y, float z) -> std::string {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%.9g\t%.9g\t%.9g", x, y, z);
      return buf;
    };

    line("\t\t*MESH_NORMALS {");
    for (int i = 0; i < obj.face_count; ++i) {
      const Face& f = obj.faces[i];
      Vec3 fn = face_normals[i];
      char buf[256];
      std::snprintf(buf, sizeof(buf), "\t\t\t*MESH_FACENORMAL %d\t%s",
                    i, p3(fn.x, fn.y, fn.z).c_str());
      line(buf);

      for (int j = 0; j < 3; ++j) {
        int vi = f.vert[j];
        Vec3 vn;
        if (has_pre) {
          const float* pn = &obj.face_normals[i * 9 + j * 3];
          vn = {pn[0], pn[1], pn[2]};
        } else if (f.smoothing_mask != 0 && vi >= 0 && vi < obj.vert_count) {
          float sx = 0, sy = 0, sz = 0;
          for (int adj : vert_faces[vi]) {
            if (obj.faces[adj].smoothing_mask & f.smoothing_mask) {
              sx += face_normals[adj].x;
              sy += face_normals[adj].y;
              sz += face_normals[adj].z;
            }
          }
          vn = normalize(Vec3{sx, sy, sz});
        } else {
          vn = fn;
        }
        // Use high precision for vertex normals to ensure lossless float32 roundtrip
        std::snprintf(buf, sizeof(buf), "\t\t\t\t*MESH_VERTEXNORMAL %d\t%s",
                      vi, (has_pre ? hp3(vn.x, vn.y, vn.z) : p3(vn.x, vn.y, vn.z)).c_str());
        line(buf);
      }
    }
    line("\t\t}");
  }

  void write_light_object(const Light& light) {
    line("*LIGHTOBJECT {");
    line("\t*NODE_NAME \"" + std::string(light.name) + "\"");

    const char* type_str = (light.type == 0) ? "Omni" : "Target";
    line(std::string("\t*LIGHT_TYPE ") + type_str);

    // Write a NODE_TM with the light position
    line("\t*NODE_TM {");
    line("\t\t*NODE_NAME \"" + std::string(light.name) + "\"");
    line("\t\t*INHERIT_POS 0 0 0");
    line("\t\t*INHERIT_ROT 0 0 0");
    line("\t\t*INHERIT_SCL 0 0 0");
    line("\t\t*TM_ROW0 1.0000\t0.0000\t0.0000");
    line("\t\t*TM_ROW1 0.0000\t1.0000\t0.0000");
    line("\t\t*TM_ROW2 " + p3(light.tm_row2));
    line("\t\t*TM_ROW3 0.0000\t0.0000\t0.0000");
    line("\t\t*TM_POS " + p3(light.pos));
    line("\t\t*TM_ROTAXIS 0.0000\t0.0000\t0.0000");
    line("\t\t*TM_ROTANGLE 0.0000");
    line("\t\t*TM_SCALE 1.0000\t1.0000\t1.0000");
    line("\t\t*TM_SCALEAXIS 0.0000\t0.0000\t0.0000");
    line("\t\t*TM_SCALEAXISANG 0.0000");
    line("\t}");

    line("\t*LIGHT_SHADOWS Off");
    line("\t*LIGHT_USELIGHT 1");
    line("\t*LIGHT_SPOTSHAPE Circle");
    line("\t*LIGHT_USEGLOBAL 0");
    line("\t*LIGHT_ABSMAPBIAS 0");
    line("\t*LIGHT_OVERSHOOT 0");

    line("\t*LIGHT_SETTINGS {");
    line("\t\t*TIMEVALUE 0");
    line("\t\t*LIGHT_COLOR " + p3(light.color));
    line("\t\t*LIGHT_INTENS " + ff(light.intensity));
    line("\t\t*LIGHT_ASPECT " + ff(-1.0f));
    line("\t\t*LIGHT_ATTNSTART " + ff(light.atten_start));
    line("\t\t*LIGHT_ATTNEND " + ff(light.atten_end));
    if (light.near_atten_start != 0.0f) {
      line("\t\t*LIGHT_NEAR_ATTNSTART " + ff(light.near_atten_start));
    }
    if (light.near_atten_end != 0.0f) {
      line("\t\t*LIGHT_NEAR_ATTNEND " + ff(light.near_atten_end));
    }
    if (light.hotspot != 0.0f) {
      line("\t\t*LIGHT_HOTSPOT " + ff(light.hotspot * 2.0f));
    }
    if (light.falloff != 0.0f) {
      line("\t\t*LIGHT_FALLOFF " + ff(light.falloff * 2.0f));
    }
    line("\t\t*LIGHT_TDIST " + ff(-1.0f));
    line("\t\t*LIGHT_MAPBIAS " + ff(0.0f));
    line("\t\t*LIGHT_MAPRANGE " + ff(0.0f));
    line("\t\t*LIGHT_MAPSIZE 0");
    line("\t\t*LIGHT_RAYBIAS " + ff(0.2f));
    line("\t}");

    line("}");
  }
};

}  // namespace

bool write_string(std::string& out, const Document& doc,
                  const WriteOptions& opts, std::string* error) {
  (void)error;
  AseWriter writer(opts);
  return writer.write(out, doc);
}

bool write_file(const std::string& path, const Document& doc,
                const WriteOptions& opts, std::string* error) {
  std::string text;
  if (!write_string(text, doc, opts, error)) return false;

  std::ofstream fout(path, std::ios::binary);
  if (!fout) {
    if (error) *error = "Cannot open file for writing: " + path;
    return false;
  }
  fout.write(text.data(), text.size());
  if (!fout) {
    if (error) *error = "Write error: " + path;
    return false;
  }
  return true;
}

}  // namespace ase

// ---------------------------------------------------------------------------
// C API (for FFI)
// ---------------------------------------------------------------------------

#include "ase/ase.h"

extern "C" {

int ase_write(const char* path, const ase_Document* doc) {
  if (!path || !doc) return -1;
  std::string error;
  return ase::write_file(path, *doc, ase::WriteOptions{}, &error) ? 0 : -1;
}

}  // extern "C"
