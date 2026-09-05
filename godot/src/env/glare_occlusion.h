#pragma once

#include <godot_cpp/variant/vector3.hpp>

#include <formats/env/env_celestial.h>

#include <cstdint>

namespace godot {

// The sun-glare terrain-occlusion state of [orig: render_skybox_sun_glow
// @ 0x5acd00, see docs/env/env-tod-re.md] (env #14): two jittered rays per frame feed an 8-bit sliding
// visibility window; brightness steps +-16 (dead-band) toward
// popcount * 32 * fog/1000. All math lives in engine/formats/env; the shell casts the
// two rays (camera -> camera + sun_dir * 1024 + jitter) against terrain and
// reports visibility. A C++-only device helper (Celestial's member — its
// ClassDB row died with the ADR 0043 d10 env-core sweep; env_render_unit pins
// the window, hysteresis and jitter vectors). RE record: docs/env/env-tod-re.md
// "Celestial bodies".
class GlareOcclusion {
private:
	opennova::env::GlareOcclusionState state;

	Vector3 jitter_to_godot(uint32_t p_index) const;

public:
	// World-space offsets (Godot basis) for this frame's two ray endpoints
	// [orig: jitter @ 0x5ace3b..0x5ace61 — mission Y (north) +-16/+-8
	// (Godot -z), height +-16 (Godot +y), see docs/env/env-tod-re.md].
	Vector3 get_ray_jitter_a() const;
	Vector3 get_ray_jitter_b() const;

	// The witnessed ray length (world units): camera + sun_dir * 1024.
	float get_ray_length() const;

	// Advance one frame with the two samples' visibility.
	void tick(bool p_visible_a, bool p_visible_b, float p_fog_distance);

	// The hysteresis brightness 0..256 [orig: Glare_OcclusionBrightness, see docs/env/env-tod-re.md].
	int get_brightness() const;

	// The 8-sample sliding visibility window bits (diagnostics)
	// [orig: Glare_OcclusionWindow @ 0x27E2E34, see docs/env/env-tod-re.md].
	int get_window() const;

	// The per-frame jitter index [orig: Glare_JitterFrameIndex @ 0x27E5690, see docs/env/env-tod-re.md]
	// BEFORE this frame's two sample increments — the coarse gate ray's
	// start-lift selector (env_celestial.h glare_coarse_start_lift).
	uint32_t get_frame_index() const { return state.jitter_index; }
};

} // namespace godot
