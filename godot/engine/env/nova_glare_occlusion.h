#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <env/env_celestial.h>

namespace godot {

// The sun-glare terrain-occlusion state of [orig: render_skybox_sun_glow
// @ 0x5acd00] (env #14): two jittered rays per frame feed an 8-bit sliding
// visibility window; brightness steps +-16 (dead-band) toward
// popcount * 32 * fog/1000. All math lives in libs/env; the shell casts the
// two rays (camera -> camera + sun_dir * 1024 + jitter) against terrain and
// reports visibility. RE record: docs/env/env-tod-re.md "Celestial bodies".
class NovaGlareOcclusion : public RefCounted {
	GDCLASS(NovaGlareOcclusion, RefCounted)

private:
	opennova::env::GlareOcclusionState state;

	Vector3 jitter_to_godot(uint32_t p_index) const;

protected:
	static void _bind_methods();

public:
	// World-space offsets (Godot basis) for this frame's two ray endpoints
	// [orig: jitter @ 0x5ace3b..0x5ace61 — engine Y +-16/+-8 (Godot -x),
	// height +-16 (Godot +y)].
	Vector3 get_ray_jitter_a() const;
	Vector3 get_ray_jitter_b() const;

	// The witnessed ray length (world units): camera + sun_dir * 1024.
	float get_ray_length() const;

	// Advance one frame with the two samples' visibility.
	void tick(bool p_visible_a, bool p_visible_b, float p_fog_distance);

	// The hysteresis brightness 0..256 [orig: Glare_OcclusionBrightness].
	int get_brightness() const;
};

} // namespace godot
