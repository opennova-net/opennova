#include "environment/water_frame.h"

#include <env/env_weather.h>

namespace opennova::env {

float resolve_water_height(const WaterHeightRungs &rungs,
		const EnvironmentState *env, float current) {
	if (rungs.has_mission_override) {
		return rungs.mission_override;
	}
	if (rungs.terrain_height != 0.0f) {
		return rungs.terrain_height;
	}
	if (env != nullptr && env->has_water_height()) {
		// .env water_height is stored <<15 by the engine — half world units,
		// same convention as the terrain value
		// [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
		return env->water_height() * 0.5f;
	}
	const bool has_loaded_env = env != nullptr && env->is_loaded();
	if (rungs.has_loaded_terrain || has_loaded_env) {
		return 0.0f;
	}
	return current;
}

WaterFrameInputs build_water_frame_inputs(const EnvironmentState *env,
		float murk_default) {
	WaterFrameInputs inputs;
	inputs.murk = murk_default;
	if (env == nullptr || !env->is_loaded()) {
		return inputs;
	}
	inputs.env_loaded = true;
	const Rgb combined =
			combine_terrain_light(env->sun_light(), env->sky_ambient());
	inputs.lit = lit_water_color(env->water_color(), combined);
	inputs.fog_end = env->fog_level();
	if (env->config() != nullptr) {
		inputs.murk = env->config()->water_murk;
	}
	return inputs;
}

uint8_t underwater_murk_overlay_alpha_byte(float murk) {
	// Do not add a lower clamp: the retail parser constrains only the upper
	// bound, and the packed ARGB destination takes the resulting low byte.
	return static_cast<uint8_t>(128 + static_cast<int>(murk * 96.0f));
}

} // namespace opennova::env
