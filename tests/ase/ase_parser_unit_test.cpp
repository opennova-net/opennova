#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "ase/ase_parser.h"
#include "common/ase_roundtrip_util.h"
#include "common/test_paths.h"

namespace {

bool write_temp_ase(const char* name, const char* content, std::filesystem::path& out) {
  out = std::filesystem::path(test_paths_temp_dir()) / name;
  std::ofstream f(out);
  if (!f) {
    std::fprintf(stderr, "Cannot write temp file: %s\n", out.string().c_str());
    return false;
  }
  f << content;
  return true;
}

bool test_weights_and_tm_swizzle() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "Skinned"
  *MATERIAL_REF 0
  *NODE_TM {
    *NODE_NAME "Skinned"
    *TM_ROW0 1 0 0
    *TM_ROW1 0 1 0
    *TM_ROW2 0 0 1
    *TM_ROW3 1 2 3
    *TM_POS 1 2 3
  }
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 3
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 1
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 0
    }
    *MESH_WEIGHTS {
      *MESH_WEIGHTSVERTEX 0 0 1 -1 -1 0.5 0.5 0.0 0.0
      *MESH_WEIGHTSVERTEX 1 2 -1 -1 -1 1.0 0.0 0.0 0.0
      *MESH_WEIGHTSVERTEX 2 -1 -1 -1 -1 0.0 0.0 0.0 0.0
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *MATERIAL_NAME "Default"
  }
}
)";

  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_weights.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.object_count != 1 || !doc.objects) { pass = false; }
  if (pass) {
    auto& w0 = doc.objects[0].weights[0];
    if (w0.bone_index[0] != 0 || w0.bone_index[1] != 1 ||
        !ase_test::nearly_equal(w0.weight[0], 0.5f) ||
        !ase_test::nearly_equal(w0.weight[1], 0.5f)) {
      std::fprintf(stderr, "weight[0] mismatch\n");
      pass = false;
    }
    // TM_POS (1,2,3) swizzled to (-2, 1, 3)
    float* row3 = doc.objects[0].tm_row[3];
    if (!ase_test::nearly_equal(row3[0], -2.0f) ||
        !ase_test::nearly_equal(row3[1], 1.0f) ||
        !ase_test::nearly_equal(row3[2], 3.0f)) {
      std::fprintf(stderr, "tm_row[3] = (%.4f, %.4f, %.4f), expected (-2, 1, 3)\n",
                   row3[0], row3[1], row3[2]);
      pass = false;
    }
    if (!doc.objects[0].skinned) {
      std::fprintf(stderr, "skinned flag not set\n");
      pass = false;
    }
  }
  ase::free_document(doc);
  return pass;
}

bool test_material_maps_and_bump_ignore() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "Dummy"
  *MATERIAL_REF 0
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 6
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 2
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
      *MESH_VERTEX 3 0 0 0
      *MESH_VERTEX 4 1 0 0
      *MESH_VERTEX 5 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 0
      *MESH_FACE 1: A: 3 B: 4 C: 5 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 1
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *SUBMATERIAL 0 {
      *MATERIAL_NAME "MatA"
      *MAP_DIFFUSE {
        *MAP_CLASS "Bitmap"
        *BITMAP "C:\textures\diffuse_a.tga"
      }
      *MAP_BUMP {
        *MAP_CLASS "Bitmap"
        *BITMAP "C:\textures\bump_a.tga"
      }
    }
    *SUBMATERIAL 1 {
      *MATERIAL_NAME "MatB"
      *MAP_DIFFUSE {
        *MAP_CLASS "RGB Multiply"
        *MAP_GENERIC {
          *MAP_CLASS "Bitmap"
          *MAP_SUBNO 0
          *BITMAP "C:\textures\layer0.tga"
        }
        *MAP_GENERIC {
          *MAP_CLASS "Bitmap"
          *MAP_SUBNO 1
          *BITMAP "C:\textures\layer1.tga"
        }
      }
    }
  }
}
)";

  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_maps.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.material_count != 2) {
    std::fprintf(stderr, "material_count=%d (expected 2)\n", doc.material_count);
    pass = false;
  }
  if (std::string(doc.materials[0].maps[0]) != "diffuse_a.tga") {
    std::fprintf(stderr, "mat0 map0='%s'\n", doc.materials[0].maps[0]);
    pass = false;
  }
  // Bump should not overwrite diffuse.
  if (doc.materials[0].maps[1][0] != '\0' && std::string(doc.materials[0].maps[1]) != "") {
    std::fprintf(stderr, "mat0 map1 should be empty but is '%s'\n", doc.materials[0].maps[1]);
    pass = false;
  }
  // RGB Multiply sublayers captured into maps[0] and maps[1].
  if (std::string(doc.materials[1].maps[0]) != "layer0.tga") {
    std::fprintf(stderr, "mat1 map0='%s'\n", doc.materials[1].maps[0]);
    pass = false;
  }
  if (std::string(doc.materials[1].maps[1]) != "layer1.tga") {
    std::fprintf(stderr, "mat1 map1='%s'\n", doc.materials[1].maps[1]);
    pass = false;
  }
  ase::free_document(doc);
  return pass;
}

bool test_rgb_multiply_flags_and_tiling() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "Dummy2"
  *MATERIAL_REF 0
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 3
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 1
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 0
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *MATERIAL_NAME "MatRGB"
    *MAP_DIFFUSE {
      *MAP_CLASS "RGB Multiply"
      *MAP_GENERIC {
        *MAP_CLASS "Bitmap"
        *MAP_SUBNO 0
        *BITMAP "C:\\base.tga"
        *UVW_U_TILING 2.0
        *UVW_V_TILING 3.0
      }
      *MAP_GENERIC {
        *MAP_CLASS "Bitmap"
        *MAP_SUBNO 1
        *BITMAP "C:\\overlay.tga"
        *UVW_U_TILING 4.0
        *UVW_V_TILING 5.0
      }
    }
  }
}
)";

  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_rgb_mult.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.material_count != 1) {
    std::fprintf(stderr, "material_count=%d (expected 1)\n", doc.material_count);
    pass = false;
  } else {
    const auto& m = doc.materials[0];
    if (m.flags != 2) {
      std::fprintf(stderr, "flags=0x%x (expected 0x2)\n", m.flags);
      pass = false;
    }
    if (std::string(m.maps[0]) != "base.tga" || std::string(m.maps[1]) != "overlay.tga") {
      std::fprintf(stderr, "maps='%s','%s'\n", m.maps[0], m.maps[1]);
      pass = false;
    }
    if (!ase_test::nearly_equal(m.uv_u_tiling[0], 2.0f) ||
        !ase_test::nearly_equal(m.uv_v_tiling[0], 3.0f) ||
        !ase_test::nearly_equal(m.uv_u_tiling[1], 4.0f) ||
        !ase_test::nearly_equal(m.uv_v_tiling[1], 5.0f)) {
      std::fprintf(stderr, "tiling mismatch\n");
      pass = false;
    }
  }
  ase::free_document(doc);
  return pass;
}

bool test_unused_submaterial_pruned() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "OneTri"
  *MATERIAL_REF 0
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 3
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 1
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 1
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *SUBMATERIAL 0 {
      *MATERIAL_NAME "Unused"
      *MAP_DIFFUSE {
        *MAP_CLASS "Bitmap"
        *BITMAP "C:\\unused.tga"
      }
    }
    *SUBMATERIAL 1 {
      *MATERIAL_NAME "Used"
      *MAP_DIFFUSE {
        *MAP_CLASS "Bitmap"
        *BITMAP "C:\\used.tga"
      }
    }
  }
}
)";
  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_unused_sub.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.material_count != 1) {
    std::fprintf(stderr, "material_count=%d (expected 1)\n", doc.material_count);
    pass = false;
  } else if (std::string(doc.materials[0].name) != "Used") {
    std::fprintf(stderr, "material name='%s' (expected Used)\n", doc.materials[0].name);
    pass = false;
  }
  ase::free_document(doc);
  return pass;
}

bool test_top_material_keeps_multi_name() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "TwoFace"
  *MATERIAL_REF 0
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 3
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 2
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 0
      *MESH_FACE 1: A: 0 B: 2 C: 1 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 9
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *MATERIAL_NAME "MultiTop"
    *SUBMATERIAL 0 {
      *MATERIAL_NAME "Sub0"
      *MAP_DIFFUSE { *MAP_CLASS "Bitmap" *BITMAP "C:\\sub0.tga" }
    }
  }
}
)";
  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_topname.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.material_count != 2) {
    std::fprintf(stderr, "material_count=%d (expected 2)\n", doc.material_count);
    pass = false;
  } else {
    if (std::string(doc.materials[0].name) != "MultiTop") {
      std::fprintf(stderr, "top name='%s' (expected MultiTop)\n", doc.materials[0].name);
      pass = false;
    }
    if (std::string(doc.materials[1].name) != "Sub0") {
      std::fprintf(stderr, "sub0 name='%s' (expected Sub0)\n", doc.materials[1].name);
      pass = false;
    }
  }
  ase::free_document(doc);
  return pass;
}

bool test_uv_tiling_channels_respect_map_type() {
  const char* ase = R"(
*GEOMOBJECT {
  *NODE_NAME "OneTri"
  *MATERIAL_REF 0
  *MESH {
    *TIMEVALUE 0
    *MESH_NUMVERTEX 3
    *MESH_NUMTVERTEX 0
    *MESH_NUMFACES 1
    *MESH_VERTEX_LIST {
      *MESH_VERTEX 0 0 0 0
      *MESH_VERTEX 1 1 0 0
      *MESH_VERTEX 2 0 1 0
    }
    *MESH_FACE_LIST {
      *MESH_FACE 0: A: 0 B: 1 C: 2 AB: 1 BC: 1 CA: 1 *MESH_SMOOTHING 0 *MESH_MTLID 0
    }
  }
}
*MATERIAL_LIST {
  *MATERIAL_COUNT 1
  *MATERIAL 0 {
    *MATERIAL_NAME "TilingTest"
    *MAP_DIFFUSE {
      *MAP_CLASS "RGB Multiply"
      *MAP_GENERIC {
        *MAP_CLASS "Bitmap"
        *MAP_SUBNO 0
        *BITMAP "C:\\base.tga"
        *UVW_U_TILING 2.0
        *UVW_V_TILING 3.0
      }
      *MAP_GENERIC {
        *MAP_CLASS "Bitmap"
        *MAP_SUBNO 1
        *BITMAP "C:\\overlay.tga"
        *UVW_U_TILING 4.0
        *UVW_V_TILING 5.0
      }
      *MAP_GENERIC {
        *MAP_CLASS "Bitmap"
        *MAP_SUBNO 2
        *BITMAP "C:\\extra.tga"
        *UVW_U_TILING 6.0
        *UVW_V_TILING 7.0
      }
    }
    *MAP_OPACITY {
      *MAP_CLASS "Bitmap"
      *MAP_SUBNO 0
      *BITMAP "C:\\opacity.tga"
      *UVW_U_TILING 9.0
      *UVW_V_TILING 9.0
    }
    *MAP_BUMP {
      *MAP_CLASS "Bitmap"
      *MAP_SUBNO 0
      *BITMAP "C:\\bump.tga"
      *UVW_U_TILING 11.0
      *UVW_V_TILING 11.0
    }
  }
}
)";
  std::filesystem::path path;
  if (!write_temp_ase("ase_unit_uvtiling.ase", ase, path)) return false;
  ase::Document doc{};
  std::string err;
  bool ok = ase::parse_file(path.string(), doc, err);
  std::filesystem::remove(path);
  if (!ok) {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return false;
  }
  bool pass = true;
  if (doc.material_count != 1) {
    std::fprintf(stderr, "material_count=%d (expected 1)\n", doc.material_count);
    pass = false;
  } else {
    const auto& m = doc.materials[0];
    if (std::string(m.maps[0]) != "base.tga" || std::string(m.maps[1]) != "overlay.tga") {
      std::fprintf(stderr, "maps='%s','%s'\n", m.maps[0], m.maps[1]);
      pass = false;
    }
    if (!ase_test::nearly_equal(m.uv_u_tiling[0], 2.0f) ||
        !ase_test::nearly_equal(m.uv_v_tiling[0], 3.0f) ||
        !ase_test::nearly_equal(m.uv_u_tiling[1], 4.0f) ||
        !ase_test::nearly_equal(m.uv_v_tiling[1], 5.0f)) {
      std::fprintf(stderr, "diffuse/RGB tiling mismatch\n");
      pass = false;
    }
  }
  ase::free_document(doc);
  return pass;
}

}  // namespace

int main() {
  if (!test_weights_and_tm_swizzle()) return 1;
  if (!test_material_maps_and_bump_ignore()) return 2;
  if (!test_rgb_multiply_flags_and_tiling()) return 3;
  if (!test_unused_submaterial_pruned()) return 4;
  if (!test_top_material_keeps_multi_name()) return 5;
  if (!test_uv_tiling_channels_respect_map_type()) return 6;
  return 0;
}
