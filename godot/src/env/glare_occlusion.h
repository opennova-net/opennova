#pragma once

#include <runtime/environment/glare_occlusion.h>

#include <utility>

namespace godot {

// The sun-glare terrain-occlusion state of (engine: formats/env/env_celestial.h) (env #14): the engine's ray
// sequence (runtime/environment/glare_occlusion.h advance_glare_occlusion)
// feeds an 8-bit sliding visibility window; brightness steps +-16 (dead-band)
// toward popcount * 32 * fog/1000. All math lives in the engine; the shell
// answers the terrain line of sight for the gate ray and the entities' for the
// two fine rays. A C++-only device helper
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

	// One frame: the engine's coarse gate ray over `p_terrain_clear` and its
	// two jittered fine rays over `p_entities_clear` (mission points; true =
	// nothing blocks), then the window tick at the frame's fog distance.
	template <typename TerrainClear, typename EntitiesClear>
	void advance(const opennova::env::Vec3 &p_cam_mission,
			const opennova::env::Vec3 &p_sun_mission, float p_fog_distance,
			TerrainClear &&p_terrain_clear, EntitiesClear &&p_entities_clear) {
		opennova::env::advance_glare_occlusion(state, p_cam_mission, p_sun_mission,
				p_fog_distance, std::forward<TerrainClear>(p_terrain_clear),
				std::forward<EntitiesClear>(p_entities_clear));
	}

	// The hysteresis brightness 0..256 [orig: g_GlareOcclusionBrightness, see docs/env/env-tod-re.md].
	int get_brightness() const;

	// The 8-sample sliding visibility window bits (diagnostics)
	// (engine: formats/env/env_celestial.h).
	int get_window() const;
};

} // namespace godot
