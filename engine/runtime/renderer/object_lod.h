#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// Retail's sub-pixel floor: the sector-entity draw returns before the RLOD
// selector when the projected bound-sphere radius is at most 0.75 px
// (Q16.16 49152), so such an entity is not drawn at any level.
// [orig: render_sector_entity @ 0x5c42de]
inline constexpr int32_t kObjectLodSubPixelCullQ16 = 49152;

// The sub-pixel floor as a predicate over a projected radius: TRUE = the
// entity is not drawn. The sector-entity draw tests it before the RLOD walk,
// the person collector before its latch.
// [orig: render_sector_entity `cmp edi,0C000h; jle` @ 0x5c42d8..0x5c42de;
//  collect_visible_entities_for_terrain @ 0x5c8e5e]
inline constexpr bool object_subpixel_culled(int32_t projected_radius_q16) {
  return projected_radius_q16 <= kObjectLodSubPixelCullQ16;
}

// The projected radius the point projector reports for a sphere whose view
// depth is smaller than its radius (the eye is inside or behind it): a fixed
// 4096 px in Q16.16, which selects the finest level.
// [orig: Viewport_TransformAndClipPoint @ 0x411782..0x411788]
inline constexpr int32_t kObjectLodBehindEyeRadiusQ16 = 0x10000000;

// The highest shipped object-detail profile (the frame scale's fixed-quality
// leg). [orig: Terrain_RenderSceneWithReflection @ 0x5c944c]
inline constexpr int kObjectLodDetailLevelMax = 3;
// The special item preloaded for the person flag-0x20 radius substitution.
// [orig: Entity_PreloadSpecialItems @ 0x43C220]
inline constexpr int kParachuteProjectionTypeId = 185;

// Entity-local sphere consumed by the visibility projector, in the source
// model's fixed-point axes. It is distinct from GHDR's origin-centered radius.
struct ObjectProjectionSphere {
  std::array<int32_t, 3> center_q16{};
  int32_t radius_q16 = 0;
  bool valid = false;
};

// Ordinary entities use the CMDL AABB midpoint and max-minus-center diagonal.
// A nonzero runtime scale overrides the definition scale; either applies once
// to the center and radius before the unscaled entity pose transforms them.
// The live producer (entity init) zeroes the center FIRST for eweap powerups
// (type 6 with attrib 0x20), so their halves are the clamped maxima
// themselves; the recompute path keeps the midpoint form.
// [orig: Entity_InitFromModel @ 0x40df06..0x40dfac (zero-center leg
// @ 0x40df06..0x40df16), scale @ 0x40dfd0..0x40e03c;
// Entity_ComputeBoundingSphere @ 0x5C69A0, scale selection @ 0x5C69C4,
// midpoint/diagonal @ 0x5C6A3B..0x5C6AC2, scale @ 0x5C6ACE..0x5C6B52]
ObjectProjectionSphere object_projection_sphere_from_bounds_q16(
    const std::array<int32_t, 3> &minimum,
    const std::array<int32_t, 3> &maximum,
    int32_t runtime_scale_q16 = 0, int32_t definition_scale_q16 = 0,
    bool zero_center = false);

// Apply the entity scale to an already-derived local sphere; zero is unscaled.
ObjectProjectionSphere scale_object_projection_sphere_q16(
    ObjectProjectionSphere sphere, int32_t scale_q16);

// Infantry project the entity position and entity+0 bound radius. A deployed
// parachute substitutes its own model radius; head/body share this one sphere.
// [orig: collect_visible_entities_for_terrain @ 0x5C8C60,
// person radius @ 0x5C8DF3..0x5C8E10, entity-position transform @ 0x5C8E21]
ObjectProjectionSphere person_projection_sphere_q16(
    int32_t entity_bound_radius_q16, bool parachute_deployed = false,
    int32_t parachute_model_radius_q16 = 0);

// The runtime RLOD threshold of one level: the RMDL chunk authors an integer
// pixel count (the dword after the model type), and the model loader stores
// it shifted into Q16.16 in the level's own table slot (model+0x40+4*level).
// Corpus tables descend to zero (Armry01 200, 60, 20, 0).
// [orig: ThreediGp_LoadFromFile @ 0x5b5bdf..0x5b5be5 — `mov ecx,[ecx+4];
//  shl ecx,10h; mov [eax+20h],ecx` into loader+0x44+4*i; the model the
//  wrapper hands out is loader+4 — sub_5B6160 @ 0x5b6273]
inline constexpr int32_t rlod_threshold_q16_from_rmdl(int32_t rmdl_threshold_pixels) {
  return static_cast<int32_t>(static_cast<uint32_t>(rmdl_threshold_pixels) << 16);
}

struct ObjectLodSelection {
  int lod_index = -1;
  int32_t scaled_projected_radius_q16 = 0;
  // The coarsest-slot back-off fired: the scaled walk landed on the final
  // level (or a level whose own threshold is zero) while the UNSCALED radius
  // still exceeds the next finer level's threshold, so that finer level is
  // drawn.
  bool backed_off = false;
};

// Select an authored object LOD from the retail fine-to-coarse threshold table.
// thresholds_q16[i] is level i's own threshold (rlod_threshold_q16_from_rmdl):
// level i draws while the scaled radius exceeds it and the level before did
// not claim the radius, so a first slot of zero pins the model to level 0.
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
//  from slot 0 @ 0x5c3b3b..0x5c3b5a, the back-off @ 0x5c3b5d..0x5c3b8c; the dead
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

// The viewport's focal length in pixels: half the viewport WIDTH over the
// tangent of half the HORIZONTAL field of view, rounded half up.
// [orig: Viewport_BuildProjectionMatrix @ 0x410fb0 — (fov >> 1) * dbl_7C3620
//  (degrees Q16 -> radians) @ 0x410fc0..0x410fdb, the width right-left+1
//  @ 0x410fc3..0x410fd3, width * 0.5 / fptan + 0.5 -> _ftol2_sse
//  @ 0x410fe1..0x410ff7, stored to viewport+0x40 @ 0x4110e1]
int32_t object_lod_focal_pixels(float viewport_width, double tan_half_horizontal);

// The projected bound-sphere radius in Q16.16 pixels: the sphere radius
// (Q16.16 world units) times the focal length in pixels over the view depth
// (Q16.16), with retail's two half-unit roundings. A depth smaller than the
// radius reports kObjectLodBehindEyeRadiusQ16.
// [orig: Viewport_TransformAndClipPoint @ 0x41177a..0x4117ec]
int32_t project_bound_sphere_radius_q16(int32_t radius_q16, int32_t depth_q16,
                                        int32_t focal_pixels);

} // namespace opennova::renderer
