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
  using opennova::renderer::ObjectLodSelection;
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

  // The frame scale is applied before the integer threshold walk and truncates
  // toward zero like retail's _ftol2_sse call.
  const ObjectLodSelection scaled =
      select_object_lod(thresholds, 6 << 16, 1.25f);
  CHECK(scaled.scaled_projected_radius_q16 == (7 << 16) + (1 << 15));
  CHECK(scaled.lod_index == 0);

  // A missing level falls back toward the finer authored model, matching
  // the submit-time null walk.
  const std::vector<bool> available = {true, false, true};
  CHECK(select_object_lod(thresholds, 5 << 16, 1.0f, available).lod_index == 0);
  CHECK(select_object_lod(thresholds, 2 << 16, 1.0f, available).lod_index == 2);

  CHECK(select_object_lod({}, 100).lod_index == -1);
  CHECK(select_object_lod({0}, 100).lod_index == 0);
  CHECK(std::fabs(select_object_lod(thresholds, 5 << 16).blend_fraction -
                  0.5f) < 0.00001f);

  return failures == 0 ? 0 : 1;
}
