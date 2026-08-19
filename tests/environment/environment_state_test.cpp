// EnvironmentState + WeatherRuntime (engine/runtime/environment): the state
// owner and the weather embedding ported from the nova_environment.gd /
// nova_weather.gd shell scripts (2026-08-09 de-scripting). Pins the mission
// clock views, the weather-driven targets/currents split, the NVG rewrite,
// generation discipline, the reset/prewarm epoch, the network wire units, and
// the shader-global publication policy. RE record: docs/env/env-tod-re.md.
#include <environment/environment_state.h>
#include <environment/water_frame.h>
#include <environment/weather_runtime.h>
#include <env/env_weather.h>

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

bool rgb_near(const opennova::env::Rgb &actual,
		const opennova::env::Rgb &expected, float epsilon = 0.0001f) {
	return near(actual.r, expected.r, epsilon) &&
			near(actual.g, expected.g, epsilon) &&
			near(actual.b, expected.b, epsilon);
}

// A minimal loaded config: envscale 0.5, odd global bytes (truncation pins),
// one noon keyframe so TOD interpolation is live.
opennova::env::Config make_config() {
	opennova::env::Config cfg = opennova::env::make_default_config();
	cfg.envscale = 0.5f;
	cfg.ceiling_rgb = {71.0f / 255.0f, 61.0f / 255.0f, 51.0f / 255.0f};
	cfg.fog_level = 640.0f;
	cfg.sky_height = 175.0f;
	opennova::env::Keyframe noon;
	noon.time = 1200;
	noon.sun = {0.9f, 0.85f, 0.75f};
	noon.ground = {0.4f, 0.45f, 0.55f};
	noon.fog = {0.25f, 0.35f, 0.45f};
	noon.sky = {0.3f, 0.4f, 0.6f};
	noon.moon = {0.1f, 0.1f, 0.2f};
	cfg.keyframes.clear();
	cfg.keyframes.push_back(noon);
	return cfg;
}

} // namespace

int main() {
	using opennova::env::EnvironmentState;
	using opennova::env::WeatherRuntime;

	bool ok = true;

	// --- HHMM / minute / fixed24 clock views --------------------------------
	ok &= expect(near(EnvironmentState::minute_of_day_to_hhmm(750.0f), 1230.0f),
			"750 minutes reads 12:30");
	ok &= expect(near(EnvironmentState::hhmm_to_minute_of_day(1230.0f), 750.0f),
			"12:30 is minute 750");
	ok &= expect(
			near(EnvironmentState::fixed24_to_hhmm(12 << 24), 1200.0f),
			"12h fixed24 reads 12:00");
	ok &= expect(near(EnvironmentState::mission_start_time_hhmm(0x0C80),
						  1230.0f),
			"Q8.8 12.5h widens to the 12:30 display clock");

	// --- mission clock over the exact tod_clock increments ------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.configure_mission_clock(0x0C00, 1440);
		ok &= expect(env.mission_time_fixed24() == (12 << 24),
				"the Q8.8 noon start widens into the 8.24 accumulator");
		const int per_tick = env.mission_advance_per_tick();
		ok &= expect(per_tick == 0x18000000 / (3720 * 1440),
				"the witnessed per-tick increment");
		env.advance_mission_clock(5);
		ok &= expect(env.mission_time_fixed24() == (12 << 24) + 5 * per_tick,
				"five ticks advance exactly five increments");
		ok &= expect(env.debug_set_mission_minute_of_day(750.0f),
				"in-range debug minute accepted");
		ok &= expect(!env.debug_set_mission_minute_of_day(1440.0f),
				"a full day is out of the debug slider range");
		ok &= expect(near(env.mission_minute_of_day(), 750.0f),
				"the debug write moves the minute view");
	}

	// --- the weather-driven targets/currents split --------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		// Standalone: update_tod seeded the currents from the keyframe.
		ok &= expect(rgb_near(env.fill_light(), env.fill_light_target()),
				"standalone owners seed currents from the keyframe targets");
		// Claim the currents, scrub, and verify only targets move.
		env.set_weather_driven(true);
		const opennova::env::Rgb held = env.sun_light();
		env.set_sun_light({0.11f, 0.22f, 0.33f});
		env.set_time_of_day(1200.0f); // per-tick TOD recompute
		ok &= expect(rgb_near(env.sun_light(), {0.11f, 0.22f, 0.33f}),
				"weather-driven TOD recomputes never clobber written-back "
				"currents (the 2026-07-13 black-flicker split)");
		(void)held;
	}

	// --- envscale global view + NVG rewrite + generation discipline ---------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		// 71 * .5 truncates to 35 [orig: Color_ScaleRGBAndPack @ 0x57f890].
		ok &= expect(near(env.ceiling_color_target().r, 35.0f / 255.0f),
				"envscale truncates at the byte quantization");
		const int64_t gen = env.env_generation();
		env.set_fill_light({0.8f, 0.4f, 0.2f});
		env.set_color_src_gain({0.5f, 0.25f, 0.75f});
		ok &= expect(env.env_generation() == gen + 2,
				"each changed writeback bumps once");
		env.set_fill_light({0.8f, 0.4f, 0.2f});
		ok &= expect(env.env_generation() == gen + 2,
				"a settled writeback stops bumping");
		ok &= expect(env.set_nvg_view(true, 2), "NVG change reported");
		// f = (2+1)*.2 = .6: c' = c*.15 + gain*.06.
		ok &= expect(near(env.fill_light().r, 0.8f * 0.15f + 0.5f * 0.06f),
				"the NVG hemisphere rewrite (modulator*f/640 as gain*f/10)");
		ok &= expect(!env.set_nvg_view(true, 2), "identical NVG is idempotent");
	}

	// --- per-scene-pass underwater fog --------------------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		const opennova::env::SceneFogValues dry =
				env.build_scene_fog(false);
		const opennova::env::SceneFogValues underwater =
				env.build_scene_fog(true);
		ok &= expect(rgb_near(dry.color, env.fog_color()) &&
					near(dry.end, env.fog_end_distance()) &&
					dry.type == env.fog_type(),
				"the dry scene pass preserves current weather fog");
		ok &= expect(rgb_near(underwater.color,
					env.frame_clear_color_for(false, false)),
				"the underwater scene fog target is Env_WaterColorLit");
		ok &= expect(near(underwater.end,
					opennova::env::fog_end_underwater(cfg.water_murk)) &&
					near(underwater.start, 0.5f) && underwater.type == 1,
				"underwater scene fog is murk-derived linear fog");

		opennova::env::WorldLightValues object_values;
		ok &= expect(env.build_light_values({}, object_values, true),
				"a loaded underwater pass builds object values");
		const opennova::env::TerrainEnvUniforms terrain_values =
				env.build_terrain_uniforms(true);
		const opennova::env::EnvShaderGlobals shader_values =
				env.build_shader_globals(true);
		ok &= expect(rgb_near(object_values.fog_color, underwater.color) &&
					near(object_values.fog_end, underwater.end) &&
					object_values.fog_type == underwater.type,
				"ObjectModel values consume the selected scene fog");
		ok &= expect(rgb_near(terrain_values.fog_color, underwater.color) &&
					near(terrain_values.fog_end, underwater.end) &&
					terrain_values.fog_type == underwater.type,
				"terrain uniforms consume the selected scene fog");
		ok &= expect(rgb_near(shader_values.fog_color, underwater.color) &&
					near(shader_values.fog_end, underwater.end) &&
					shader_values.fog_type == underwater.type,
				"shader globals consume the selected scene fog");
		ok &= expect(opennova::env::underwater_murk_overlay_alpha_byte(-0.5f) == 80 &&
					opennova::env::underwater_murk_overlay_alpha_byte(0.0f) == 128 &&
					opennova::env::underwater_murk_overlay_alpha_byte(0.6f) == 185 &&
					opennova::env::underwater_murk_overlay_alpha_byte(0.8f) == 204 &&
					opennova::env::underwater_murk_overlay_alpha_byte(0.99f) == 223,
				"the underwater scissor uses raw 128 + trunc(96 * murk) alpha");
	}

	// --- WeatherRuntime reset epoch + prewarm + snapshot units --------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		env.configure_mission_clock(0x0C00, 1440);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		ok &= expect(env.is_weather_driven(),
				"the reset epoch claims the currents");
		ok &= expect(near(weather.core().scalar_channels.fog_dist_fp / 65536.0f,
							  640.0f),
				"mission init snaps scalar currents to the authored targets");
		// Snap-to-targets seeded the blocks [orig: @ 0x57d1e0].
		ok &= expect(rgb_near(weather.smooth_ceiling(),
							  env.ceiling_color_target(), 1.0f / 255.0f),
				"the color snap lands on the scaled targets");
		const int start_fixed24 = env.mission_time_fixed24();
		weather.prewarm_mission_start(&env);
		ok &= expect(env.mission_time_fixed24() ==
						start_fixed24 +
								255 * env.mission_advance_per_tick(),
				"prewarm advances exactly 255 weather ticks "
				"[orig: sub_57F1E0 @ 0x57f878]");
		opennova::env::NetEnvSnapshot snapshot;
		ok &= expect(weather.network_snapshot(&env, snapshot),
				"a loaded env backs the snapshot");
		ok &= expect(snapshot.fog_target_q16 == 640 << 16,
				"fog target packs q16");
		ok &= expect(snapshot.cloud_scroll_rate_target ==
						static_cast<int>(std::lround(cfg.sky_speed * 1024.0f)),
				"cloud scroll rate packs sky_speed << 10");
		ok &= expect(snapshot.tod_fixed24 == env.mission_time_fixed24(),
				"the snapshot carries the exact integer clock");
	}

	// --- shader-global publication policy -----------------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(2200.0f); // night: the moon owns the light
		ok &= expect(env.is_night_phase(), "22:00 reads as night");
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		const opennova::env::WeatherShaderGlobals globals =
				opennova::env::build_weather_shader_globals(env, weather);
		const opennova::env::Vec3 light = env.light_direction();
		ok &= expect(near(globals.base.sun_direction.x, light.x) &&
						near(globals.base.sun_direction.y, light.y) &&
						near(globals.base.sun_direction.z, light.z),
				"the published direction follows the ACTIVE light "
				"[orig: Environment_GetLightDirectionFloat @ 0x57d870]");
		const opennova::env::Vec3 sun = env.sun_direction();
		ok &= expect(!(near(light.x, sun.x) && near(light.y, sun.y) &&
							 near(light.z, sun.z)),
				"night publication is not pinned to the solar vector");
		ok &= expect(globals.base.wind_sway_amount >= 0.25f,
				"the sway floor holds");
		const opennova::env::WeatherShaderGlobals underwater_globals =
				opennova::env::build_weather_shader_globals(env, weather, true);
		const opennova::env::SceneFogValues underwater =
				env.build_scene_fog(true);
		ok &= expect(rgb_near(underwater_globals.base.fog_color,
					underwater.color) &&
					near(underwater_globals.base.fog_start, underwater.start) &&
					near(underwater_globals.base.fog_end, underwater.end) &&
					underwater_globals.base.fog_type == underwater.type,
				"Weather's direct global write preserves the selected underwater pass fog");
	}

	// --- the 62 Hz autonomous accumulator clamp ------------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_autonomous(&env);
		ok &= expect(!weather.world_tick_driven(),
				"autonomous mode leaves the render accumulator armed");
		// A huge stall clamps at 31 catch-up ticks and drops the credit.
		weather.process_delta(&env, 10.0f);
		weather.set_world_tick_driven(true);
		ok &= expect(weather.world_tick_driven(), "world-driven flips on");
	}

	if (!ok) {
		return 1;
	}
	std::printf("environment_state_test: all pins green\n");
	return 0;
}
