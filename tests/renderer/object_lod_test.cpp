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
  using opennova::renderer::attachment_lod_index;
  using opennova::renderer::kObjectLodBehindEyeRadiusQ16;
  using opennova::renderer::kObjectLodDetailLevelMax;
  using opennova::renderer::kObjectLodSubPixelCullQ16;
  using opennova::renderer::object_lod_frame_scale;
  using opennova::renderer::ObjectLodSelection;
  using opennova::renderer::project_bound_sphere_radius_q16;
  using opennova::renderer::select_object_lod;

  // Full retail Entity_ComputeBoundingSphere execution @0x5C69A0 supplied
  // these literal midpoint/radius and scale-precedence witnesses.
  using opennova::renderer::object_projection_sphere_from_bounds_q16;
  using opennova::renderer::person_projection_sphere_q16;
  const auto offset = object_projection_sphere_from_bounds_q16(
      {-131072, 65536, 196608}, {393216, 327680, 589824});
  CHECK(offset.valid);
  CHECK((offset.center_q16 == std::array<int32_t, 3>{131072, 196608, 393216}));
  CHECK(offset.radius_q16 == 352922);
  const auto odd = object_projection_sphere_from_bounds_q16({-3, -2, 10}, {4, 7, 15});
  CHECK((odd.center_q16 == std::array<int32_t, 3>{0, 2, 12}));
  CHECK(odd.radius_q16 == 7); // max-center uses {4,5,3}, not floor halves {3,4,2}.
  const auto runtime_scaled = object_projection_sphere_from_bounds_q16(
      {-3, -2, 10}, {4, 7, 15}, 98304, 131072);
  CHECK((runtime_scaled.center_q16 == std::array<int32_t, 3>{0, 3, 18}));
  CHECK(runtime_scaled.radius_q16 == 11);
  const auto definition_scaled = object_projection_sphere_from_bounds_q16(
      {-3, -2, 10}, {4, 7, 15}, 0, 131072);
  CHECK((definition_scaled.center_q16 == std::array<int32_t, 3>{0, 4, 24}));
  CHECK(definition_scaled.radius_q16 == 14);
  // The eweap-powerup leg zeroes the center before measuring, so the halves
  // are the maxima {4,7,15}: sqrt(290) -> 17, then the same RHU scale.
  // [orig: Entity_InitFromModel @0x40df06..0x40df16, @0x40df66..0x40dfac]
  const auto zero_centered = object_projection_sphere_from_bounds_q16(
      {-3, -2, 10}, {4, 7, 15}, 0, 0, true);
  CHECK((zero_centered.center_q16 == std::array<int32_t, 3>{0, 0, 0}));
  CHECK(zero_centered.radius_q16 == 17);
  const auto zero_centered_scaled = object_projection_sphere_from_bounds_q16(
      {-3, -2, 10}, {4, 7, 15}, 98304, 131072, true);
  CHECK((zero_centered_scaled.center_q16 == std::array<int32_t, 3>{0, 0, 0}));
  CHECK(zero_centered_scaled.radius_q16 == 26);
  const auto inverted = object_projection_sphere_from_bounds_q16(
      {0x40000000, 0x40000000, 0x40000000},
      {-0x40000000, -0x40000000, -0x40000000});
  CHECK((inverted.center_q16 == std::array<int32_t, 3>{0, 0, 0}));
  CHECK(inverted.radius_q16 == 1859775393);
  const auto person = person_projection_sphere_q16(0x19002);
  CHECK(person.valid && person.radius_q16 == 0x19002);
  CHECK((person.center_q16 == std::array<int32_t, 3>{0, 0, 0}));
  CHECK(person_projection_sphere_q16(0x19002, true, 0x78000).radius_q16 == 0x78000);

  // The runtime table: each level's RMDL pixel count shifted into Q16.16, in
  // that level's own slot [orig: ThreediGp_LoadFromFile @ 0x5b5bdf..0x5b5be5].
  using opennova::renderer::rlod_threshold_q16_from_rmdl;
  CHECK(rlod_threshold_q16_from_rmdl(200) == (200 << 16));
  CHECK(rlod_threshold_q16_from_rmdl(0) == 0);

  // Retail's table is ordered fine/near -> coarse/far and slot i is level i's
  // own threshold: the walk starts at slot 0 and advances while the scaled
  // radius is at or below the slot [orig: Model_SelectRlodLevel
  // @ 0x5c3b3b..0x5c3b5a]. The selector's input is the projected screen
  // radius in Q16.16, not world distance.
  const std::vector<int32_t> thresholds = {
      7 << 16,
      3 << 16,
      0,
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

  // Coarsest-slot back-off [orig: @ 0x5c3b5d..0x5c3b8c]: a scale below 1
  // lands the scaled walk on the final level while the unscaled radius still
  // exceeds the next finer level's threshold, so that finer level is drawn.
  const ObjectLodSelection backed =
      select_object_lod(thresholds, 4 << 16, 0.667f);
  CHECK(backed.lod_index == 1);
  CHECK(backed.backed_off);
  // The same unscaled radius at unit scale reaches level 1 without backing off.
  CHECK(select_object_lod(thresholds, 4 << 16, 1.0f).lod_index == 1);
  CHECK(!select_object_lod(thresholds, 4 << 16, 1.0f).backed_off);
  // An unscaled radius at or below the finer threshold stays coarsest.
  CHECK(select_object_lod(thresholds, 2 << 16, 0.667f).lod_index == 2);
  CHECK(!select_object_lod(thresholds, 2 << 16, 0.667f).backed_off);
  CHECK(select_object_lod(thresholds, 3 << 16, 0.5f).lod_index == 2);
  // A two-level table backs off from its final level 1 to level 0; level 0
  // itself never backs off (the lodIndex > 0 gate).
  CHECK(select_object_lod({7 << 16, 0}, 9 << 16, 0.5f).lod_index == 0);
  CHECK(select_object_lod({7 << 16, 0}, 9 << 16, 0.5f).backed_off);
  CHECK(select_object_lod({7 << 16, 0}, 9 << 16, 2.0f).lod_index == 0);
  CHECK(!select_object_lod({7 << 16, 0}, 9 << 16, 2.0f).backed_off);
  // A zero first slot pins the model to level 0 (the corpus's T[0] = 0
  // tables): no positive radius is at or below it.
  CHECK(select_object_lod({0, 7 << 16}, 1 << 16).lod_index == 0);
  CHECK(select_object_lod({0, 7 << 16}, 1 << 16, 0.25f).lod_index == 0);

  // A zero own-slot entry triggers the same back-off short of the final
  // level: the walk stops on level 1 (4 <= 0 fails), level 1 reads zero,
  // and the unscaled radius exceeds level 0's threshold.
  const std::vector<int32_t> zero_gap = {7 << 16, 0, 5 << 16, 0};
  const ObjectLodSelection gap = select_object_lod(zero_gap, 8 << 16, 0.5f);
  CHECK(gap.lod_index == 0);
  CHECK(gap.backed_off);
  CHECK(select_object_lod(zero_gap, 6 << 16, 1.0f).lod_index == 1);
  CHECK(!select_object_lod(zero_gap, 6 << 16, 1.0f).backed_off);

  // Retail corpus tables at a 1920-wide viewport (frame scale 2/1920*640).
  // Armry01's RMDL rows 200, 60, 20, 0: 400 px draws LOD0, 250 px (scaled
  // ~167) LOD1, 25 px (scaled ~17) walks to LOD3 and backs off to LOD2
  // (unscaled 25 > 20), 10 px stays LOD3. 47gl_3RD's 160, 64, 19, 6 clamps
  // a scaled 3.3 px to its final level.
  const float scale_1920 = object_lod_frame_scale(3, 1920.0f);
  const std::vector<int32_t> armry01 = {
      rlod_threshold_q16_from_rmdl(200), rlod_threshold_q16_from_rmdl(60),
      rlod_threshold_q16_from_rmdl(20), rlod_threshold_q16_from_rmdl(0)};
  CHECK(select_object_lod(armry01, 400 << 16, scale_1920).lod_index == 0);
  CHECK(select_object_lod(armry01, 250 << 16, scale_1920).lod_index == 1);
  const ObjectLodSelection armry_far = select_object_lod(armry01, 25 << 16, scale_1920);
  CHECK(armry_far.lod_index == 2);
  CHECK(armry_far.backed_off);
  CHECK(select_object_lod(armry01, 10 << 16, scale_1920).lod_index == 3);
  const std::vector<int32_t> gl_3rd = {
      rlod_threshold_q16_from_rmdl(160), rlod_threshold_q16_from_rmdl(64),
      rlod_threshold_q16_from_rmdl(19), rlod_threshold_q16_from_rmdl(6)};
  CHECK(select_object_lod(gl_3rd, 5 << 16, scale_1920).lod_index == 3);

  // A missing level falls back toward the finer authored model, matching
  // the submit-time null walk; the fallback runs after the back-off.
  const std::vector<bool> available = {true, false, true};
  CHECK(select_object_lod(thresholds, 5 << 16, 1.0f, available).lod_index == 0);
  CHECK(select_object_lod(thresholds, 2 << 16, 1.0f, available).lod_index == 2);
  CHECK(select_object_lod(thresholds, 4 << 16, 0.667f, available).lod_index ==
        0);

  CHECK(select_object_lod({}, 100).lod_index == -1);
  CHECK(select_object_lod({0}, 100).lod_index == 0);
  CHECK(select_object_lod({200 << 16}, 100).lod_index == 0);

  // Attachments never run the threshold walk: the bone callback indexes the
  // held weapon, the NVG/binocular items and the mounted child with the
  // parent's level clamped to their own count
  // [orig: BoneCallback_org0_World @ 0x4e39c4..0x4e39ce].
  CHECK(attachment_lod_index(0, 3) == 0);
  CHECK(attachment_lod_index(1, 3) == 1);
  CHECK(attachment_lod_index(2, 3) == 2);
  CHECK(attachment_lod_index(5, 3) == 2);
  CHECK(attachment_lod_index(2, 1) == 0);
  CHECK(attachment_lod_index(-1, 2) == 0);
  CHECK(attachment_lod_index(1, 0) == -1);

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
