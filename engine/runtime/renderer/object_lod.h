#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// Retail's sub-pixel floor: the sector-entity draw returns before the RLOD
// selector when the projected bound-sphere radius is at most 0.75 px
// (Q16.16 49152), so such an entity is not drawn at any level.
// [orig: render_sector_entity @ 0x5c42de]
inline constexpr int32_t kObjectLodSubPixelCullQ16 = 49152;

// The projected radius the point projector reports for a sphere whose view
// depth is smaller than its radius (the eye is inside or behind it): a fixed
// 4096 px in Q16.16, which selects the finest level.
// [orig: Viewport_TransformAndClipPoint @ 0x411782..0x411788]
inline constexpr int32_t kObjectLodBehindEyeRadiusQ16 = 0x10000000;

// The highest shipped object-detail profile (the frame scale's fixed-quality
// leg). [orig: Terrain_RenderSceneWithReflection @ 0x5c944c]
inline constexpr int kObjectLodDetailLevelMax = 3;

struct ObjectLodSelection {
  int lod_index = -1;
  int32_t scaled_projected_radius_q16 = 0;
  // The coarsest-slot back-off fired: the scaled walk landed on the final
  // row (or a row whose next threshold is zero) while the UNSCALED radius
  // still exceeds that row's threshold, so the level one finer is drawn.
  bool backed_off = false;
};

// Select an authored object LOD from the retail fine-to-coarse threshold table.
// The input is projected screen radius in Q16.16 (project_bound_sphere_radius_q16
// below). `projection_scale` is the frame's resolution/detail normalization
// (object_lod_frame_scale). `available`, when supplied, models the
// submit-time null-model fallback toward a finer LOD.
//
// The selector returns ONE level and the draw is a hard switch: retail's
// selector also writes an overlap fraction to a global whose only reader is
// a callerless stub, and no submit, collector or flush consumes it, so no
// cross-fade or dual submission exists across an RLOD threshold (D-RORD-11,
// docs/render/render-order-re.md).
// [orig: Model_SelectRlodLevel @ 0x5c3b20 (the level in EAX; the walk
//  @ 0x5c3b45..0x5c3b5a, the back-off @ 0x5c3b88..0x5c3b9b; the dead
//  fraction store @ 0x5c3bb3/0x5c3bc2 -> dword_29ACD9C, read only by the
//  orphan stub @ 0x5c38c0); null fallback at render_sector_entity
//  @ 0x5c4303]
ObjectLodSelection select_object_lod(const std::vector<int32_t> &thresholds_q16,
                                     int32_t projected_radius_q16,
                                     float projection_scale = 1.0f,
                                     const std::vector<bool> &available = {});

// The level an attachment draws at: the parent entity's selected RLOD index,
// clamped to the attachment's own LOD count. The bone callback receives the
// parent's level as its frame index and indexes every overlay model (the
// held weapon, the NVG and binocular items, the mounted child) with
// min(level, count - 1); an attachment never runs its own threshold walk.
// A parent level below zero (nothing selected) reads as the finest level.
// [orig: BoneCallback_org0_World @ 0x4e39c4..0x4e39ce (item overlay),
//  @ 0x4e3b82..0x4e3b8c (NVG), @ 0x4e3c2d..0x4e3c37 (binoculars),
//  @ 0x4e3ce8..0x4e3cf2 (held weapon), @ 0x4e3e34..0x4e3e51 (mounted child)]
inline int attachment_lod_index(int parent_lod_index, int lod_count) {
  if (lod_count <= 0) {
    return -1;
  }
  return std::clamp(parent_lod_index, 0, lod_count - 1);
}

// The per-frame multiplier applied to every projected radius before the
// threshold walk: the detail profile's quality term (detail * 0.33 + 0.34,
// or the fixed 2.0 on detail 3, the highest shipped profile) divided by the
// viewport width in pixels, times the 640-wide reference.
// [orig: Terrain_RenderSceneWithReflection @ 0x5c940c..0x5c9468;
//  Terrain_CollectVisibleEntitiesForReflection @ 0x5c90c3..0x5c90f1]
float object_lod_frame_scale(int detail_level, float viewport_width);

// The projected bound-sphere radius in Q16.16 pixels: the sphere radius
// (Q16.16 world units) times the focal length in pixels over the view depth
// (Q16.16), with retail's two half-unit roundings. A depth smaller than the
// radius reports kObjectLodBehindEyeRadiusQ16.
// [orig: Viewport_TransformAndClipPoint @ 0x41177a..0x4117ec]
int32_t project_bound_sphere_radius_q16(int32_t radius_q16, int32_t depth_q16,
                                        int32_t focal_pixels);

} // namespace opennova::renderer
