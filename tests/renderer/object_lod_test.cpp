#include <runtime/renderer/object_lod.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

} // namespace

int main() {
  using opennova::renderer::kObjectLodBehindEyeRadiusQ16;
  using opennova::renderer::kObjectLodDetailLevelMax;
  using opennova::renderer::kObjectLodSubPixelCullQ16;
  using opennova::renderer::object_lod_frame_scale;
  using opennova::renderer::ObjectLodSelection;
  using opennova::renderer::project_bound_sphere_radius_q16;
  using opennova::renderer::select_object_lod;

  // Retail's runtime table is ordered fine/near -> coarse/far. The first
  // threshold is unused; selection begins at threshold[1]. The selector's
  // input is the projected screen radius in Q16.16, not world distance.
  const std::vector<int32_t> thresholds = {
      0,
      7 << 16,
      3 << 16,
  };

  CHECK(select_object_lod(thresholds, 8 << 16).lod_index == 0);
  CHECK(select_object_lod(thresholds, (7 << 16) + 1).lod_index == 0);
  CHECK(select_object_lod(thresholds, 7 << 16).lod_index == 1);
  CHECK(select_object_lod(thresholds, (3 << 16) + 1).lod_index == 1);
  CHECK(select_object_lod(thresholds, 3 << 16).lod_index == 2);
  CHECK(select_object_lod(thresholds, 0).lod_index == 2);
  CHECK(!select_object_lod(thresholds, 3 << 16).backed_off);

  // The frame scale is applied before the integer threshold walk and truncates
  // toward zero like retail's _ftol2_sse call.
  const ObjectLodSelection scaled =
      select_object_lod(thresholds, 6 << 16, 1.25f);
  CHECK(scaled.scaled_projected_radius_q16 == (7 << 16) + (1 << 15));
  CHECK(scaled.lod_index == 0);

  // Coarsest-slot back-off [orig: @ 0x5c3b88..0x5c3b9b]: a scale below 1
  // lands the scaled walk on the final row while the unscaled radius still
  // exceeds that row's threshold, so the level one finer is drawn.
  const ObjectLodSelection backed =
      select_object_lod(thresholds, 4 << 16, 0.667f);
  CHECK(backed.lod_index == 1);
  CHECK(backed.backed_off);
  // After the back-off the far threshold is rescaled by 1/scale, which puts
  // the scaled radius at or below the new row's near threshold: no overlap.
  CHECK(backed.blend_fraction == 0.0f);
  // The same unscaled radius at unit scale reaches row 1 without backing off.
  CHECK(select_object_lod(thresholds, 4 << 16, 1.0f).lod_index == 1);
  CHECK(!select_object_lod(thresholds, 4 << 16, 1.0f).backed_off);
  // An unscaled radius at or below the final row's threshold stays coarsest.
  CHECK(select_object_lod(thresholds, 2 << 16, 0.667f).lod_index == 2);
  CHECK(!select_object_lod(thresholds, 2 << 16, 0.667f).backed_off);
  CHECK(select_object_lod(thresholds, 3 << 16, 0.5f).lod_index == 2);
  // A two-row table backs off from its final row 1 to row 0; row 0 itself
  // never backs off (the lodIndex > 0 gate).
  CHECK(select_object_lod({0, 7 << 16}, 9 << 16, 0.5f).lod_index == 0);
  CHECK(select_object_lod({0, 7 << 16}, 9 << 16, 0.5f).backed_off);
  CHECK(select_object_lod({0, 7 << 16}, 9 << 16, 2.0f).lod_index == 0);
  CHECK(!select_object_lod({0, 7 << 16}, 9 << 16, 2.0f).backed_off);

  // A zero next-slot entry triggers the same back-off short of the final
  // row: the walk stops on row 1 (4 <= 0 fails), row 2 reads zero, and the
  // unscaled radius exceeds row 1's threshold.
  const std::vector<int32_t> zero_gap = {0, 7 << 16, 0, 5 << 16};
  const ObjectLodSelection gap = select_object_lod(zero_gap, 8 << 16, 0.5f);
  CHECK(gap.lod_index == 0);
  CHECK(gap.backed_off);
  CHECK(gap.blend_fraction == 0.0f);
  CHECK(select_object_lod(zero_gap, 6 << 16, 1.0f).lod_index == 1);
  CHECK(!select_object_lod(zero_gap, 6 << 16, 1.0f).backed_off);

  // A missing level falls back toward the finer authored model, matching
  // the submit-time null walk; the fallback runs after the back-off.
  const std::vector<bool> available = {true, false, true};
  CHECK(select_object_lod(thresholds, 5 << 16, 1.0f, available).lod_index == 0);
  CHECK(select_object_lod(thresholds, 2 << 16, 1.0f, available).lod_index == 2);
  CHECK(select_object_lod(thresholds, 4 << 16, 0.667f, available).lod_index ==
        0);

  CHECK(select_object_lod({}, 100).lod_index == -1);
  CHECK(select_object_lod({0}, 100).lod_index == 0);
  CHECK(std::fabs(select_object_lod(thresholds, 5 << 16).blend_fraction -
                  0.5f) < 0.00001f);

  // The frame scale [orig: @ 0x5c940c..0x5c9468]: the highest shipped
  // profile's fixed 2.0 quality over the viewport width, times 640.
  CHECK(kObjectLodDetailLevelMax == 3);
  CHECK(std::fabs(object_lod_frame_scale(3, 640.0f) - 2.0f) < 1.0e-6f);
  CHECK(std::fabs(object_lod_frame_scale(3, 1280.0f) - 1.0f) < 1.0e-6f);
  CHECK(std::fabs(object_lod_frame_scale(3, 1920.0f) - (2.0f / 1920.0f) * 640.0f) <
        1.0e-6f);
  CHECK(object_lod_frame_scale(3, 0.0f) == 0.0f);
  // The lower profiles' quality term detail * 0.33 + 0.34
  // [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90c3..0x5c90d9].
  CHECK(std::fabs(object_lod_frame_scale(2, 640.0f) - 1.0f) < 1.0e-6f);
  CHECK(std::fabs(object_lod_frame_scale(1, 640.0f) - 0.67f) < 1.0e-6f);
  CHECK(std::fabs(object_lod_frame_scale(0, 640.0f) - 0.34f) < 1.0e-6f);
  CHECK(std::fabs(object_lod_frame_scale(2, 1280.0f) - 0.5f) < 1.0e-6f);

  // The projected radius [orig: Viewport_TransformAndClipPoint
  // @ 0x41177a..0x4117ec]: radius * focal / depth with the witnessed
  // truncating 2^32/depth and the two +0x8000 roundings.
  CHECK(project_bound_sphere_radius_q16(2 << 16, 10 << 16, 500) == 6553000);
  CHECK(project_bound_sphere_radius_q16(1 << 16, 100 << 16, 342) == 224010);
  CHECK(project_bound_sphere_radius_q16(3 << 16, 3 << 16, 342) == 22412970);
  // A depth smaller than the radius reports the fixed behind-eye radius.
  CHECK(project_bound_sphere_radius_q16(3 << 16, (3 << 16) - 1, 342) ==
        kObjectLodBehindEyeRadiusQ16);
  CHECK(project_bound_sphere_radius_q16(1 << 16, 0, 342) ==
        kObjectLodBehindEyeRadiusQ16);
  CHECK(kObjectLodBehindEyeRadiusQ16 == 0x10000000);

  // The sub-pixel floor [orig: render_sector_entity @ 0x5c42de]: 0.75 px.
  CHECK(kObjectLodSubPixelCullQ16 == 49152);
  CHECK(kObjectLodSubPixelCullQ16 == (3 << 16) / 4);
  CHECK(project_bound_sphere_radius_q16(1 << 15, 1000 << 16, 342) == 11115);
  CHECK(project_bound_sphere_radius_q16(1 << 15, 1000 << 16, 342) <=
        kObjectLodSubPixelCullQ16);

  return failures == 0 ? 0 : 1;
}
