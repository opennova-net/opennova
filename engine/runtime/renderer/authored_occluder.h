#pragma once

#include <cstdint>
#include <vector>

#include <runtime/world/occlusion.h>

namespace opennova::renderer {

struct OccluderVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct AuthoredOccluderSection {
  uint8_t section = 0;
  std::vector<OccluderVertex> vertices;
  std::vector<int32_t> indices;
};

// Convert only closed OOBJ type-0 faces into section-grouped triangle data.
// Portal, window, open, and welded-link records are visibility semantics, not
// conservative scene occluders, and are deliberately excluded.
std::vector<AuthoredOccluderSection>
build_authored_occluder_sections(const world::OcclusionModel &model);

} // namespace opennova::renderer
