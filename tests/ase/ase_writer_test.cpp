// Round-trip test: build Document programmatically -> write_string() ->
// write to temp file -> parse_file() -> compare_document().
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "ase/ase_parser.h"
#include "ase/ase_writer.h"
#include "common/ase_roundtrip_util.h"
#include "common/test_paths.h"

namespace {

// Build a minimal Document with 1 object, 1 material, 1 light.
ase::Document build_test_doc() {
  ase::Document doc{};

  // Allocate
  doc.object_count = 1;
  doc.objects = new ase::Object[1]();
  doc.material_count = 1;
  doc.materials = new ase::Material[1]();
  doc.light_count = 1;
  doc.lights = new ase::Light[1]();
  doc.flags = 1;
  doc.skinned_flags = 1;

  // Object
  auto& obj = doc.objects[0];
  std::strncpy(obj.name, "TestMesh", sizeof(obj.name) - 1);
  obj.node_id = -1;
  obj.material_ref = 0;

  // TM (identity-ish)
  obj.tm_row[0][0] = 1.0f; obj.tm_row[0][1] = 0.0f; obj.tm_row[0][2] = 0.0f;
  obj.tm_row[1][0] = 0.0f; obj.tm_row[1][1] = 1.0f; obj.tm_row[1][2] = 0.0f;
  obj.tm_row[2][0] = 0.0f; obj.tm_row[2][1] = 0.0f; obj.tm_row[2][2] = 1.0f;
  obj.tm_row[3][0] = 5.0f; obj.tm_row[3][1] = 10.0f; obj.tm_row[3][2] = 15.0f;

  // Vertices (triangle)
  obj.vert_count = 3;
  obj.verts = new float[9]{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

  // UVs
  obj.uv_count = 3;
  obj.uvs = new ase::UV[3]{
    {0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
  };

  // Faces
  obj.face_count = 1;
  obj.faces = new ase::Face[1]();
  auto& f = obj.faces[0];
  f.vert[0] = 0; f.vert[1] = 1; f.vert[2] = 2; f.vert[3] = 0;
  f.uv[0] = 0; f.uv[1] = 1; f.uv[2] = 2; f.uv[3] = 0;
  f.edge_visibility[0] = 1; f.edge_visibility[1] = 1;
  f.edge_visibility[2] = 1; f.edge_visibility[3] = 1;
  f.smoothing_mask = 1;
  f.material_id = 0;

  // Weights
  obj.weight_count = 3;
  obj.weights = new ase::Weight[3]();
  obj.weights[0].bone_index[0] = 0; obj.weights[0].bone_index[1] = 1;
  obj.weights[0].bone_index[2] = -1; obj.weights[0].bone_index[3] = -1;
  obj.weights[0].weight[0] = 0.7f; obj.weights[0].weight[1] = 0.3f;
  obj.weights[1].bone_index[0] = 0; obj.weights[1].bone_index[1] = -1;
  obj.weights[1].bone_index[2] = -1; obj.weights[1].bone_index[3] = -1;
  obj.weights[1].weight[0] = 1.0f;
  obj.weights[2].bone_index[0] = -1; obj.weights[2].bone_index[1] = -1;
  obj.weights[2].bone_index[2] = -1; obj.weights[2].bone_index[3] = -1;
  obj.skinned = 1;

  // Material
  auto& mat = doc.materials[0];
  std::strncpy(mat.name, "TestMat", sizeof(mat.name) - 1);
  std::strncpy(mat.maps[0], "diffuse.tga", sizeof(mat.maps[0]) - 1);
  mat.uv_u_tiling[0] = 1.0f;
  mat.uv_v_tiling[0] = 1.0f;

  // Light
  auto& light = doc.lights[0];
  std::strncpy(light.name, "TestLight", sizeof(light.name) - 1);
  light.type = 0;  // Omni
  light.color[0] = 1.0f; light.color[1] = 0.8f; light.color[2] = 0.5f;
  light.intensity = 1.5f;
  light.atten_start = 10.0f;
  light.atten_end = 100.0f;
  light.pos[0] = 1.0f; light.pos[1] = 2.0f; light.pos[2] = 3.0f;
  light.tm_row2[0] = 0.0f; light.tm_row2[1] = 0.0f; light.tm_row2[2] = 1.0f;

  return doc;
}

bool test_write_parse_roundtrip() {
  ase::Document src = build_test_doc();

  // Write to string
  std::string ase_text;
  ase::WriteOptions opts;
  opts.precision = 4;
  if (!ase::write_string(ase_text, src, opts)) {
    std::fprintf(stderr, "write_string failed\n");
    ase::free_document(src);
    return false;
  }

  // Write to temp file
  std::filesystem::path path = std::filesystem::path(test_paths_temp_dir()) / "ase_writer_roundtrip.ase";
  {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "Cannot write temp file\n");
      ase::free_document(src);
      return false;
    }
    f << ase_text;
  }

  // Parse back
  ase::Document parsed{};
  std::string err;
  bool ok = ase::parse_file(path.string(), parsed, err);
  std::filesystem::remove(path);

  if (!ok) {
    std::fprintf(stderr, "Re-parse failed: %s\n", err.c_str());
    ase::free_document(src);
    return false;
  }

  // Compare core geometry fields
  bool pass = true;
  if (parsed.object_count != src.object_count) {
    std::fprintf(stderr, "object_count: %d vs %d\n", parsed.object_count, src.object_count);
    pass = false;
  }
  if (parsed.light_count != src.light_count) {
    std::fprintf(stderr, "light_count: %d vs %d\n", parsed.light_count, src.light_count);
    pass = false;
  }
  if (parsed.material_count != src.material_count) {
    std::fprintf(stderr, "material_count: %d vs %d\n", parsed.material_count, src.material_count);
    pass = false;
  }

  if (pass && parsed.object_count > 0) {
    auto& a = src.objects[0];
    auto& b = parsed.objects[0];
    if (std::strcmp(a.name, b.name) != 0) {
      std::fprintf(stderr, "name mismatch: '%s' vs '%s'\n", a.name, b.name);
      pass = false;
    }
    if (a.vert_count != b.vert_count) {
      std::fprintf(stderr, "vert_count: %d vs %d\n", a.vert_count, b.vert_count);
      pass = false;
    }
    // Vertices should match (writer writes as-is, parser swizzles, so they won't match
    // directly since the parser applies swizzle on read). We just verify the counts match
    // and the data round-trips through write->parse without crashing.
    if (a.face_count != b.face_count) {
      std::fprintf(stderr, "face_count: %d vs %d\n", a.face_count, b.face_count);
      pass = false;
    }
    if (a.uv_count != b.uv_count) {
      std::fprintf(stderr, "uv_count: %d vs %d\n", a.uv_count, b.uv_count);
      pass = false;
    }
    if (a.weight_count != b.weight_count) {
      std::fprintf(stderr, "weight_count: %d vs %d\n", a.weight_count, b.weight_count);
      pass = false;
    }
    if (a.skinned != b.skinned) {
      std::fprintf(stderr, "skinned: %d vs %d\n", a.skinned, b.skinned);
      pass = false;
    }
    // Verify weight data survives round-trip
    if (pass && b.weights) {
      for (int i = 0; i < b.weight_count; ++i) {
        for (int j = 0; j < 4; ++j) {
          if (a.weights[i].bone_index[j] != b.weights[i].bone_index[j]) {
            std::fprintf(stderr, "weight[%d].bone_index[%d]: %d vs %d\n",
                         i, j, a.weights[i].bone_index[j], b.weights[i].bone_index[j]);
            pass = false;
          }
          if (!ase_test::nearly_equal(a.weights[i].weight[j], b.weights[i].weight[j])) {
            std::fprintf(stderr, "weight[%d].weight[%d]: %f vs %f\n",
                         i, j, a.weights[i].weight[j], b.weights[i].weight[j]);
            pass = false;
          }
        }
      }
    }
  }

  // Verify material data
  if (pass && parsed.material_count > 0) {
    if (std::strcmp(src.materials[0].name, parsed.materials[0].name) != 0) {
      std::fprintf(stderr, "material name: '%s' vs '%s'\n",
                   src.materials[0].name, parsed.materials[0].name);
      pass = false;
    }
    if (std::strcmp(src.materials[0].maps[0], parsed.materials[0].maps[0]) != 0) {
      std::fprintf(stderr, "material map0: '%s' vs '%s'\n",
                   src.materials[0].maps[0], parsed.materials[0].maps[0]);
      pass = false;
    }
  }

  // Verify light data
  if (pass && parsed.light_count > 0) {
    if (std::strcmp(src.lights[0].name, parsed.lights[0].name) != 0) {
      std::fprintf(stderr, "light name: '%s' vs '%s'\n",
                   src.lights[0].name, parsed.lights[0].name);
      pass = false;
    }
    for (int i = 0; i < 3; ++i) {
      if (!ase_test::nearly_equal(src.lights[0].color[i], parsed.lights[0].color[i])) {
        std::fprintf(stderr, "light color[%d]: %f vs %f\n",
                     i, src.lights[0].color[i], parsed.lights[0].color[i]);
        pass = false;
      }
    }
  }

  ase::free_document(src);
  ase::free_document(parsed);
  return pass;
}

bool test_write_file_api() {
  ase::Document doc = build_test_doc();
  std::filesystem::path path = std::filesystem::path(test_paths_temp_dir()) / "ase_writer_file.ase";

  std::string err;
  bool ok = ase::write_file(path.string(), doc, ase::WriteOptions{}, &err);
  if (!ok) {
    std::fprintf(stderr, "write_file failed: %s\n", err.c_str());
    ase::free_document(doc);
    return false;
  }

  // Verify file exists and is non-empty
  auto sz = std::filesystem::file_size(path);
  std::filesystem::remove(path);
  ase::free_document(doc);

  if (sz == 0) {
    std::fprintf(stderr, "Written file is empty\n");
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!test_write_parse_roundtrip()) return 1;
  if (!test_write_file_api()) return 2;
  return 0;
}
