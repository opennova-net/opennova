#include "renderer/light_runtime.h"

#include <algorithm>
#include <cmath>

namespace renderer {

std::array<float, 4> build_oed_light_attenuation(float light_range) {
    // Original OED reference: PrepareLightParams @ 0x46A500 emits
    // {1, 0, 15 / range^2, 1} after scaling the runtime range by 1.25.
    const float range = std::max(light_range * 1.25f, 0.001f);
    return {1.0f, 0.0f, 15.0f / (range * range), 1.0f};
}

std::array<float, 4> build_depth_mask_plane(float pos_x, float pos_y, float pos_z,
                                             float dir_x, float dir_y, float dir_z,
                                             float atten_start, float atten_end) {
    const float len_sq = dir_x * dir_x + dir_y * dir_y + dir_z * dir_z;
    if (len_sq < 0.000001f)
        return {0.0f, 0.0f, 0.0f, 0.0f};

    const float inv_len = 1.0f / std::sqrt(len_sq);
    const float nx = dir_x * inv_len;
    const float ny = dir_y * inv_len;
    const float nz = dir_z * inv_len;
    const float range = std::max(atten_end - atten_start, 0.001f);
    const float distance = -(nx * pos_x + ny * pos_y + nz * pos_z) - atten_start;
    return {nx / range, ny / range, nz / range, distance / range};
}

std::array<float, 16> build_shadow_sample_matrix(const std::array<float, 16> &raw_view_proj,
                                                  uint32_t shadow_resolution) {
    std::array<float, 16> out = raw_view_proj;
    const float half = 0.5f;
    const float bias = half + (half / float(shadow_resolution == 0 ? 512 : shadow_resolution));

    out[0] *= half;
    out[4] *= half;
    out[8] *= half;
    out[12] *= half;
    out[1] *= half;
    out[5] *= half;
    out[9] *= half;
    out[13] *= half;

    out[0] += bias * out[3];
    out[1] -= bias * out[3];
    out[4] += bias * out[7];
    out[5] -= bias * out[7];
    out[8] += bias * out[11];
    out[9] -= bias * out[11];
    out[12] += bias * out[15];
    out[13] -= bias * out[15];

    return out;
}

}  // namespace renderer
