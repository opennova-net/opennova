// The sun-glare occlusion leg's per-frame ray sequence (env #14) -- the engine
// half of the shell's glare applier. The window, the dead-band hysteresis, the
// ray jitter and the coarse start lift it drives are env_celestial.h's
// (engine/formats/env); the embedder answers two lines of sight between two
// points, the terrain's and the entities' (world::CollisionWorld::
// sun_ray_clear_of_entities), and reads the brightness back.
// RE record: docs/env/env-tod-re.md "Celestial bodies".
#pragma once

#include <formats/env/env.h> // Vec3
#include <formats/env/env_celestial.h>

namespace opennova::env {

// The glare ray length in world units: camera + sun_dir * 1024
// [orig: Render_SkyboxSunGlow @ 0x5acdaa..0x5acde8 -- sun_dir << 10].
inline constexpr float kGlareRayLength = 1024.0f;

// One frame of the glare occlusion [orig: Render_SkyboxSunGlow @ 0x5acd9e..0x5acf7f]:
// - one end for every ray: camera + sun * kGlareRayLength, from the camera's
//   own height [orig: Render_SkyboxSunGlow @ 0x5acdaa..0x5acde8];
// - ONE coarse TERRAIN gate ray to it from the camera lifted by
//   glare_coarse_start_lift (the frame index BEFORE this frame's two samples)
//   [orig: Render_SkyboxSunGlow @ 0x5acde4; Terrain_RaycastLoResNoNormal
//   @ 0x610860 (the call @ 0x5acdec)];
// - only when it is clear, two fine ENTITY rays from the exact camera
//   [orig: Render_SkyboxSunGlow @ 0x5ace37, start.z reset]: each sample adds
//   its jitter (glare_ray_jitter of jitter_index + 1, then + 2) to the one end
//   in place, so the second sample's end carries both jitters
//   [orig: Render_SkyboxSunGlow @ 0x5ace3d..0x5ace61], and is clear when no
//   entity blocks it [orig: Physics_RaycastIntContext @ 0x5385e0 (the call
//   @ 0x5ace79); Physics_RaycastAgainstPoolByIndex @ 0x539530 (the calls
//   @ 0x5aceae / @ 0x5acec6 / @ 0x5acede), pools 2, 1, 0]; the fine rays test
//   no terrain;
// - then the window tick at the frame's fog distance (glare_occlusion_tick).
// Mission axes throughout (z = height); `terrain_clear(from, to)` and
// `entities_clear(from, to)` are the embedder's two lines of sight between two
// mission points (true = clear).
template <typename TerrainClear, typename EntitiesClear>
inline void advance_glare_occlusion(GlareOcclusionState &state, const Vec3 &cam_m,
		const Vec3 &sun_m, float fog_distance_world, TerrainClear &&terrain_clear,
		EntitiesClear &&entities_clear) {
	Vec3 end{cam_m.x + sun_m.x * kGlareRayLength, cam_m.y + sun_m.y * kGlareRayLength,
			cam_m.z + sun_m.z * kGlareRayLength};
	const Vec3 coarse_from{cam_m.x, cam_m.y,
			cam_m.z + glare_coarse_start_lift(state.jitter_index)};
	const bool coarse_clear = terrain_clear(coarse_from, end);
	bool visible[2] = {false, false};
	if (coarse_clear) {
		for (uint32_t sample = 0; sample < 2; ++sample) {
			const GlareRayJitter jitter = glare_ray_jitter(state.jitter_index + 1 + sample);
			end.y += jitter.offset_eng_y;
			end.z += jitter.offset_eng_z;
			visible[sample] = entities_clear(cam_m, end);
		}
	}
	glare_occlusion_tick(state, visible[0], visible[1], fog_distance_world);
}

} // namespace opennova::env
