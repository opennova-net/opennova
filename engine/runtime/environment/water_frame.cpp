#include <runtime/environment/water_frame.h>

#include <formats/env/env_water_render.h>
#include <formats/env/env_weather.h>
#include <runtime/renderer/triangle_strip.h>

#include <algorithm>
#include <iterator>

namespace opennova::env {

WaterHeightRungs mission_water_rungs(const BmsEnvOverrides &overrides,
		float terrain_height, bool has_loaded_terrain) {
	return water_height_rungs(overrides.has_water_height
					? std::optional<float>(overrides.water_height * kWaterHeightUnit)
					: std::nullopt,
			terrain_height, has_loaded_terrain);
}

WaterHeightRungs water_height_rungs(std::optional<float> mission_override,
		float terrain_height, bool has_loaded_terrain) {
	WaterHeightRungs rungs;
	rungs.has_mission_override = mission_override.has_value();
	rungs.mission_override = mission_override.value_or(0.0f);
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

ResolvedWaterHeight mission_water_rung(const BmsEnvOverrides &overrides, const Config &environment) {
	if (overrides.has_water_height) {
		return { overrides.water_height * kWaterHeightUnit, WaterRung::Mission };
	}
	if (environment.water_height_set) {
		return { environment.water_height * kWaterHeightUnit, WaterRung::Environment };
	}
	return { 0.0f, WaterRung::None };
}

WaterFrameInputs build_water_frame_inputs(const EnvironmentState *env,
		float murk_default) {
	WaterFrameInputs inputs;
	inputs.murk = murk_default;
	const float overcast = env != nullptr ? env->overcast_blend() : 0.0f;
	if (env == nullptr || !env->is_loaded()) {
		inputs.pass_fog_end_above = fog_end_above_water(inputs.fog_end, overcast);
		inputs.pass_fog_end_underwater = inputs.pass_fog_end_above;
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
	// [orig: Environment_GetFogEndDistance @ 0x57e426 (above) / @ 0x57e40d
	// (underwater: the murk visibility curve of the loaded .env's water_murk)]
	inputs.pass_fog_end_above = fog_end_above_water(inputs.fog_end, overcast);
	inputs.pass_fog_end_underwater = fog_end_underwater(env->water_murk());
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

WaterFramePasses water_frame_passes(float eye_y, float water_height, bool water_pass_active) {
	WaterFramePasses passes;
	passes.sides = water_surface_sides(eye_y, water_height);
	passes.beauty = water_pass_active && (passes.sides.above || passes.sides.underwater);
	passes.night_vision = passes.sides.above;
	passes.refresh_mirror = water_pass_active;
	return passes;
}

WaterFramePasses inset_water_frame_passes(float eye_y, float water_height,
		bool water_pass_active, bool view_water_letter) {
	WaterFramePasses passes;
	passes.sides = water_surface_sides(eye_y, water_height);
	passes.refresh_mirror = water_pass_active;
	passes.beauty = water_pass_active && view_water_letter &&
			(passes.sides.above || passes.sides.underwater);
	return passes;
}

uint8_t underwater_murk_overlay_alpha_byte(float murk) {
	// Do not add a lower clamp: the retail parser constrains only the upper
	// bound, and the packed ARGB destination takes the resulting low byte.
	return static_cast<uint8_t>(128 + static_cast<int>(murk * 96.0f));
}

std::vector<uint32_t> water_strip_triangle_list(int row_count) {
	std::vector<uint32_t> list;
	for (const WaterStripBatch &batch : water_strip_batches(row_count)) {
		renderer::triangle_strip_to_list(kWaterStripIndexTable,
				std::min(static_cast<std::size_t>(std::max(batch.vertex_count, 0)),
						std::size(kWaterStripIndexTable)),
				static_cast<uint32_t>(batch.first_row * 3), list);
	}
	return list;
}

} // namespace opennova::env
