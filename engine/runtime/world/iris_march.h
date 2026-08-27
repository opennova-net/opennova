#pragma once

// The marched iris-exposure feed (D-RLIT-2): three samples along the local
// player's view ray each render frame — the far point 8 u ahead of the camera
// (clipped to the nearest collision), then a thirds march back toward the
// camera — each classified indoor (the hit's pool-2 interior group, or no
// data) or outdoor with a sun level 8 minus one per blocked sun ray at the
// witnessed clip radii. The weather's exposure re-target consumes the levels
// on its next tick; the interior light group the last sample named rides
// along for the renderer.
// [orig: Environment_ApplyFogAndAmbient @ 0x57e512 ->
//  compute_ambient_light_along_direction @ 0x5c7a00 — retail re-targets from
//  the local player's view every render pass]

#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace opennova::world {

struct IrisMarch {
	static constexpr int kSampleCount = 3;
	int32_t samples[kSampleCount] = { 0, 0, 0 };
	int count = 0; // 0 when there is no local player
	// Lighting_SetInteriorLightGroup(building, section) as the last sample
	// left it: the hit's pool-2 entity, or empty / 0 for an outdoor sample.
	EntityHandle interior_group_entity;
	int32_t interior_group_section = 0;
};

// `cam` and `end` are mission fixed (16.16); `end` is the pre-clip far point
// (camera + forward * 8) and is clipped here; `sun` is the sun-ray offset
// (light direction * 200 u). Returns count 0 without a spawned local player.
void compute_iris_march(World &world, CollisionWorld &collision,
		const OcclusionWorld &occlusion, const int32_t cam[3], int32_t end[3],
		const int32_t sun[3], IrisMarch &out);

// The far-point reach and the sun-ray length, world units [orig: the
// (0x80000, 0, 0) forward vector @ 0x5c7a56..0x5c7a6f; end = sample + 200 *
// light_dir @ 0x5c776c..0x5c7780].
inline constexpr float kIrisMarchReachUnits = 8.0f;
inline constexpr float kIrisSunRayUnits = 200.0f;

} // namespace opennova::world
