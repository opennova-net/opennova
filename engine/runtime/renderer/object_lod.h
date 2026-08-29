#pragma once

#include <cstdint>
#include <vector>

namespace opennova::renderer {

struct ObjectLodSelection {
  int lod_index = -1;
  int32_t scaled_projected_radius_q16 = 0;
  float blend_fraction = 0.0f;
};

// Select an authored object LOD from the retail fine-to-coarse threshold table.
// The input is projected screen radius in Q16.16. `projection_scale` is the
// frame's resolution/detail normalization. `available`, when supplied, models
// the submit-time null-model fallback toward a finer LOD.
// [orig: object LOD threshold walk + blend @ 0x5c3b20; null fallback at
//  render_sector_entity @ 0x5c4303]
ObjectLodSelection select_object_lod(const std::vector<int32_t> &thresholds_q16,
                                     int32_t projected_radius_q16,
                                     float projection_scale = 1.0f,
                                     const std::vector<bool> &available = {});

} // namespace opennova::renderer
