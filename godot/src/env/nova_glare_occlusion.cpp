#include "env/nova_glare_occlusion.h"

using namespace godot;

void GlareOcclusion::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_ray_jitter_a"), &GlareOcclusion::get_ray_jitter_a);
	ClassDB::bind_method(D_METHOD("get_ray_jitter_b"), &GlareOcclusion::get_ray_jitter_b);
	ClassDB::bind_method(D_METHOD("get_ray_length"), &GlareOcclusion::get_ray_length);
	ClassDB::bind_method(D_METHOD("tick", "visible_a", "visible_b", "fog_distance"),
			&GlareOcclusion::tick);
	ClassDB::bind_method(D_METHOD("get_brightness"), &GlareOcclusion::get_brightness);
}

Vector3 GlareOcclusion::jitter_to_godot(uint32_t p_index) const {
	// Engine axes -> render/Godot basis (Math_FixedPointToFloat3_YNegated
	// @ 0x611210): engine Y -> -x, engine Z (height) -> +y.
	const opennova::env::GlareRayJitter jitter = opennova::env::glare_ray_jitter(p_index);
	return Vector3(-jitter.offset_eng_y, jitter.offset_eng_z, 0.0f);
}

Vector3 GlareOcclusion::get_ray_jitter_a() const {
	return jitter_to_godot(state.jitter_index + 1);
}

Vector3 GlareOcclusion::get_ray_jitter_b() const {
	return jitter_to_godot(state.jitter_index + 2);
}

float GlareOcclusion::get_ray_length() const {
	return 1024.0f; // [orig: sun_dir << 10 @ 0x5acd71/0x5acde8]
}

void GlareOcclusion::tick(bool p_visible_a, bool p_visible_b, float p_fog_distance) {
	opennova::env::glare_occlusion_tick(state, p_visible_a, p_visible_b, p_fog_distance);
}

int GlareOcclusion::get_brightness() const {
	return state.brightness;
}
