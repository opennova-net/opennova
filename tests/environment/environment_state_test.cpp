// EnvironmentState + WeatherRuntime (engine/runtime/environment): the state
// owner and the weather embedding ported from the environment.gd /
// weather.gd shell scripts (2026-08-09 de-scripting). Pins the mission
// clock views, the weather-driven targets/currents split, the NVG rewrite,
// generation discipline, the reset/prewarm epoch, the network wire units, and
// the shader-global publication policy. RE record: docs/env/env-tod-re.md.
#include <runtime/environment/environment_state.h>
#include <runtime/environment/sky_frame.h>
#include <runtime/environment/water_frame.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/world/weather_state.h>
#include <runtime/renderer/device_fog.h>
#include <formats/env/env_celestial.h>
#include <formats/env/env_weather.h>

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

	// --- the mission clock lives in the weather home ---------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		opennova::bms::Header header{};
		header.start_time = 0x0C00;
		header.minutes_per_day = 1440;
		opennova::world::WeatherState weather;
		weather.seed(opennova::env::weather_seed_from_config(cfg, header));
		env.bind_weather(&weather);
		ok &= expect(weather.tod_fixed24 == (12u << 24),
				"the Q8.8 noon start widens into the 8.24 accumulator");
		const uint32_t per_tick = weather.tod_advance_per_tick;
		ok &= expect(per_tick == 0x18000000u / (3720u * 1440u),
				"the witnessed per-tick increment");
		opennova::world::WeatherTickEvents events;
		for (int i = 0; i < 5; ++i) weather.tick_sim(nullptr, events);
		ok &= expect(weather.tod_fixed24 == (12u << 24) + 5u * per_tick,
				"five ticks advance exactly five increments");
		env.sync_clock_from_weather();
		ok &= expect(near(env.time_of_day(), EnvironmentState::fixed24_to_hhmm(
										  static_cast<int>(weather.tod_fixed24)), 0.01f),
				"the env clock follows the weather clock");
		weather.command_time_of_day_minutes(750);
		env.sync_clock_from_weather();
		ok &= expect(near(EnvironmentState::hhmm_to_minute_of_day(env.time_of_day()), 750.0f, 0.01f),
				"the TOD command moves the minute view");
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
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		ok &= expect(env.is_weather_driven(),
				"the reset epoch claims the currents");
		ok &= expect(weather.standalone() && env.weather() == &weather.state(),
				"a runtime without a simulation binds its private weather home");
		ok &= expect(near(weather.core().scalar_channels.fog_dist_fp / 65536.0f,
							  640.0f),
				"the seed snaps scalar currents to the authored targets");
		// Snap-to-targets seeded the blocks [orig: @ 0x57d1e0].
		ok &= expect(rgb_near(weather.smooth_ceiling(),
							  env.ceiling_color_target(), 1.0f / 255.0f),
				"the color snap lands on the scaled targets");
		const uint32_t start_fixed24 = weather.state().tod_fixed24;
		weather.prewarm_mission_start(&env);
		ok &= expect(weather.state().tod_fixed24 ==
						start_fixed24 + 255u * weather.state().tod_advance_per_tick,
				"prewarm advances exactly 255 weather ticks "
				"[orig: Environment_MissionStartInit @ 0x57f878]");
		ok &= expect(weather.state().fog_target_q16() == (640 << 16),
				"fog target packs q16");
		ok &= expect(weather.state().cloud_scroll_rate_target ==
						static_cast<uint32_t>(std::lround(cfg.sky_speed * 1024.0f)),
				"cloud scroll rate packs sky_speed << 10");
		ok &= expect(near(env.time_of_day(), EnvironmentState::fixed24_to_hhmm(
										  static_cast<int>(weather.state().tod_fixed24)), 0.01f),
				"the env clock reads the exact integer clock");
	}

	// --- settle_exposure: the frozen-fixture chase fixed point --------------
	// The capture-refresh seam (D-RLIT-2 fixture starvation): a paused
	// runtime never ticks, so the modulator holds its mission-reset identity;
	// the settle must land it on the iris target through the witnessed chase
	// while advancing neither the mission clock nor any other weather leg.
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		ok &= expect(near(weather.color_src_gain().r, 1.0f) &&
						near(weather.color_src_gain().g, 1.0f) &&
						near(weather.color_src_gain().b, 1.0f),
				"a frozen fixture starts at the mission-reset identity gain");
		const int32_t samples[3] = {8, 8, 8};
		weather.set_iris_samples(samples, 3);
		const opennova::env::CloudScrollState scroll_before =
				weather.core().cloud_scroll;
		const uint32_t clock_before = weather.state().tod_fixed24;
		weather.settle_exposure(&env);
		// Expected: the outdoor iris gain over the byte-quantized snapped
		// blocks [orig: terrain_sector_compute_lighting @ 0x5c7550;
		// target chase @ 0x57e512..0x57e538].
		const auto quant = [](const opennova::env::Rgb &c) {
			const auto q = [](float v) {
				const int b = static_cast<int>(v * 255.0f + 0.5f);
				return static_cast<float>(b < 0 ? 0 : (b > 255 ? 255 : b)) /
						255.0f;
			};
			return opennova::env::Rgb{q(c.r), q(c.g), q(c.b)};
		};
		const opennova::env::Vec3 sun_dir = env.sun_direction();
		const int expected_gain = opennova::env::iris_gain(
				quant(env.sun_light_target()), quant(env.sky_ambient_target()),
				quant(env.fill_light_target()), sun_dir.x, sun_dir.y,
				sun_dir.z, cfg.iris_center, cfg.iris_percent);
		ok &= expect(expected_gain != 64,
				"the fixture's iris target is not the identity gain");
		const float settled = weather.color_src_gain().r * 64.0f;
		ok &= expect(std::abs(settled - static_cast<float>(expected_gain)) <=
						1.0f,
				"settle_exposure lands the modulator on the iris target "
				"(the 12.20 chase may truncate one LSB short)");
		ok &= expect(weather.state().tod_fixed24 == clock_before,
				"the settle may not advance the mission clock");
		ok &= expect(weather.core().cloud_scroll.rate == scroll_before.rate &&
						weather.core().cloud_scroll.acc_l1_u ==
								scroll_before.acc_l1_u &&
						weather.core().cloud_scroll.acc_l1_v ==
								scroll_before.acc_l1_v &&
						weather.core().cloud_scroll.acc_l2_u ==
								scroll_before.acc_l2_u &&
						weather.core().cloud_scroll.acc_l2_v ==
								scroll_before.acc_l2_v,
				"the settle may not advance the cloud-scroll accumulators");
	}

	// --- sun veil: the dot^32 white-quad alpha + modulator-2 stop-down ------
	// [orig: compute_sun_glare_and_fog_blend @ 0x5ad610; the veil submit +
	//  modulator-2 writer Environment_ApplySunVeilAndExposureStopdown
	//  @ 0x5ad8b0].
	{
		using opennova::env::sun_veil_from_dot;
		const opennova::env::SunVeil head_on =
				sun_veil_from_dot(0x10000, 256, 0, 0);
		ok &= expect(head_on.glare == 192 && head_on.stopdown == 40,
				"head-on full-brightness veil serves the witnessed 192/40 caps");
		const opennova::env::SunVeil settled_fixture =
				sun_veil_from_dot(0x10000, 176, 0, 0);
		ok &= expect(settled_fixture.glare == 132 && settled_fixture.stopdown == 27,
				"brightness 176 folds 192 * b/256 and 40 * b/256 exactly");
		const opennova::env::SunVeil half_overcast =
				sun_veil_from_dot(0x10000, 176, 0, 0x8000);
		ok &= expect(half_overcast.glare == 66,
				"the overcast fold halves the veil at 0.5");
		const opennova::env::SunVeil away = sun_veil_from_dot(-0x8000, 256, 0, 0);
		ok &= expect(away.glare == 0 && away.stopdown == 0,
				"looking away from the sun serves no veil");
		// The fold's immediate is 0x640000, not 0x640001: SunDim = 1 (every
		// residue 1 mod 256 while the spring sweeps) scales by 25599/25600
		// [orig: @ 0x5ad80f — `mov ecx, 0x640000`].
		const opennova::env::SunVeil one_lsb = sun_veil_from_dot(0x10000, 256, 1, 0);
		ok &= expect(one_lsb.glare == 191 && one_lsb.stopdown == 39,
				"SunDim 1 folds 192 * 25599 / 25600 = 191 and 40 * 25599 / 25600 = 39");
		// SunDim above 100% (author-reachable; the spring is kUnclamped)
		// turns the fold scale negative (here -13000): both lanes are one
		// signed /25600 that truncates toward zero like retail's idiv
		// [orig: @ 0x5ad82c — the reciprocal multiply + UNSIGNED >>31 fixup].
		const opennova::env::SunVeil over_dim =
				sun_veil_from_dot(0x10000, 256, 0x640001 + 3328000, 0);
		ok &= expect(over_dim.glare == -97 && over_dim.stopdown == -20,
				"a negative SunDim scale truncates toward zero "
				"(192 * -13000 / 25600 = -97, 40 * -13000 / 25600 = -20)");
		// The water-reflected secondary term sums per channel under the 192
		// clamps, and only a glare above 2 draws the quad
		// [orig: Environment_ApplySunVeilAndExposureStopdown
		//  @ 0x5ad8f3..0x5ad928].
		const opennova::env::SunVeil combined =
				opennova::env::sun_veil_combine(head_on, settled_fixture);
		ok &= expect(combined.glare == 192 && combined.stopdown == 67,
				"the veil sums clamp the glare at 192 and add the stop-downs "
				"(40 + 27)");
		ok &= expect(!opennova::env::sun_veil_draws(2) &&
						opennova::env::sun_veil_draws(3),
				"the fullscreen veil draws only above glare 2");

		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		// Stop-down 40 dims modulator-2 toward 0x10101 * (64 - 60) = bytes 4
		// [orig: @ 0x5ad96a..0x5ad989]. The block chase is exponential
		// (delta = dist >> 3 clamped by the 8-tick step rate), so landing
		// within a byte takes ~35 ticks; retail rewrites the same target
		// every frame while staring at the sun.
		for (int i = 0; i < 64; ++i) {
			weather.set_sun_veil_stopdown(40);
			weather.tick_fixed(&env);
		}
		const int stopped_byte = static_cast<int>(
				(weather.core().modulator_chain.modulator2.render_color >> 16) &
				0xFF);
		ok &= expect(stopped_byte >= 3 && stopped_byte <= 5,
				"the stop-down chase lands modulator-2 on the dimmed target");
		// Release restores identity over 124 ticks [orig: @ 0x5ad996..0x5ad9a7].
		weather.set_sun_veil_stopdown(0);
		for (int i = 0; i < 140; ++i) {
			weather.tick_fixed(&env);
		}
		const int released_byte = static_cast<int>(
				(weather.core().modulator_chain.modulator2.render_color >> 16) &
				0xFF);
		ok &= expect(released_byte >= 63 && released_byte <= 65,
				"the release chase returns modulator-2 to identity");
	}

	// --- water glint: window/chase, reflected point, submit alpha -----------
	// [orig: update_sun_glare @ 0x5ad130; Water_ComputeReflectedSunPoint].
	{
		opennova::env::WaterGlintState glint;
		for (int i = 0; i < 20; ++i) {
			opennova::env::water_glint_tick(glint, true);
		}
		ok &= expect(glint.window == 0xF && glint.brightness == 256,
				"clear frames fill the 4-bit window and chase to 4 * 64");
		opennova::env::water_glint_tick(glint, false);
		ok &= expect(glint.window == 0x7 && glint.brightness == 240,
				"an occluded frame shifts the window and steps down 16 "
				"(no dead-band)");

		opennova::env::Vec3 point;
		const opennova::env::Vec3 low_cam{0.0f, 0.0f, 4.0f};
		const opennova::env::Vec3 sun{0.9316f, 0.342f, 0.1227f};
		ok &= expect(!opennova::env::water_glint_point(low_cam, sun, 10.0f,
							 0.0f, point),
				"a camera below the reflected-height clip serves no glint");
		const opennova::env::Vec3 cam{0.0f, 0.0f, 30.0f};
		ok &= expect(opennova::env::water_glint_point(cam, sun, 10.0f, 0.0f,
							 point),
				"an above-water camera serves the reflected point");
		// clip = 2*10 - 30 = -10; denom = 30 + 251.29 + 10; ratio = 20 /
		// denom; x = 0.9316*2048*ratio; z = clip + denom*ratio = the water
		// height itself (the last fmulp multiplies ratio by the FULL delta
		// [orig: Water_ComputeReflectedSunPoint @ 0x5ac0d0..0x5ac0e5]).
		ok &= expect(near(point.x, 130.99f, 0.05f) &&
						near(point.z, 10.0f, 0.01f),
				"the reflected point lands on the water plane");

		ok &= expect(opennova::env::water_glint_alpha_fixed(0x10000, 256, 0) ==
						9216,
				"head-on full-brightness glint alpha is the witnessed 9216 "
				"(dot^4 - 28672/65536 fold, >> 2)");
		ok &= expect(opennova::env::water_glint_alpha_fixed(0x10000, 0, 0) == 0 &&
						opennova::env::water_glint_alpha_fixed(-0x8000, 256, 0) ==
								0,
				"zero brightness or looking away serves no glint");
		// dot^4 below 28672/65536 clamps at zero (the negative-factor region).
		ok &= expect(opennova::env::water_glint_alpha_fixed(0xC000, 256, 0) == 0,
				"the sub-threshold dot region clamps to zero");
	}

	// --- shader-global publication policy -----------------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.configure_mission_clock(22 << 8, 1440); // night: the moon owns the light
		ok &= expect(env.is_night_phase(), "22:00 reads as night");
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		ok &= expect(env.is_night_phase(), "the standalone weather home keeps the configured clock");
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

	// --- the terrain colour reciprocal the loaded config parses to ----------
	// [orig: TimeOfDay_ParseProperty @ 0x57ca60..0x57cae3; the boot default
	//  Environment_InitDefaults @ 0x57c065]
	{
		EnvironmentState env;
		ok &= expect(env.terrain_color_recip_packed() == 0x808080u,
				"no config serves the 0x808080 boot default");
		opennova::env::Config cfg = make_config();
		cfg.terrain_rgb = {1.0f, 1.0f, 1.0f};
		env.set_config(&cfg, true);
		ok &= expect(env.terrain_color_recip_packed() == 0x808080u,
				"white terrain_rgb divides to the same 0x808080");
		cfg.terrain_rgb = {200.0f / 255.0f, 128.0f / 255.0f, 0.0f};
		ok &= expect(env.terrain_color_recip_packed() == 0xA3FF80u,
				"200 -> 163, 128 -> 255 (clamp), 0 -> 0x80, packed r<<16|g<<8|b");
	}

	// --- the light values carry the one device fog range ---------------------
	{
		// Under overcast the device end is Environment_GetFogEndDistance's
		// L * (1 - o/2) and Render_SetFogState's type-2 start is
		// (1 - o) * 0.5 * that end; the corona fold and the Q3 copies read
		// both from the light values, which must publish the same pair the
		// scene fog (shader globals) carries, never the unscaled level.
		EnvironmentState env;
		opennova::env::Config cfg = make_config();
		cfg.fog_type = 2;
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		weather.state().core.scalar_channels.fog_dist_fp = 640 << 16;
		weather.state().core.scalar_channels.overcast_fp = 0x8000;
		ok &= expect(near(env.overcast_blend(), 0.5f), "overcast 0.5 is live");
		const float device_end = env.fog_end_distance();
		ok &= expect(near(device_end, 640.0f * 0.75f),
				"the device fog end is the level scaled by (1 - overcast/2)");
		opennova::env::WorldLightValues values;
		ok &= expect(env.build_light_values({}, values, false),
				"a loaded dry pass builds light values");
		const opennova::env::SceneFogValues scene = env.build_scene_fog(false);
		ok &= expect(near(values.fog_end, device_end) &&
						near(values.fog_end, scene.end),
				"the light values' fog end is the overcast-scaled device end");
		ok &= expect(near(values.fog_start,
						opennova::env::compute_fog_params(2, device_end, 0.5f)
								.start) &&
						near(values.fog_start, 0.5f * 0.5f * device_end) &&
						near(values.fog_start, scene.start),
				"the light values' fog start is Render_SetFogState's type-2 "
				"start from that same end");
		// The device evaluation over that pair: unfogged at the start,
		// fully fogged at the end.
		ok &= expect(near(opennova::renderer::device_fog_visibility(
								values.fog_start, values.fog_start,
								values.fog_end, values.fog_type, true), 1.0f) &&
						near(opennova::renderer::device_fog_visibility(
								values.fog_end, values.fog_start,
								values.fog_end, values.fog_type, true), 0.0f),
				"the corona fold's linear range runs start..end of the device pair");
		// The sky dome is the one consumer that does NOT take the scaled end:
		// c9.x is the raw smoothed distance [orig: render_skybox @ 0x5792c2].
		const opennova::env::SkyFrameState sky =
				opennova::env::build_sky_frame(env);
		ok &= expect(sky.loaded && near(sky.fog_end, env.fog_level()) &&
						near(sky.fog_end, 640.0f) &&
						!near(sky.fog_end, device_end),
				"the dome fog end is the unscaled smoothed fog distance, not the "
				"overcast-scaled device end");
	}

	// --- the 62.5 Hz autonomous accumulator clamp ----------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_autonomous(&env);
		ok &= expect(!weather.world_tick_driven(),
				"autonomous mode leaves the render accumulator armed");
		// A huge stall clamps at 31 catch-up ticks and drops the credit; the
		// private state's clock moves with them.
		const uint32_t before = weather.state().tod_fixed24;
		weather.process_delta(&env, 10.0f);
		ok &= expect(weather.state().tod_fixed24 ==
						before + 31u * weather.state().tod_advance_per_tick,
				"the autonomous accumulator runs 31 full ticks on a stall");
		weather.set_world_tick_driven(true);
		ok &= expect(weather.world_tick_driven(), "world-driven flips on");
	}

	// --- the standalone home follows the owner's TOD -------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(2200.0f);
		ok &= expect(env.standalone_weather().tod_fixed24 == (22u << 24),
				"an owner TOD write moves the standalone clock");
		env.ensure_standalone_weather_seeded(256);
		ok &= expect(env.weather_live(), "the first seed comes from the loaded config");
		ok &= expect(env.standalone_weather().tod_fixed24 == (22u << 24) &&
						env.standalone_weather().tod_advance_per_tick == 0,
				"without a configured mission clock a preview holds its TOD");
		env.set_render_time_of_day(1200.0f);
		ok &= expect(env.standalone_weather().tod_fixed24 == (22u << 24),
				"the tick writeback never touches the clock");
		env.ensure_standalone_weather_seeded(256);
		ok &= expect(env.standalone_weather().tod_fixed24 == (22u << 24),
				"a seeded home is never re-seeded by the attach path");
		env.configure_mission_clock(9 << 8, 1440);
		env.reset_standalone_weather(256);
		ok &= expect(env.standalone_weather().tod_fixed24 == (9u << 24) &&
						env.standalone_weather().tod_advance_per_tick != 0,
				"a configured mission clock runs from the header");
		ok &= expect(env.debug_set_mission_minute_of_day(540.0) &&
						env.standalone_weather().tod_fixed24 == (9u << 24),
				"the dev-tool scrub lands exactly on the 8.24 clock");
	}

	if (!ok) {
		return 1;
	}
	std::printf("environment_state_test: all pins green\n");
	return 0;
}
