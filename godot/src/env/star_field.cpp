#include "env/star_field.h"

namespace godot {

void StarField::_bind_methods() {
	ClassDB::bind_method(D_METHOD("regenerate", "seed"), &StarField::regenerate);
	ClassDB::bind_method(D_METHOD("get_count"), &StarField::get_count);
}

void StarField::regenerate(int p_seed) {
	prng_state = static_cast<uint32_t>(p_seed == 0 ? 1 : p_seed);
	opennova::env::generate_star_instances(stars, prng_state);
	generated = true;
}

int StarField::get_count() const {
	return opennova::env::kStarInstanceCount;
}

PackedFloat32Array StarField::tick_frame(const Vector3 &p_light_dir_godot) {
	PackedFloat32Array out;
	if (!generated) {
		return out;
	}
	out.resize(opennova::env::kStarInstanceCount * 6);
	float *w = out.ptrw();
	// Both swizzles (render <-> engine fixed) live engine-side in
	// env_celestial.h, which carries the fixed->render witness.
	int32_t light_fp[3];
	opennova::env::star_light_dir_fixed3_from_render(
			static_cast<float>(p_light_dir_godot.x),
			static_cast<float>(p_light_dir_godot.y),
			static_cast<float>(p_light_dir_godot.z), light_fp);
	for (int i = 0; i < opennova::env::kStarInstanceCount; ++i) {
		opennova::env::StarInstance &star = stars[i];
		const bool visible = opennova::env::star_visible_fixed(star, light_fp);
		int32_t brightness = star.brightness;
		if (visible) {
			// (retail: render_star_field @ 0x5adb45 — twinkle inside the
			// visibility branch only, see docs/env/env-tod-re.md)
			brightness = opennova::env::star_twinkle_tick(star, prng_state);
		}
		opennova::env::star_offset_render_float3(star, &w[i * 6]);
		w[i * 6 + 3] = opennova::env::star_billboard_world_size(star);
		w[i * 6 + 4] = static_cast<float>(brightness) / 255.0f;
		w[i * 6 + 5] = visible ? 1.0f : 0.0f;
	}
	return out;
}

} // namespace godot
