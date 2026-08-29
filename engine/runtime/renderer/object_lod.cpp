#include <runtime/renderer/object_lod.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opennova::renderer {

// [orig: object RLOD threshold walk and overlap fraction @ 0x5c3b20]
ObjectLodSelection select_object_lod(const std::vector<int32_t> &thresholds_q16,
                                     int32_t projected_radius_q16,
                                     float projection_scale,
                                     const std::vector<bool> &available) {
  ObjectLodSelection result;
  if (thresholds_q16.empty()) {
    return result;
  }

  const double scaled = static_cast<double>(projected_radius_q16) *
                        static_cast<double>(projection_scale);
  const double bounded = std::clamp(
      scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
      static_cast<double>(std::numeric_limits<int32_t>::max()));
  result.scaled_projected_radius_q16 = static_cast<int32_t>(bounded);

  int selected = 0;
  while (selected + 1 < static_cast<int>(thresholds_q16.size()) &&
         result.scaled_projected_radius_q16 <= thresholds_q16[selected + 1]) {
    ++selected;
  }

  if (!available.empty()) {
    selected = std::min(selected, static_cast<int>(available.size()) - 1);
    while (selected > 0 && !available[static_cast<std::size_t>(selected)]) {
      --selected;
    }
    if (!available[static_cast<std::size_t>(selected)]) {
      result.lod_index = -1;
      return result;
    }
  }
  result.lod_index = selected;

  if (selected > 0) {
    const int32_t near_threshold =
        selected + 1 < static_cast<int>(thresholds_q16.size())
            ? thresholds_q16[static_cast<std::size_t>(selected + 1)]
            : 0;
    const int32_t far_threshold =
        thresholds_q16[static_cast<std::size_t>(selected)];
    const int64_t width = static_cast<int64_t>(far_threshold) -
                          static_cast<int64_t>(near_threshold);
    if (width != 0) {
      result.blend_fraction = std::clamp(
          static_cast<float>(static_cast<int64_t>(
                                 result.scaled_projected_radius_q16) -
                             near_threshold) /
              static_cast<float>(width),
          0.0f, 1.0f);
    }
  }
  return result;
}

} // namespace opennova::renderer
