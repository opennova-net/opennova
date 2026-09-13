#include <runtime/renderer/object_lod.h>
#include <runtime/world/entity.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opennova::renderer {

namespace {

// The quality term of the frame scale: detail * 0.33 + 0.34 for the lower
// profiles (flt_7C59B4 = 0x3EA8F5C3, flt_7D76CC = 0x3EAE147B), replaced by
// the fixed 2.0 (flt_7C3B90) on the highest shipped profile, and the
// 640-wide reference width the projection is normalized to (flt_7DC188).
// [orig: Terrain_RenderSceneWithReflection @ 0x5c940c..0x5c9462;
//  Terrain_CollectVisibleEntitiesForReflection @ 0x5c90c3..0x5c90eb]
constexpr float kObjectLodDetailQualitySlope = 0.33f;
constexpr float kObjectLodDetailQualityBias = 0.34f;
constexpr float kObjectLodDetail3Quality = 2.0f;
constexpr float kObjectLodReferenceWidth = 640.0f;

} // namespace

ObjectProjectionSphere object_projection_sphere_from_bounds_q16(
    const std::array<int32_t, 3> &minimum,
    const std::array<int32_t, 3> &maximum,
    int32_t runtime_scale_q16, int32_t definition_scale_q16,
    bool zero_center) {
  ObjectProjectionSphere sphere;
  sphere.valid = true;
  double squared_radius = 0.0;
  for (int axis = 0; axis < 3; ++axis) {
    const int32_t low = std::min(minimum[axis], int32_t{0x40000000});
    const int32_t high = std::max(maximum[axis], int32_t{-0x40000000});
    const int32_t difference = static_cast<int32_t>(
        static_cast<uint32_t>(high) - static_cast<uint32_t>(low));
    // The eweap-powerup leg stores a zero center before the halves are
    // measured, so each half is the clamped maximum itself.
    // [orig: Entity_InitFromModel @ 0x40df06..0x40df16, halves @ 0x40df66..0x40df76]
    sphere.center_q16[axis] = zero_center ? 0 : static_cast<int32_t>(
        static_cast<uint32_t>(low) + static_cast<uint32_t>(difference >> 1));
    // Odd fixed-point widths use the larger, positive-side half. Computing
    // length from difference >> 1 would lose one word on each odd axis.
    const int32_t half = static_cast<int32_t>(
        static_cast<uint32_t>(high) -
        static_cast<uint32_t>(sphere.center_q16[axis]));
    squared_radius += static_cast<double>(half) * half;
  }
  sphere.radius_q16 = static_cast<int32_t>(
      std::min(std::sqrt(squared_radius), 2147418112.0));
  const int32_t scale = runtime_scale_q16 != 0
                           ? runtime_scale_q16 : definition_scale_q16;
  return scale_object_projection_sphere_q16(sphere, scale);
}

ObjectProjectionSphere scale_object_projection_sphere_q16(
    ObjectProjectionSphere sphere, int32_t scale_q16) {
  if (sphere.valid && scale_q16 != 0) {
    for (int32_t &center : sphere.center_q16)
      center = opennova::world::retail_q16_mul_rhu(center, scale_q16);
    sphere.radius_q16 =
        opennova::world::retail_q16_mul_rhu(sphere.radius_q16, scale_q16);
  }
  return sphere;
}

ObjectProjectionSphere person_projection_sphere_q16(
    int32_t entity_bound_radius_q16, bool parachute_deployed,
    int32_t parachute_model_radius_q16) {
  ObjectProjectionSphere sphere;
  sphere.valid = true;
  sphere.radius_q16 = parachute_deployed
                         ? parachute_model_radius_q16 : entity_bound_radius_q16;
  return sphere;
}

// [orig: Model_SelectRlodLevel @ 0x5c3b20 — the threshold walk and the
//  coarsest-slot back-off; the fraction it also stores (@ 0x5c3bb3/0x5c3bc2
//  -> dword_29ACD9C) is dead retail data with no live reader and is not
//  reproduced]
ObjectLodSelection select_object_lod(const std::vector<int32_t> &thresholds_q16,
                                     int32_t projected_radius_q16,
                                     float projection_scale,
                                     const std::vector<bool> &available) {
  ObjectLodSelection result;
  if (thresholds_q16.empty()) {
    return result;
  }
  const int level_count = static_cast<int>(thresholds_q16.size());
  // Rows beyond the authored table read as zero, like the unused slots of
  // retail's fixed-size per-model threshold array.
  const auto threshold_at = [&](int index) -> int32_t {
    return index >= 0 && index < level_count
               ? thresholds_q16[static_cast<std::size_t>(index)]
               : 0;
  };

  // scaledDist = viewDist * flt_298055C, truncated toward zero by the
  // integer compare (_ftol2_sse) [orig: @ 0x5c3b27..0x5c3b4a].
  const double scaled = static_cast<double>(projected_radius_q16) *
                        static_cast<double>(projection_scale);
  const double bounded = std::clamp(
      scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
      static_cast<double>(std::numeric_limits<int32_t>::max()));
  result.scaled_projected_radius_q16 = static_cast<int32_t>(bounded);

  // The walk starts at row 1 and advances while the scaled radius is at or
  // below the next row; the index is clamped to the final row
  // [orig: @ 0x5c3b45..0x5c3b5a].
  int selected = 0;
  while (selected + 1 < level_count &&
         result.scaled_projected_radius_q16 <= thresholds_q16[selected + 1]) {
    ++selected;
  }

  // Coarsest-slot back-off: on the final row (or a row whose next threshold
  // is zero) the UNSCALED radius must also exceed the row's threshold, or
  // the level one finer is drawn [orig: @ 0x5c3b88..0x5c3b9b].
  if (selected > 0 &&
      (selected == level_count - 1 || threshold_at(selected + 1) == 0) &&
      projected_radius_q16 > threshold_at(selected)) {
    --selected;
    result.backed_off = true;
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
  return result;
}

// [orig: Terrain_RenderSceneWithReflection @ 0x5c940c..0x5c9468: fild the
//  detail level, fmul flt_7C59B4, fadd flt_7D76CC, the fixed flt_7C3B90 when
//  the level is 3, fidiv by the viewport width dword_A7837C, fmul flt_7DC188,
//  one float store into flt_298055C. The nonzero-dword_B4C3C0 substitution
//  of flt_7C44B8 (4.0f) @ 0x5c946e..0x5c9478 is the capture-quality
//  override (an input-binding toggle that also forces the 512 reflection
//  target @ 0x5c08d1 and a full cubemap refresh @ 0x6106cb), never reached
//  in ordinary play and not modelled]
float object_lod_frame_scale(int detail_level, float viewport_width) {
  if (viewport_width <= 0.0f) {
    return 0.0f;
  }
  // x87 keeps the intermediate wide; the single rounding is the store.
  const double quality =
      detail_level == kObjectLodDetailLevelMax
          ? static_cast<double>(kObjectLodDetail3Quality)
          : static_cast<double>(detail_level) *
                    static_cast<double>(kObjectLodDetailQualitySlope) +
                static_cast<double>(kObjectLodDetailQualityBias);
  return static_cast<float>(quality / static_cast<double>(viewport_width) *
                            static_cast<double>(kObjectLodReferenceWidth));
}

// [orig: Viewport_TransformAndClipPoint @ 0x41177a..0x4117ec: the
//  depth-versus-radius gate, 2^32 / depth, (focal << 16) * that + 0x8000
//  >> 16, then radius * that + 0x8000 >> 16 into viewport[100]]
int32_t project_bound_sphere_radius_q16(int32_t radius_q16, int32_t depth_q16,
                                        int32_t focal_pixels) {
  if (depth_q16 < radius_q16 || depth_q16 <= 0) {
    return kObjectLodBehindEyeRadiusQ16;
  }
  const int64_t inverse_depth =
      static_cast<int64_t>(0x100000000LL / static_cast<int64_t>(depth_q16));
  const int64_t focal_over_depth =
      ((static_cast<int64_t>(focal_pixels) << 16) * inverse_depth + 0x8000) >>
      16;
  const int64_t projected =
      (static_cast<int64_t>(radius_q16) * focal_over_depth + 0x8000) >> 16;
  return static_cast<int32_t>(std::clamp(
      projected, static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
      static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

} // namespace opennova::renderer
