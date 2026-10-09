#include <runtime/environment/water_frame.h>

#include <formats/env/env_weather.h>

namespace opennova::env {

WaterHeightRungs mission_water_rungs(const BmsEnvOverrides &overrides,
		float terrain_height, bool has_loaded_terrain) {
	WaterHeightRungs rungs;
	rungs.has_mission_override = overrides.has_water_height;
	rungs.mission_override = overrides.has_water_height ? overrides.water_height * kWaterHeightUnit : 0.0f;
	rungs.terrain_height = terrain_height;
	rungs.has_loaded_terrain = has_loaded_terrain;
	return rungs;
}

ResolvedWaterHeight resolve_water_rung(const WaterHeightRungs &rungs,
		const EnvironmentState *env, float current) {
	if (rungs.has_mission_override) {
		return {rungs.mission_override, WaterRung::Mission};
	}
	if (env != nullptr && env->has_water_height()) {
		// .env water_height is stored <<15 by the engine — half world units,
		// same convention as the terrain value; the .env's line writes after
		// the .trn's [orig: TimeOfDay_ParseProperty @ 0x57cb4e].
		return {env->water_height() * kWaterHeightUnit, WaterRung::Environment};
	}
	if (rungs.terrain_height != 0.0f) {
		return {rungs.terrain_height, WaterRung::Terrain};
	}
	const bool has_loaded_env = env != nullptr && env->is_loaded();
	if (rungs.has_loaded_terrain || has_loaded_env) {
		return {0.0f, WaterRung::None};
	}
	return {current, WaterRung::None};
}

float resolve_water_height(const WaterHeightRungs &rungs,
		const EnvironmentState *env, float current) {
	return resolve_water_rung(rungs, env, current).height;
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

WaterSurfaceSides water_surface_sides(float eye_y, float water_height) {
	// [orig: Render_WaterSurface @ 0x5c32f6 (underwater: cmp cam.z, wh;
	// jge skip) / @ 0x5c3304 (above: cmp cam.z, wh; jle skip)].
	WaterSurfaceSides sides;
	sides.above = eye_y > water_height;
	sides.underwater = eye_y < water_height;
	return sides;
}

uint8_t underwater_murk_overlay_alpha_byte(float murk) {
	// Do not add a lower clamp: the retail parser constrains only the upper
	// bound, and the packed ARGB destination takes the resulting low byte.
	return static_cast<uint8_t>(128 + static_cast<int>(murk * 96.0f));
}

} // namespace opennova::env
