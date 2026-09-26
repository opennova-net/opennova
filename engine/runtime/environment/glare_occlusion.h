// The sun-glare occlusion leg's per-frame ray sequence (env #14, closed) --
// the engine half of the shell's glare applier. The window, the dead-band
// hysteresis, the ray jitter and the coarse start lift it drives are
// env_celestial.h's (engine/formats/env); the embedder only answers the
// terrain line of sight between two points and reads the brightness back.
// RE record: docs/env/env-tod-re.md "Celestial bodies".
#pragma once

#include <formats/env/env.h> // Vec3
#include <formats/env/env_celestial.h>

namespace opennova::env {

// The glare ray length in world units: camera + sun_dir * 1024
// [orig: render_skybox_sun_glow @ 0x5acd71/0x5acde8 -- sun_dir << 10].
inline constexpr float kGlareRayLength = 1024.0f;

// One frame of the glare occlusion [orig: render_skybox_sun_glow
// @ 0x5acd9e..0x5acf7f -- the fine rays' entity leg keeps the documented
// sun-occlusion statics posture (render-lighting-re.md D-RLIT-2/D-RLIT-3),
// see docs/env/env-tod-re.md]: ONE coarse unjittered gate ray from the camera
// lifted by glare_coarse_start_lift (the frame index BEFORE this frame's two
// samples); only when it is clear, the two jittered fine rays from the exact
// camera height (sample indices jitter_index + 1 and + 2, glare_ray_jitter);
// then the window tick at the frame's fog distance (glare_occlusion_tick).
// Every ray runs camera -> camera + sun * kGlareRayLength (+ its jitter).
// Mission axes throughout (z = height); `segment_clear(from, to)` is the
// embedder's terrain line of sight between two mission points (true = clear).
template <typename SegmentClear>
inline void advance_glare_occlusion(GlareOcclusionState &state, const Vec3 &cam_m,
		const Vec3 &sun_m, float fog_distance_world, SegmentClear &&segment_clear) {
	const auto ray_end = [&sun_m](const Vec3 &from, float jitter_y, float jitter_z) {
		return Vec3{from.x + sun_m.x * kGlareRayLength,
				from.y + sun_m.y * kGlareRayLength + jitter_y,
				from.z + sun_m.z * kGlareRayLength + jitter_z};
	};
	const Vec3 coarse_from{cam_m.x, cam_m.y,
			cam_m.z + glare_coarse_start_lift(state.jitter_index)};
	const bool coarse_clear = segment_clear(coarse_from, ray_end(coarse_from, 0.0f, 0.0f));
	bool visible_a = false;
	bool visible_b = false;
	if (coarse_clear) {
		const GlareRayJitter jitter_a = glare_ray_jitter(state.jitter_index + 1);
		const GlareRayJitter jitter_b = glare_ray_jitter(state.jitter_index + 2);
		visible_a = segment_clear(cam_m,
				ray_end(cam_m, jitter_a.offset_eng_y, jitter_a.offset_eng_z));
		visible_b = segment_clear(cam_m,
				ray_end(cam_m, jitter_b.offset_eng_y, jitter_b.offset_eng_z));
	}
	glare_occlusion_tick(state, visible_a, visible_b, fog_distance_world);
}

} // namespace opennova::env
