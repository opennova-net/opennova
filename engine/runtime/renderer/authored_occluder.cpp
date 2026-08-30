#include <runtime/renderer/authored_occluder.h>

#include <algorithm>
#include <cstddef>

namespace opennova::renderer {
namespace {

bool valid_slice(int32_t start, int32_t count, std::size_t size) {
  return start >= 0 && count >= 0 && static_cast<std::size_t>(start) <= size &&
         static_cast<std::size_t>(count) <=
             size - static_cast<std::size_t>(start);
}

} // namespace

// [orig: load_occlusion_model_data type-0 OOBJ build @ 0x5b4a00]
std::vector<AuthoredOccluderSection>
build_authored_occluder_sections(const world::OcclusionModel &model) {
  std::vector<AuthoredOccluderSection> sections;
  for (const world::OcclusionPortalFace &record : model.records) {
    if (record.type != world::kOccRecOccluder ||
        !valid_slice(record.vert_start, record.vert_count,
                     model.vertices.size()) ||
        !valid_slice(record.face_start, record.face_count,
                     model.faces.size())) {
      continue;
    }
    bool faces_valid = true;
    for (int32_t i = 0; i < record.face_count; ++i) {
      const world::OcclusionFaceRec &face =
          model.faces[static_cast<std::size_t>(record.face_start + i)];
      faces_valid = faces_valid && face.v[0] < record.vert_count &&
                    face.v[1] < record.vert_count &&
                    face.v[2] < record.vert_count;
    }
    if (!faces_valid) {
      continue;
    }

    auto section_it =
        std::find_if(sections.begin(), sections.end(),
                     [&record](const AuthoredOccluderSection &section) {
                       return section.section == record.section_a;
                     });
    if (section_it == sections.end()) {
      sections.push_back(AuthoredOccluderSection{});
      section_it = sections.end() - 1;
      section_it->section = record.section_a;
    }
    const int32_t vertex_base =
        static_cast<int32_t>(section_it->vertices.size());
    section_it->vertices.reserve(section_it->vertices.size() +
                                 static_cast<std::size_t>(record.vert_count));
    for (int32_t i = 0; i < record.vert_count; ++i) {
      const world::OcclusionVertex &vertex =
          model.vertices[static_cast<std::size_t>(record.vert_start + i)];
      section_it->vertices.push_back({vertex.p[0], vertex.p[1], vertex.p[2]});
    }
    section_it->indices.reserve(section_it->indices.size() +
                                static_cast<std::size_t>(record.face_count) *
                                    3);
    for (int32_t i = 0; i < record.face_count; ++i) {
      const world::OcclusionFaceRec &face =
          model.faces[static_cast<std::size_t>(record.face_start + i)];
      section_it->indices.push_back(vertex_base + face.v[0]);
      section_it->indices.push_back(vertex_base + face.v[1]);
      section_it->indices.push_back(vertex_base + face.v[2]);
    }
  }
  std::sort(sections.begin(), sections.end(),
            [](const AuthoredOccluderSection &left,
               const AuthoredOccluderSection &right) {
              return left.section < right.section;
            });
  return sections;
}

} // namespace opennova::renderer
