#pragma once

#include <runtime/environment/glare_occlusion.h>

#include <utility>

namespace godot {

// The sun-glare terrain-occlusion state of [orig: Render_SkyboxSunGlow
// @ 0x5acd00, see docs/env/env-tod-re.md] (env #14): the engine's ray
// sequence (runtime/environment/glare_occlusion.h advance_glare_occlusion)
// feeds an 8-bit sliding visibility window; brightness steps +-16 (dead-band)
// toward popcount * 32 * fog/1000. All math lives in the engine; the shell
// answers the terrain line of sight for each ray. A C++-only device helper
// (Celestial's member — its ClassDB row died with the ADR 0043 d10 env-core
// sweep; env_render_unit pins the window, hysteresis and jitter vectors, the
// glare_occlusion ctest the ray sequence). RE record: docs/env/env-tod-re.md
// "Celestial bodies".
class GlareOcclusion {
private:
	opennova::env::GlareOcclusionState state;

public:
	// The ray length (world units): camera + sun_dir * kGlareRayLength.
	float get_ray_length() const { return opennova::env::kGlareRayLength; }

	// One frame: the engine's coarse gate ray and two jittered fine rays over
	// `p_segment_clear(from, to)` (mission points; true = nothing blocks),
	// then the window tick at the frame's fog distance.
	template <typename SegmentClear>
	void advance(const opennova::env::Vec3 &p_cam_mission,
			const opennova::env::Vec3 &p_sun_mission, float p_fog_distance,
			SegmentClear &&p_segment_clear) {
		opennova::env::advance_glare_occlusion(state, p_cam_mission, p_sun_mission,
				p_fog_distance, std::forward<SegmentClear>(p_segment_clear));
	}

	// The hysteresis brightness 0..256 [orig: g_GlareOcclusionBrightness, see docs/env/env-tod-re.md].
	int get_brightness() const;

	// The 8-sample sliding visibility window bits (diagnostics)
	// [orig: g_GlareOcclusionWindow @ 0x27E2E34, see docs/env/env-tod-re.md].
	int get_window() const;
};

} // namespace godot
