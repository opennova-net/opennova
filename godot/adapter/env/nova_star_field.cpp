#include "nova_star_field.h"

namespace godot {

void NovaStarField::_bind_methods() {
	ClassDB::bind_method(D_METHOD("regenerate", "seed"), &NovaStarField::regenerate);
	ClassDB::bind_method(D_METHOD("get_count"), &NovaStarField::get_count);
	ClassDB::bind_method(D_METHOD("tick_frame", "light_dir"), &NovaStarField::tick_frame);
}

void NovaStarField::regenerate(int p_seed) {
	prng_state = static_cast<uint32_t>(p_seed == 0 ? 1 : p_seed);
	opennova::env::generate_star_instances(stars, prng_state);
	generated = true;
}

int NovaStarField::get_count() const {
	return opennova::env::kStarInstanceCount;
}

PackedFloat32Array NovaStarField::tick_frame(const Vector3 &p_light_dir_godot) {
	PackedFloat32Array out;
	if (!generated) {
		return out;
	}
	out.resize(opennova::env::kStarInstanceCount * 6);
	float *w = out.ptrw();
	// godot = (-engY, engZ, engX)/65536 => eng = (-g.x, g.z, g.y) * 65536
	// [orig: Math_FixedPointToFloat3_YNegated @ 0x611210].
	const int32_t light_fp[3] = {
		static_cast<int32_t>(-p_light_dir_godot.x * 65536.0f),
		static_cast<int32_t>(p_light_dir_godot.z * 65536.0f),
		static_cast<int32_t>(p_light_dir_godot.y * 65536.0f),
	};
	for (int i = 0; i < opennova::env::kStarInstanceCount; ++i) {
		opennova::env::StarInstance &star = stars[i];
		const bool visible = opennova::env::star_visible_fixed(star, light_fp);
		int32_t brightness = star.brightness;
		if (visible) {
			// [orig: render_star_field @ 0x5adb45 — twinkle inside the
			// visibility branch only]
			brightness = opennova::env::star_twinkle_tick(star, prng_state);
		}
		const float inv = 1.0f / 65536.0f;
		w[i * 6 + 0] = -static_cast<float>(star.offset_fp[1]) * inv;
		w[i * 6 + 1] = static_cast<float>(star.offset_fp[2]) * inv;
		w[i * 6 + 2] = static_cast<float>(star.offset_fp[0]) * inv;
		// billboard param 12288..13311 in 4096-fixed world units.
		w[i * 6 + 3] = static_cast<float>(star.billboard_param) / 4096.0f;
		w[i * 6 + 4] = static_cast<float>(brightness) / 255.0f;
		w[i * 6 + 5] = visible ? 1.0f : 0.0f;
	}
	return out;
}

} // namespace godot
