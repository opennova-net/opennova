#pragma once

#include <array>
#include <cstdint>

namespace renderer {

// Original OED reference: PrepareLightParams @ 0x46A500.
std::array<float, 4> build_oed_light_attenuation(float light_range);

// Build a depth-mask plane (a, b, c, d so that ax + by + cz + d = 0 marks
// the slice of space occluded by the light cone).  Used by spotlight
// projection passes; same math runs server-side for physics/AI cone tests.
std::array<float, 4> build_depth_mask_plane(float pos_x, float pos_y, float pos_z,
                                             float dir_x, float dir_y, float dir_z,
                                             float atten_start, float atten_end);

// Convert a raw view-projection matrix into a shadow-sample matrix (offsets
// xy by 0.5 with a half-texel bias).  shadow_resolution=0 falls back to 512.
std::array<float, 16> build_shadow_sample_matrix(const std::array<float, 16> &raw_view_proj,
                                                  uint32_t shadow_resolution = 512);

}  // namespace renderer
