#include <runtime/renderer/authored_occluder.h>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

opennova::world::OcclusionPortalFace record(uint8_t type, uint8_t section,
                                            int32_t vertex_start,
                                            int32_t face_start) {
  opennova::world::OcclusionPortalFace out;
  out.type = type;
  out.section_a = section;
  out.vert_start = vertex_start;
  out.vert_count = 3;
  out.face_start = face_start;
  out.face_count = 1;
  return out;
}

} // namespace

int main() {
  using namespace opennova;
  world::OcclusionModel model;
  for (int i = 0; i < 9; ++i) {
    world::OcclusionVertex vertex;
    vertex.p[0] = static_cast<float>(i);
    vertex.p[1] = static_cast<float>(i + 10);
    vertex.p[2] = static_cast<float>(i + 20);
    model.vertices.push_back(vertex);
  }
  for (int i = 0; i < 3; ++i) {
    world::OcclusionFaceRec face;
    face.v[0] = 0;
    face.v[1] = 1;
    face.v[2] = 2;
    model.faces.push_back(face);
  }
  model.records.push_back(record(world::kOccRecOccluder, 2, 0, 0));
  model.records.push_back(record(world::kOccRecWindow, 2, 3, 1));
  model.records.push_back(record(world::kOccRecOccluder, 2, 6, 2));

  const auto sections = renderer::build_authored_occluder_sections(model);
  CHECK(sections.size() == 1);
  CHECK(sections[0].section == 2);
  CHECK(sections[0].vertices.size() == 6);
  CHECK(sections[0].indices.size() == 6);
  CHECK(sections[0].indices[0] == 0 && sections[0].indices[2] == 2);
  CHECK(sections[0].indices[3] == 3 && sections[0].indices[5] == 5);
  CHECK(sections[0].vertices[3].x == 6.0f);

  // Open/window/portal/welded records are never promoted to engine occluders.
  world::OcclusionModel portals;
  portals.records.push_back(record(world::kOccRecOpen, 1, 0, 0));
  portals.records.push_back(record(world::kOccRecWindow, 1, 0, 0));
  portals.records.push_back(record(world::kOccRecPortal, 1, 0, 0));
  portals.records.push_back(record(world::kOccRecWeldedLink, 1, 0, 0));
  CHECK(renderer::build_authored_occluder_sections(portals).empty());

  // A malformed record is ignored as one unit; it cannot partially poison a
  // valid section built from a following record.
  world::OcclusionPortalFace malformed =
      record(world::kOccRecOccluder, 4, 200, 0);
  model.records.insert(model.records.begin(), malformed);
  const auto robust = renderer::build_authored_occluder_sections(model);
  CHECK(robust.size() == 1 && robust[0].section == 2);

  return failures == 0 ? 0 : 1;
}
