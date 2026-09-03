#include "env/glare_occlusion.h"

using namespace godot;

Vector3 GlareOcclusion::jitter_to_godot(uint32_t p_index) const {
	// Mission axes -> Godot world (the (x, z, -y) map every mission
	// position takes): jitter offsets live on mission Y (north) and mission
	// Z (height) [orig: @ 0x5ace3b..0x5ace61, see docs/env/env-tod-re.md], so godot y = +offset_z,
	// godot z = -offset_y. (2026-08-20 correction with the util/axes.h sweep:
	// the earlier mapping sent mission Y to godot -x — a 90-degree-rotated
	// jitter plane; symmetric offsets made it invisible to the window.)
	const opennova::env::GlareRayJitter jitter = opennova::env::glare_ray_jitter(p_index);
	return Vector3(0.0f, jitter.offset_eng_z, -jitter.offset_eng_y);
}

Vector3 GlareOcclusion::get_ray_jitter_a() const {
	return jitter_to_godot(state.jitter_index + 1);
}

Vector3 GlareOcclusion::get_ray_jitter_b() const {
	return jitter_to_godot(state.jitter_index + 2);
}

float GlareOcclusion::get_ray_length() const {
	return 1024.0f; // [orig: sun_dir << 10 @ 0x5acd71/0x5acde8, see docs/env/env-tod-re.md]
}

void GlareOcclusion::tick(bool p_visible_a, bool p_visible_b, float p_fog_distance) {
	opennova::env::glare_occlusion_tick(state, p_visible_a, p_visible_b, p_fog_distance);
}

int GlareOcclusion::get_brightness() const {
	return state.brightness;
}

int GlareOcclusion::get_window() const {
	return static_cast<int>(state.window);
}
