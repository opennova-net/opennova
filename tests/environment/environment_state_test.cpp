// EnvironmentState + WeatherRuntime (engine/runtime/environment): the state
// owner and the weather embedding ported from the environment.gd /
// weather.gd shell scripts (2026-08-09 de-scripting). Pins the mission
// clock views, the weather-driven targets/currents split, the NVG rewrite
// (sky/ground per-channel, ceiling/floor R-term), the thermal view (grey
// world block, 0x808080 fog/clear, flat terrain ramps), generation
// discipline, the reset/prewarm epoch, the network wire units, and the
// shader-global publication policy. RE record: docs/env/env-tod-re.md.
#include <base/vfs/file_source.h>
#include <runtime/environment/celestial_frame.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/sky_frame.h>
#include <runtime/environment/water_frame.h>
#include <runtime/environment/water_mirror.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/world/weather_state.h>
#include <runtime/world/world.h>
#include <runtime/renderer/device_fog.h>
#include <formats/env/env.h>
#include <formats/env/env_celestial.h>
#include <formats/env/env_weather.h>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

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

// The 8.24 mission clock read as HHMM, computed independently of the code
// under test (whole hours * 100 + the minute remainder).
float fixed24_to_hhmm(int value) {
	const double hours = static_cast<double>(value) / static_cast<double>(1 << 24);
	const double hour = std::floor(hours);
	return static_cast<float>(hour * 100.0 + (hours - hour) * 60.0);
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
	ok &= expect(near(EnvironmentState::hhmm_to_minute_of_day(1230.0f), 750.0f),
			"12:30 is minute 750");
	// The curtime word: the truncating 16.16 parse widened by << 8, so a minute
	// that is not a quarter hour lands below the exact 8.24 hour (12:10 is not
	// llround(12.1667 * 2^24) = 204122795), and the minutes clamp to 59.
	// [orig: TimeOfDay_ParseProperty @0x57d0b6..0x57d0d2;
	//  Environment_ParseTimeString @0x57c500, clamps @0x57c552/@0x57c55c,
	//  divide @0x57c566..0x57c57c]
	ok &= expect(EnvironmentState::hhmm_to_fixed24(1210.0) == ((12u << 16) + (10u << 16) / 60u) << 8 &&
					EnvironmentState::hhmm_to_fixed24(1210.0) == 204122624u,
			"12:10 truncates its minute before the shift");
	ok &= expect(EnvironmentState::hhmm_to_fixed24(1215.0) == (12u << 24) + (1u << 22),
			"a quarter hour is exact");
	ok &= expect(EnvironmentState::hhmm_to_fixed24(1275.0) == ((12u << 16) + (59u << 16) / 60u) << 8,
			"the minutes clamp to 59, not into the next hour");
	ok &= expect(EnvironmentState::hhmm_to_fixed24(2359.0) == 402373376u &&
					EnvironmentState::hhmm_to_fixed24(2359.0) < 0x18000000u,
			"the last minute stays under one day");
	ok &= expect(EnvironmentState::hhmm_to_fixed24(0.0) == 0u, "midnight is zero");

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
		ok &= expect(near(env.time_of_day(), fixed24_to_hhmm(
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
		// f = (2+1)*.2 = .6: c' = c*.15 + gain*.06, in the object block only.
		opennova::env::WorldLightValues values;
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values) &&
						near(values.hemi_ground.r, 0.8f * 0.15f + 0.5f * 0.06f),
				"the NVG hemisphere rewrite (modulator*f/640 as gain*f/10)");
		ok &= expect(rgb_near(env.fill_light(), {0.8f, 0.4f, 0.2f}),
				"the ground getter stays the raw block under NVG");
		ok &= expect(!env.set_nvg_view(true, 2), "identical NVG is idempotent");
	}

	// --- the NVG interior pair: the modulator R term on all channels ---------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		env.set_static_colors_rt({0.8f, 0.4f, 0.2f}, {0.5f, 0.5f, 0.5f},
				{0.2f, 0.4f, 0.8f});
		env.set_color_src_gain({0.5f, 0.25f, 0.75f});
		// The served sky is the envscale-quantized keyframe (0.4 -> 51/255 at
		// envscale .5): read it raw before NVG instead of assuming the authored
		// value.
		const opennova::env::Rgb sky_raw = env.sky_ambient();
		const opennova::env::SceneFogValues underwater_raw = env.build_scene_fog(true);
		const opennova::env::TerrainEnvUniforms terrain_raw = env.build_terrain_uniforms(false);
		ok &= expect(env.set_nvg_view(true, 2), "NVG change reported");
		opennova::env::WorldLightValues values;
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values),
				"the block builds under NVG");
		// f = .6: ceiling'/floor' = c*.15 + gain.r*.06 on EVERY channel (the
		// R term reused [orig: @ 0x5c82a5..0x5c82e9]) ...
		ok &= expect(rgb_near(values.ceiling,
					{0.8f * 0.15f + 0.03f, 0.4f * 0.15f + 0.03f,
							0.2f * 0.15f + 0.03f}),
				"the NVG ceiling rewrite adds the modulator R term to all channels");
		ok &= expect(rgb_near(values.floor_color,
					{0.2f * 0.15f + 0.03f, 0.4f * 0.15f + 0.03f,
							0.8f * 0.15f + 0.03f}),
				"the NVG floor rewrite adds the modulator R term to all channels");
		// ... while sky/ground keep their per-channel terms (gain.g on g, not
		// the R term) [orig: @ 0x5c8258..0x5c82a1].
		ok &= expect(near(values.hemi_sky.g, sky_raw.g * 0.15f + 0.25f * 0.06f) &&
						!near(values.hemi_sky.g, sky_raw.g * 0.15f + 0.5f * 0.06f),
				"sky/ground keep the per-channel modulator terms");
		// The rewrite is the object block's alone: the colour getters, lit
		// water / the underwater fog and the combined terrain light read the raw
		// blocks [orig: g_EnvWaterColorLit @ 0x57f177 and g_EnvTerrainLightCombined
		// @ 0x57f0d5 are built from the raw blocks in the weather tick].
		ok &= expect(rgb_near(env.sky_ambient(), sky_raw) &&
						rgb_near(env.ceiling_color(), {0.8f, 0.4f, 0.2f}) &&
						rgb_near(env.floor_color(), {0.2f, 0.4f, 0.8f}),
				"the colour getters stay raw under NVG");
		ok &= expect(rgb_near(env.build_scene_fog(true).color, underwater_raw.color),
				"the lit water colour ignores the NVG rewrite");
		// The terrain rebuilds its NVG sky as BYTES with a chop:
		// trunc(sky_byte * 0.15 + mod_byte * 0.6 * 0.0015625 * 255)
		// [orig: Render_TerrainScene @ 0x610d16..0x610e22]. The modulator bytes
		// are gain * 64 = (32, 16, 48).
		const auto terrain_nvg_byte = [](float sky, int mod_byte) {
			const int sky_byte = static_cast<int>(std::lround(sky * 255.0f));
			const float sum = static_cast<float>(sky_byte) * (0.25f * 0.6f) +
					static_cast<float>(mod_byte) * (0.6f * 0.0015625f * 255.0f);
			return static_cast<float>(static_cast<int>(sum)) / 255.0f;
		};
		const opennova::env::TerrainEnvUniforms terrain_nvg = env.build_terrain_uniforms(false);
		ok &= expect(rgb_near(terrain_nvg.sky_ambient,
					{terrain_nvg_byte(sky_raw.r, 32), terrain_nvg_byte(sky_raw.g, 16),
							terrain_nvg_byte(sky_raw.b, 48)}, 1.0e-6f) &&
						rgb_near(terrain_nvg.sun_light, terrain_raw.sun_light),
				"the terrain NVG sky is the truncated byte rebuild over the raw light");
		ok &= expect(!near(terrain_nvg.sky_ambient.g, values.hemi_sky.g, 1.0e-6f),
				"the terrain byte rebuild differs from the object block's float rewrite");
		env.set_nvg_view(false, 2);
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values) &&
						rgb_near(values.ceiling, {0.8f, 0.4f, 0.2f}) &&
						rgb_near(values.floor_color, {0.2f, 0.4f, 0.8f}),
				"NVG off serves the raw interior pair");
	}

	// --- the thermal view --------------------------------------------------
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		env.set_color_src_gain({0.5f, 0.25f, 0.75f});
		const opennova::env::SceneFogValues underwater_before =
				env.build_scene_fog(true);
		const int64_t gen = env.env_generation();
		ok &= expect(!env.set_thermal_view(false, false),
				"the idle thermal view is idempotent");
		ok &= expect(env.set_thermal_view(true, true) &&
						env.env_generation() == gen + 1,
				"a thermal change reports and bumps once");
		const opennova::env::Rgb grey{0.5f, 0.5f, 0.5f};
		opennova::env::WorldLightValues values;
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values) &&
						rgb_near(values.hemi_sky, grey) &&
						rgb_near(values.hemi_ground, grey) &&
						rgb_near(values.ceiling, grey) &&
						rgb_near(values.floor_color, grey),
				"the thermal world block is flat 0.5 on every hemisphere");
		ok &= expect(rgb_near(values.dir_color, {0.0f, 0.0f, 0.0f}),
				"the thermal block disables the directional light");
		ok &= expect(rgb_near(values.gain, {0.5f, 0.25f, 0.75f}),
				"the modulator gain is not part of the thermal block");
		// The device fog / clear: unk_808080, in the render space the Clear
		// consumes verbatim.
		const opennova::env::Rgb grey_fog{128.0f / 255.0f, 128.0f / 255.0f,
				128.0f / 255.0f};
		const opennova::env::SceneFogValues dry = env.build_scene_fog(false);
		ok &= expect(rgb_near(dry.color, grey_fog) &&
						near(dry.end, env.fog_end_distance()) &&
						dry.type == env.fog_type(),
				"the thermal dry pass fogs to 0x808080 over the weather range");
		ok &= expect(rgb_near(env.build_scene_fog(true).color,
					underwater_before.color),
				"underwater lit water outranks the thermal fog");
		// The weapon Inset pass applies no alternate fog: its dry pass fogs
		// toward the fog block under the thermal view, its underwater pass
		// toward the lit water [orig: Render_WeaponInsetScene
		// @ 0x5c9d41..0x5c9d4f].
		const opennova::env::SceneFogValues inset_dry = env.build_inset_scene_fog(false);
		ok &= expect(rgb_near(inset_dry.color, env.fog_color()) &&
						!rgb_near(inset_dry.color, grey_fog) &&
						near(inset_dry.end, env.fog_end_distance()) &&
						inset_dry.type == env.fog_type(),
				"the Inset pass never fogs toward the thermal grey");
		ok &= expect(rgb_near(env.build_inset_scene_fog(true).color,
						underwater_before.color),
				"the Inset pass below the water fogs toward the lit water");
		ok &= expect(rgb_near(env.frame_clear_color_for(true), grey_fog) &&
						rgb_near(env.frame_clear_color_for(false), grey_fog),
				"the thermal clear outranks the water test");
		// The water mirror's clear takes no thermal, waterline or NVG leg: the
		// skyfog on its outdoors path, black under the indoors letter
		// [orig: Render_MainScene @ 0x5c1342..0x5c1353, @ 0x5c1474, @ 0x5c1597].
		ok &= expect(rgb_near(env.water_mirror_clear_color(true), env.frame_clear_color()) &&
						!rgb_near(env.water_mirror_clear_color(true), grey_fog),
				"the thermal grey never reaches the mirror clear");
		ok &= expect(rgb_near(env.water_mirror_clear_color(false), {0.0f, 0.0f, 0.0f}),
				"the indoors letter clears the mirror to black");
		// The terrain ramps: c1 light 0x101010, c0 sky 0xF0F0F0.
		const opennova::env::Rgb ramp_light{16.0f / 255.0f, 16.0f / 255.0f,
				16.0f / 255.0f};
		const opennova::env::Rgb ramp_sky{240.0f / 255.0f, 240.0f / 255.0f,
				240.0f / 255.0f};
		ok &= expect(rgb_near(env.build_terrain_uniforms(false).sun_light,
							 ramp_light) &&
						rgb_near(env.build_terrain_uniforms(false).sky_ambient,
								ramp_sky),
				"the thermal terrain ramps are the flat 0x101010 / 0xF0F0F0 pair");
		ok &= expect(rgb_near(env.build_shader_globals(false).sun_light,
							 ramp_light) &&
						rgb_near(env.build_shader_globals(false).sky_ambient,
								ramp_sky),
				"the sun/sky globals publish the terrain pair");
		// The environment cube's specular sphere stays lit by the RAW light
		// block under the thermal ramps [orig: the cube face callback
		// EnvCube_RenderFaceCallback pushes g_EnvLightBlock @ 0x5c3863].
		ok &= expect(rgb_near(env.build_shader_globals(false).light_block,
							 env.sun_light()) &&
						!rgb_near(env.build_shader_globals(false).light_block,
								ramp_light),
				"the cube light block stays raw under the thermal terrain ramps");
		// NVG precedence differs per side: NVG in first person takes the
		// terrain's NVG blend, while the world block's grey outranks NVG.
		env.set_nvg_view(true, 0);
		ok &= expect(!rgb_near(env.build_terrain_uniforms(false).sky_ambient,
							 ramp_sky) &&
						rgb_near(env.build_terrain_uniforms(false).sun_light,
								env.sun_light()),
				"NVG in first person outranks the thermal terrain ramps");
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values) &&
						rgb_near(values.hemi_sky, grey) &&
						rgb_near(values.ceiling, grey),
				"the world block's thermal grey outranks the NVG rewrite");
		env.set_nvg_view(false, 0);
		// A Thermal def held un-scoped in first person: only the terrain gate.
		ok &= expect(env.set_thermal_view(false, true),
				"the two gates change independently");
		ok &= expect(env.build_light_values({0.0f, 1.0f, 0.0f}, values) &&
						rgb_near(values.hemi_sky, env.sky_ambient()) &&
						rgb_near(values.dir_color, env.sun_light()),
				"the world block needs the CanFire gate");
		ok &= expect(rgb_near(env.build_scene_fog(false).color, env.fog_color()) &&
						rgb_near(env.frame_clear_color_for(true),
								env.frame_clear_color()),
				"the fog and clear need the CanFire gate");
		ok &= expect(rgb_near(env.build_terrain_uniforms(false).sun_light,
					ramp_light),
				"the terrain ramps key on the def bit alone");
		env.set_thermal_view(false, false);
		ok &= expect(rgb_near(env.build_terrain_uniforms(false).sun_light,
							 env.sun_light()) &&
						rgb_near(env.build_terrain_uniforms(false).sky_ambient,
								env.sky_ambient()),
				"thermal off restores the light/sky ramps");
	}

	// --- the viewmodel's pass fog -------------------------------------------
	// The frame fogs the viewmodel under the DRY pass (ApplyFogAndAmbient(0,
	// thermal) @ 0x5ca3bf..0x5ca3ce precedes the viewmodel @ 0x5ca829; the
	// eye's pass is re-applied only after it, @ 0x5ca82e..0x5ca841), and the
	// dome wrapper restores g_EnvFogBlock once it drew (@ 0x579ce6..0x579cf6).
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		const opennova::env::SceneFogValues dry = env.build_scene_fog(false);
		const opennova::env::SceneFogValues gun = env.build_viewmodel_fog(true);
		ok &= expect(rgb_near(gun.color, dry.color) && near(gun.start, dry.start) &&
						near(gun.end, dry.end) && gun.type == dry.type,
				"the viewmodel fogs under the dry pass payload");
		ok &= expect(!rgb_near(gun.color, env.build_scene_fog(true).color) &&
						!near(gun.end, env.build_scene_fog(true).end),
				"the viewmodel never takes the underwater pass");
		ok &= expect(rgb_near(env.build_viewmodel_fog(false).color, dry.color),
				"with no dome the dry pass colour stays");
		env.set_thermal_view(true, true);
		const opennova::env::Rgb grey_fog{128.0f / 255.0f, 128.0f / 255.0f,
				128.0f / 255.0f};
		ok &= expect(rgb_near(env.build_viewmodel_fog(false).color, grey_fog),
				"thermal without the dome keeps the pass grey");
		ok &= expect(rgb_near(env.build_viewmodel_fog(true).color, env.fog_color()) &&
						near(env.build_viewmodel_fog(true).end, dry.end),
				"thermal after the dome fogs toward the restored fog block");
		env.set_thermal_view(false, false);
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
					env.frame_clear_color_for(false)),
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
		// The water mirror pass fogs with the dry weather block on either side
		// of the plane and never takes the thermal grey [orig: Render_MainScene
		// @ 0x5c1648..0x5c164c / Water_RenderReflectedWorldScene @ 0x5c8515..
		// 0x5c8519 -> Environment_ApplyFogAndAmbient(0, 0); SkyDome_RenderWithSkyfog
		// @ 0x579ce7..0x579cf6 restores g_EnvFogBlock after the sky].
		const opennova::env::SceneFogValues mirror = env.build_water_mirror_fog();
		ok &= expect(rgb_near(mirror.color, dry.color) && near(mirror.end, dry.end) &&
					near(mirror.start, dry.start) && mirror.type == dry.type,
				"the mirror fog is the dry pass while the main eye is underwater");
		env.set_thermal_view(true, true);
		ok &= expect(rgb_near(env.build_water_mirror_fog().color, env.fog_color()) &&
					!rgb_near(env.build_scene_fog(false).color, env.fog_color()),
				"the thermal view greys the scene fog but not the mirror's");
		env.set_thermal_view(false, false);

		// The reflected-scene camera [orig: Render_MainScene @ 0x5c1361..
		// 0x5c1370]: mirrored (z' = 2wh - z, pitch/roll negated) at or above
		// the plane, the live block unchanged below it.
		opennova::env::MirrorSourceView source;
		source.basis_x = {1.0f, 0.0f, 0.0f};
		source.basis_y = {0.0f, 0.8f, -0.6f};
		source.basis_z = {0.0f, 0.6f, 0.8f};
		source.origin = {3.0f, 12.0f, -4.0f};
		source.aspect = 1024.0f / 600.0f;
		source.v_offset = 0.5f;
		const opennova::env::WaterMirrorView mirrored =
				opennova::env::build_water_mirror_view(source, 7.0f);
		ok &= expect(!mirrored.below_water && near(mirrored.origin.y, 2.0f) &&
					near(mirrored.basis_y.y, 0.8f) && near(mirrored.basis_y.z, 0.6f) &&
					near(mirrored.basis_z.y, -0.6f) && near(mirrored.v_offset, -0.5f),
				"above the plane the reflected camera is the proper mirror");
		source.origin.y = 2.0f;
		const opennova::env::WaterMirrorView live =
				opennova::env::build_water_mirror_view(source, 7.0f);
		ok &= expect(live.below_water && near(live.origin.x, 3.0f) &&
					near(live.origin.y, 2.0f) && near(live.origin.z, -4.0f) &&
					near(live.basis_y.y, 0.8f) && near(live.basis_y.z, -0.6f) &&
					near(live.basis_z.y, 0.6f) && near(live.v_offset, 0.5f),
				"below the plane the reflected camera is the live camera unchanged");

		// The reflected pass projects with the main view's field on both axes
		// over the square RTT [orig: Render_MainScene @ 0x5c1255,
		// @ 0x5c1464..0x5c1482, its Render_SetViewAndProjectionMatrices call
		// @ 0x5c1619..0x5c163e]: the mirror keeps the source frustum, its aspect
		// included, over the 512 x 512 target (non-square texels).
		source.origin.y = 12.0f;
		source.fov_deg = 60.0f;
		source.keep_aspect_height = true;
		const opennova::env::WaterMirrorView field =
				opennova::env::build_water_mirror_view(source, 7.0f);
		ok &= expect(field.projection == opennova::env::MirrorProjection::kPerspective &&
					near(field.fov_deg, 60.0f) && field.keep_aspect_height &&
					near(field.aspect, 1024.0f / 600.0f) &&
					opennova::env::kReflectionRttSize == 512,
				"the mirror takes the source field at its aspect over the 512 square");
		// The reflected pass rasterises on the RTT through its own viewport and
		// the frame's plain projection [orig: Render_MainScene @ 0x5c1614,
		// @ 0x5c163e]: D3D9's half texel of the 512 square, right and down,
		// not the main raster's half pixel.
		ok &= expect(field.raster_shift.x == 1.0f / 512.0f &&
							field.raster_shift.y == -1.0f / 512.0f &&
							live.raster_shift.x == field.raster_shift.x &&
							live.raster_shift.y == field.raster_shift.y,
				"the mirror image sits on the RTT's own D3D9 pixel centres, above or below the plane");
		source.aspect = 600.0f / 1024.0f;
		ok &= expect(near(opennova::env::build_water_mirror_view(source, 7.0f).aspect,
							 600.0f / 1024.0f),
				"a portrait source keeps its own aspect over the same square");

		// The mirror's per-draw CLIP arming [orig: Terrain_RenderSectorModels
		// @ 0x5c5e57..0x5c5e75, Terrain_RenderSectorEntities @ 0x5c7c1a..0x5c7c2e]:
		// a building below wh - 0.25, an entity whose z - radius sits below wh,
		// never a draw outside the sector walks, in 16.16.
		{
			using opennova::env::MirrorClipWave;
			using opennova::env::water_mirror_clip_armed;
			using opennova::env::water_mirror_clip_origin_offset;
			const int32_t wh = 10 << 16;
			const int32_t floor_q16 = -(1 << 16);
			ok &= expect(water_mirror_clip_armed(MirrorClipWave::kSectorModel,
								(10 << 16) + (3 << 14) - 1, floor_q16, wh) &&
						!water_mirror_clip_armed(MirrorClipWave::kSectorModel,
								(10 << 16) + (3 << 14), floor_q16, wh),
					"a building arms while its floor sits below wh - 0.25");
			const int32_t radius = 2 << 16;
			ok &= expect(water_mirror_clip_armed(MirrorClipWave::kEntity, (12 << 16) - 1,
								radius, wh) &&
						!water_mirror_clip_armed(MirrorClipWave::kEntity, 12 << 16, radius, wh),
					"an entity arms while z - boundRadius sits below wh");
			ok &= expect(!water_mirror_clip_armed(MirrorClipWave::kNone, -(100 << 16), 0, wh),
					"a draw outside the sector walks never arms");
			ok &= expect(near(water_mirror_clip_origin_offset(MirrorClipWave::kSectorModel,
								floor_q16), -0.75f) &&
						near(water_mirror_clip_origin_offset(MirrorClipWave::kEntity, radius),
								-2.0f),
					"the static rows' offset form is the same test");
		}

		// Render_WaterSurface's side gate [orig: @ 0x5c32f6 jge / @ 0x5c3304
		// jle]: strictly above draws the view-0 side (and the bloom pass's
		// nightvision redraw), strictly below the underwater side, and an eye
		// exactly on the plane draws neither.
		const opennova::env::WaterSurfaceSides above =
				opennova::env::water_surface_sides(7.5f, 7.0f);
		const opennova::env::WaterSurfaceSides below =
				opennova::env::water_surface_sides(6.5f, 7.0f);
		const opennova::env::WaterSurfaceSides level =
				opennova::env::water_surface_sides(7.0f, 7.0f);
		ok &= expect(above.above && !above.underwater && below.underwater &&
					!below.above && !level.above && !level.underwater,
				"the water surface draws strictly above or strictly below the plane");
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
		ok &= expect(near(env.time_of_day(), fixed24_to_hhmm(
										  static_cast<int>(weather.state().tod_fixed24)), 0.01f),
				"the env clock reads the exact integer clock");
	}

	// The mission seed and T0 publication precede the kernel's startup work;
	// attaching the device hook must not run the first weather tick early.
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		WeatherRuntime weather;
		weather.set_wind_strength_pct(50.0f);
		opennova::world::WeatherState mission;
		opennova::bms::Header header{};
		header.start_time = (9 << 8) | 128;
		header.minutes_per_day = 120;
		const uint32_t start = static_cast<uint32_t>(header.start_time) << 16;
		int phase = 0;
		weather.run_mission_start_boundary(&env, &mission, header, [&]() {
			ok &= expect(phase++ == 0 && mission.tod_fixed24 == start &&
					mission.fog_target_q16() == (640 << 16) &&
					mission.core.oscillator.intensity == 128,
					"the hook sees the mission clock, config and current wind already seeded");
			weather.attach_state(&mission, &env);
			weather.set_world_tick_driven(true);
		}, [&]() {
			ok &= expect(phase++ == 1 && !weather.standalone() &&
					&weather.state() == &mission && mission.tod_fixed24 == start,
					"startup work sees the attached home before any weather tick");
			ok &= expect(rgb_near(weather.smooth_ceiling(),
					env.ceiling_color_target(), 1.0f / 255.0f),
					"T0 color targets are published before startup WAC and settling");
		});
		ok &= expect(phase == 2, "each mission boundary callback runs once");
	}
	{
		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		WeatherRuntime weather;
		weather.prepare_autonomous(&env);
		const uint32_t start = weather.state().tod_fixed24;
		const uint32_t advance = weather.state().tod_advance_per_tick;
		int callbacks = 0;
		const auto unexpected = [&]() { ++callbacks; };
		weather.run_mission_start_boundary(&env, nullptr, {}, unexpected, unexpected);
		ok &= expect(callbacks == 0 && weather.state().tod_fixed24 == start + 255u * advance,
				"an absent mission retains the standalone 255-tick settle without callbacks");
		const uint32_t settled = weather.state().tod_fixed24;
		weather.run_mission_start_boundary(nullptr, &weather.state(), {}, unexpected, unexpected);
		ok &= expect(callbacks == 0 && weather.state().tod_fixed24 == settled,
				"an absent environment leaves weather and callbacks untouched");
	}
	{
		EnvironmentState env;
		WeatherRuntime weather;
		opennova::world::WeatherState mission;
		mission.tod_fixed24 = 3u << 24;
		mission.set_wind_scale(91);
		int completions = 0;
		weather.run_mission_start_boundary(&env, &mission, {}, [&]() {
			weather.attach_state(&mission, &env);
		}, [&]() { ++completions; });
		ok &= expect(completions == 1 && mission.tod_fixed24 == (3u << 24) &&
				mission.wind_scale() == 91,
				"a missing config still attaches and completes without reseeding the mission");
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
		// blocks [orig: Terrain_SectorComputeLighting @ 0x5c7550;
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

	// --- the iris re-target gate: a local player and WAC `autogain` ---------
	// The render leg re-targets the modulator only with a local player entity
	// and the `autogain` named value nonzero; the simulation samples both into
	// the weather home each tick. A closed gate leaves the modulator on its
	// last target (here the mission-reset identity).
	// [orig: Environment_ApplyFogAndAmbient @0x57E50B..0x57E51B, both `jz` to
	//  the exit @0x57E53D]
	{
		opennova::world::World world;
		world.registry.configure_pool(0, 4);
		opennova::world::WeatherTickEvents events;
		world.weather.tick_sim(&world, events);
		ok &= expect(!world.weather.iris_retarget_enabled,
				"no local player closes the iris gate");
		opennova::world::Entity body;
		body.net_id = 1;
		world.cached.local_player = world.registry.spawn(0, body);
		world.weather.tick_sim(&world, events);
		ok &= expect(world.weather.iris_retarget_enabled,
				"a local player with autogain 1 opens the iris gate");
		world.script.wac_values.autogain = 0;
		world.weather.tick_sim(&world, events);
		ok &= expect(!world.weather.iris_retarget_enabled,
				"set(autogain,0) closes the iris gate");
		world.weather.tick_sim(nullptr, events);
		ok &= expect(!world.weather.iris_retarget_enabled,
				"a tick without a World keeps the last sample");

		EnvironmentState env;
		const opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		weather.state().iris_retarget_enabled = false;
		const int32_t samples[3] = {8, 8, 8};
		weather.set_iris_samples(samples, 3);
		weather.settle_exposure(&env);
		ok &= expect(near(weather.color_src_gain().r, 1.0f) &&
						near(weather.color_src_gain().g, 1.0f) &&
						near(weather.color_src_gain().b, 1.0f),
				"a closed iris gate holds the modulator on its target");
		weather.state().iris_retarget_enabled = true;
		weather.settle_exposure(&env);
		ok &= expect(!near(weather.color_src_gain().r, 1.0f),
				"an open iris gate re-targets the modulator");
	}

	// --- sun veil: the dot^32 white-quad alpha + modulator-2 stop-down ------
	// [orig: Environment_ComputeSunGlareAndFogBlend @ 0x5ad610; the veil submit +
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
	// [orig: Environment_UpdateSunGlare @ 0x5ad130; Water_ComputeReflectedSunPoint].
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
		// Once a tick wrote back, the env-routed publication equals the
		// smoother (behaviour-identical without NVG / thermal) ...
		weather.tick_fixed(&env);
		const opennova::env::WeatherShaderGlobals ticked =
				opennova::env::build_weather_shader_globals(env, weather);
		ok &= expect(rgb_near(ticked.base.sun_light, weather.smooth_sun()) &&
						rgb_near(ticked.base.sky_ambient, weather.smooth_sky()) &&
						rgb_near(ticked.base.fog_color, weather.smooth_fog()) &&
						near(ticked.base.fog_end, env.fog_end_distance()) &&
						near(ticked.base.fog_start, env.fog_start()) &&
						ticked.base.fog_type == env.fog_type(),
				"the env-routed weather publication equals the written-back smoother");
		// ... and carries the thermal fog and terrain ramps the smoother lacks.
		env.set_thermal_view(true, true);
		const opennova::env::WeatherShaderGlobals thermal =
				opennova::env::build_weather_shader_globals(env, weather);
		ok &= expect(rgb_near(thermal.base.fog_color,
							 {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f}) &&
						rgb_near(thermal.base.sun_light,
								{16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f}) &&
						rgb_near(thermal.base.sky_ambient,
								{240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f}) &&
						near(thermal.base.fog_end, ticked.base.fog_end) &&
						thermal.base.fog_type == ticked.base.fog_type &&
						rgb_near(thermal.base.fill_light, ticked.base.fill_light),
				"the weather publication carries the thermal fog and terrain ramps");
		ok &= expect(rgb_near(thermal.base.light_block, weather.smooth_sun()),
				"the weather publication keeps the cube's light block raw under thermal");
		ok &= expect(rgb_near(opennova::env::build_weather_shader_globals(
											env, weather, true)
									.base.fog_color,
							 env.build_scene_fog(true).color),
				"underwater lit water still outranks the thermal fog in the publication");
		env.set_thermal_view(false, false);
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

	// --- the overcast cross-fade, with and without an overcast table ---------
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 lerps the .env set
	//  toward the .trn/overcast.def set by the overcast blend whatever that set
	//  holds; an empty one is Environment_InitDefaults' zeros (env #41)]
	{
		EnvironmentState env;
		opennova::env::Config cfg = make_config();
		env.set_config(&cfg, true);
		WeatherRuntime weather;
		weather.prepare_world_driven(&env);
		env.set_time_of_day(1200.0f);
		const opennova::env::TodState clear = opennova::env::interpolate_tod(
				cfg.keyframes, 1200.0f, cfg.envscale);
		ok &= expect(rgb_near(env.sun_light_target(), clear.sun),
				"no overcast: the .env colors as they are");

		weather.state().overcast_for_tod_q16 = 0x8000;
		env.update_tod();
		const opennova::env::TodState to_black = opennova::env::blend_tod_states(
				clear, opennova::env::TodState{}, 0x8000);
		ok &= expect(rgb_near(env.sun_light_target(), to_black.sun) &&
						rgb_near(env.sky_ambient_target(), to_black.sky) &&
						rgb_near(env.fog_color_base_target(), to_black.fog),
				"overcast with no overcast table fades half way to black");
		ok &= expect(env.sun_light_target().r < clear.sun.r * 0.6f,
				"the faded sun is about half the clear one");

		opennova::env::Config overcast_cfg = opennova::env::make_default_config();
		opennova::env::Keyframe grey;
		grey.time = 1200;
		grey.sun = {0.2f, 0.2f, 0.2f};
		grey.sky = {0.5f, 0.5f, 0.5f};
		grey.fog = {0.3f, 0.3f, 0.3f};
		overcast_cfg.keyframes.assign(1, grey);
		env.set_overcast_config(&overcast_cfg);
		const opennova::env::TodState to_grey = opennova::env::blend_tod_states(clear,
				opennova::env::interpolate_tod(overcast_cfg.keyframes, 1200.0f,
						overcast_cfg.envscale),
				0x8000);
		ok &= expect(rgb_near(env.sun_light_target(), to_grey.sun) &&
						rgb_near(env.sky_ambient_target(), to_grey.sky),
				"overcast with a table fades half way to the table's colors");
		env.set_overcast_config(nullptr);
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
		// c9.x is the raw smoothed distance [orig: Render_Skybox @ 0x5792c2].
		const opennova::env::SkyFrameState sky =
				opennova::env::build_sky_frame(env);
		ok &= expect(sky.loaded && near(sky.fog_end, env.fog_level()) &&
						near(sky.fog_end, 640.0f) &&
						!near(sky.fog_end, device_end),
				"the dome fog end is the unscaled smoothed fog distance, not the "
				"overcast-scaled device end");
	}

	// --- the celestial submit values (UPL_INTENSITY) -------------------------
	{
		EnvironmentState env;
		opennova::env::Config cfg = make_config();
		cfg.fog_level = 700.0f;
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		const opennova::env::Vec3 cam{10.0f, 20.0f, 30.0f};
		// The discs share one flush but not one value: each batch entry
		// snapshots its material's registers at submit and the flush restores
		// them before its RgbGen [orig: Render_CollectRenderObjectsForBatch
		// @ 0x5d91c0..0x5d91de; CRenderBatchQueue_FlushBatches
		// @ 0x5da1d6..0x5da1fd], so the sun keeps its own alpha beside a moon.
		const opennova::env::CelestialDiscsFrame discs =
				opennova::env::build_celestial_discs_frame(env, cam);
		const int moon_alpha = opennova::env::celestial_moon_alpha_fixed(700.0f, 0, false);
		ok &= expect(discs.sun_upl == 0x10000 && discs.sun_q3_upl == 0x10000,
				"the sun keeps its own alpha in both passes");
		ok &= expect(discs.moon_upl == moon_alpha && moon_alpha == 0x8000,
				"the moon takes its ramp, (700-400)/600");
		ok &= expect(discs.moon_q3_upl ==
						opennova::env::celestial_moon_alpha_fixed(700.0f, 0, true),
				"the moon's bloom redraw takes the fog-shader leg");
		ok &= expect(near(discs.sun_position.x, cam.x + env.sun_direction().x * 64.0f) &&
						near(discs.moon_position.y,
								cam.y + env.moon_direction().y * 64.0f),
				"the discs place at camera + direction * 64");
		// The glow folds the MAIN camera's view dot and skips its submit at a
		// non-positive alpha [orig: Render_SkyboxSunGlow @ 0x5ad0ae].
		const opennova::env::GlareFrame facing =
				opennova::env::build_glare_frame(env, cam, 0x10000, 255);
		ok &= expect(facing.drawn && facing.upl ==
						opennova::env::glare_glow_alpha_fixed(0x10000, 255, 0, 0, true),
				"facing the sun the glow draws the witnessed alpha");
		const opennova::env::GlareFrame away =
				opennova::env::build_glare_frame(env, cam, -0x8000, 255);
		ok &= expect(!away.drawn && away.upl == 0 && !away.q3_drawn,
				"looking away submits no glow in either pass");
		ok &= expect(opennova::env::kCelestialUplRegister == 32,
				"UPL_INTENSITY is CTRL ordinal 32 (g_CtrlGlobalUplIntensity @ 0x83fde8)");
	}

	// --- the dome constants under NVG and the thermal view -------------------
	{
		EnvironmentState env;
		opennova::env::Config cfg = make_config();
		cfg.advanced_clouds = 1;
		cfg.keyframes[0].skybase = {60.0f / 255.0f, 80.0f / 255.0f, 140.0f / 255.0f};
		cfg.keyframes[0].skybright = {20.0f / 255.0f, 20.0f / 255.0f, 32.0f / 255.0f};
		cfg.keyframes[0].skyhighlight = {150.0f / 255.0f, 160.0f / 255.0f, 150.0f / 255.0f};
		cfg.keyframes[0].cloudbase = {140.0f / 255.0f, 140.0f / 255.0f, 140.0f / 255.0f};
		cfg.keyframes[0].cloudhighlight = {20.0f / 255.0f, 24.0f / 255.0f, 10.0f / 255.0f};
		cfg.keyframes[0].cloudedge = {150.0f / 255.0f, 160.0f / 255.0f, 170.0f / 255.0f};
		cfg.keyframes[0].skyfog = {80.0f / 255.0f, 96.0f / 255.0f, 100.0f / 255.0f};
		env.set_config(&cfg, true);
		env.set_time_of_day(1200.0f);
		// make_config's envscale 0.5 halves the authored keyframe bytes; the
		// dome unpacks the scaled render bytes.
		const float base_b = env.sky_base().b * 255.0f;
		const float edge_r = env.cloud_edge().r * 255.0f;
		const float highlight_g = env.sky_highlight().g * 255.0f;
		const opennova::env::SkyFrameState plain = opennova::env::build_sky_frame(env);
		ok &= expect(!plain.flat_pass && near(base_b, 70.0f) &&
						near(plain.sky_base.b, base_b * (2.0f / 255.0f)),
				"the plain unpack is byte * 2/255 [orig: Color_UnpackToFloat4 @ 0x578985]");
		// NVG level 0: f = 0.2, a = 0.05, b = 0.0003125 per channel
		// [orig: Color_UnpackToFloat4 @ 0x578913..0x57897b].
		env.set_nvg_view(true, 0);
		const opennova::env::SkyFrameState nvg0 = opennova::env::build_sky_frame(env);
		ok &= expect(near(nvg0.sky_base.b, base_b * 0.05f * (2.0f / 255.0f) + 0.0003125f) &&
						near(nvg0.cloud_edge.r, edge_r * 0.05f * (2.0f / 255.0f) + 0.0003125f),
				"first-person NVG dims every dome constant to byte*a*2/255 + b at level 0");
		env.set_nvg_view(true, 4);
		const opennova::env::SkyFrameState nvg4 = opennova::env::build_sky_frame(env);
		ok &= expect(near(nvg4.sky_highlight.g,
							 highlight_g * 0.25f * (2.0f / 255.0f) + 0.0015625f),
				"NVG level 4: f = 1.0, a = 0.25, b = 0.0015625");
		ok &= expect(rgb_near(nvg4.skyfog_color, plain.skyfog_color),
				"the dome fog color never takes the NVG dim (FOGCOLOR is the packed block)");
		env.set_nvg_view(false, 0);
		// The thermal view: sky constants 1.0, cloud constants 0.9, fog 0x808080
		// [orig: Render_Skybox @ 0x579377..0x579447; SkyDome_RenderWithSkyfog @ 0x579cbc].
		env.set_thermal_view(true, false);
		const opennova::env::SkyFrameState thermal = opennova::env::build_sky_frame(env);
		ok &= expect(near(thermal.sky_base.r, 1.0f) && near(thermal.sky_bright.g, 1.0f) &&
						near(thermal.sky_highlight.b, 1.0f) && near(thermal.cloud_base.r, 0.9f) &&
						near(thermal.cloud_highlight.g, 0.9f) && near(thermal.cloud_edge.b, 0.9f),
				"the thermal dome is white: sky 1.0, clouds 0.9");
		ok &= expect(rgb_near(thermal.skyfog_color,
							 {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f}),
				"the thermal sky wrapper fogs toward 0x808080");
		env.set_nvg_view(true, 2);
		const opennova::env::SkyFrameState thermal_nvg = opennova::env::build_sky_frame(env);
		ok &= expect(near(thermal_nvg.sky_base.r, 1.0f) && near(thermal_nvg.cloud_edge.r, 0.9f),
				"the thermal overwrite runs after the NVG unpack");
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

	// --- an empty keyframe table: the parsed targets the load left (env #39) -------
	// With no keyframe table the per-tick compute writes no color block [orig:
	// Environment_ComputeTimeOfDayColors @ 0x57de40, the gate @ 0x57de8a], so the blocks
	// hold their parsed targets: what Environment_InitDefaults wrote when the .env was
	// skipped [orig: @ 0x57c03a..0x57c17d: light 0x646440, sky 0x404064, ground 0x202020,
	// fog and skyfog 0xC0C0FF, the sky/cloud ramps zero], the parse's scratch keyframe
	// when it parsed [orig: Environment_LoadTimeOfDayConfig, seeded @ 0x57db54..0x57db7e,
	// copied @ 0x57dce0..0x57dd57], a color line outside every tod_begin block written
	// into it.
	{
		const auto packed = [](uint32_t value) { return opennova::env::packed_to_rgb01(value); };
		const auto holds_defaults = [&](const EnvironmentState &env) {
			return rgb_near(env.sun_light_target(), packed(0x646440)) &&
					rgb_near(env.sky_ambient_target(), packed(0x404064)) &&
					rgb_near(env.fill_light_target(), packed(0x202020)) &&
					rgb_near(env.fog_color_base_target(), packed(0xC0C0FF)) &&
					rgb_near(env.skyfog_color_target(), packed(0xC0C0FF)) &&
					rgb_near(env.sky_base_target(), {}) && rgb_near(env.sky_bright_target(), {}) &&
					rgb_near(env.sky_highlight_target(), {}) && rgb_near(env.cloud_base_target(), {}) &&
					rgb_near(env.cloud_highlight_target(), {}) && rgb_near(env.cloud_edge_target(), {});
		};
		// No .env: the defaults alone.
		const opennova::env::Config missing;
		EnvironmentState skipped;
		skipped.set_config(&missing, true);
		skipped.set_time_of_day(2200.0f);
		ok &= expect(!skipped.has_tod_keyframes() && holds_defaults(skipped),
				"a skipped .env leaves the InitDefaults targets, day or night");
		// A .env with no tod_begin block: the scratch keyframe, its seeded colors the same.
		opennova::env::Config untimed;
		std::string error;
		std::istringstream plain("fog_level 640\r\nfog_type 2\r\n");
		ok &= expect(opennova::env::load_env(plain, untimed, error), "a keyframe-less .env parses");
		EnvironmentState parsed;
		parsed.set_config(&untimed, true);
		parsed.set_time_of_day(1200.0f);
		ok &= expect(!parsed.has_tod_keyframes() && holds_defaults(parsed),
				"a .env without keyframes leaves the scratch keyframe's seeded targets");
		// Its color lines outside a block land in the scratch keyframe (baked with the
		// envscale read before them [orig: Color_ScaleRGBAndPack @ 0x57f890]); fog mirrors
		// into skyfog while skyfog holds its seed [orig: @ 0x57c9b8].
		opennova::env::Config naked;
		std::istringstream lines(
				"sky_rgb 10,20,30\r\nenvscale 0.5\r\nsun_rgb 101,51,201\r\nfog_rgb 40,50,60\r\n"
				"cloudedge_rgb 7,8,9\r\ntod_begin 1200\r\ntod_end\r\nground_rgb 1,2,3\r\n");
		ok &= expect(opennova::env::load_env(lines, naked, error), "naked color lines parse");
		naked.keyframes.clear(); // the table empty, the scratch kept
		EnvironmentState scratch;
		scratch.set_config(&naked, true);
		scratch.set_time_of_day(1200.0f);
		ok &= expect(rgb_near(scratch.sky_ambient_target(), packed(0x0A141E)) &&
						rgb_near(scratch.sun_light_target(), packed(0x321964)) &&
						rgb_near(scratch.fog_color_base_target(), packed(0x14191E)) &&
						rgb_near(scratch.skyfog_color_target(), packed(0x14191E)) &&
						rgb_near(scratch.cloud_edge_target(), packed(0x030404)) &&
						rgb_near(scratch.fill_light_target(), packed(0x000101)),
				"naked color lines become the untimed targets, baked by the envscale before them");
		// A weather-driven world snaps its blocks to those targets at the start [orig:
		// Environment_SnapStateToTargets @ 0x57d1e0].
		EnvironmentState driven;
		driven.set_config(&missing, true);
		driven.set_time_of_day(1200.0f);
		WeatherRuntime runtime;
		runtime.prepare_world_driven(&driven);
		ok &= expect(rgb_near(runtime.smooth_sky(), packed(0x404064), 2.0f / 255.0f) &&
						rgb_near(runtime.smooth_fill(), packed(0x202020), 2.0f / 255.0f),
				"the weather snap starts the world on those targets");
	}

	// A terrain's water keywords through the mission's load (env #43, #44): every shipped .trn
	// writes water_rgb and water_murk, no shipped .env a murk; the murk the load leaves is what
	// the underwater fog, the murk overlay and the water strips read, and the water plane takes
	// the header's override, then the .env's line, then the .trn's [orig:
	// Environment_LoadTimeOfDayConfig @ 0x57db30; Terrain_Init @ 0x60fcb1..0x60fcba].
	{
		const std::string trn = "terrain_name \"Dvxi5\"\r\nwater_height 21\r\nwater_rgb 108,81,48\r\nwater_murk .3\r\n";
		const std::string env_text = "water_rgb 56,59,39\r\nfog_level 640\r\n";
		opennova::env::MissionEnvTexts texts;
		texts.terrain = &trn;
		texts.environment = &env_text;
		opennova::env::MissionEnv loaded;
		ok &= expect(opennova::env::load_mission_env(texts, loaded), "the .trn and the .env load");
		EnvironmentState state;
		state.set_config(&loaded.config, true);
		state.set_time_of_day(1200.0f);
		ok &= expect(near(state.water_murk(), 0.3f) &&
						near(state.build_scene_fog(true).end, opennova::env::fog_end_underwater(0.3f)) &&
						opennova::env::underwater_murk_overlay_alpha_byte(state.water_murk()) == 156,
				"the terrain's murk is the underwater fog's and the overlay's (128 + trunc(96 * 0.3))");
		const opennova::env::WaterFrameInputs inputs = opennova::env::build_water_frame_inputs(&state, 0.6f);
		ok &= expect(near(inputs.murk, 0.3f), "the water strips read the terrain's murk");
		opennova::env::WaterHeightRungs rungs;
		rungs.terrain_height = 10.5f;
		rungs.has_loaded_terrain = true;
		ok &= expect(near(opennova::env::resolve_water_height(rungs, &state, 0.0f), 10.5f),
				"the .trn's height stands where the .env writes none");
		const std::string env_height = "water_height 30\r\n";
		texts.environment = &env_height;
		ok &= expect(opennova::env::load_mission_env(texts, loaded), "the .env with a height loads");
		state.set_config(&loaded.config, true);
		ok &= expect(near(opennova::env::resolve_water_height(rungs, &state, 0.0f), 15.0f),
				"the .env's water_height writes after the .trn's: 30 half units over the terrain's");
		rungs.has_mission_override = true;
		rungs.mission_override = 0.0f;
		ok &= expect(near(opennova::env::resolve_water_height(rungs, &state, 7.0f), 0.0f),
				"the header's flagged override, zero included, over both");
	}

	// The rung that gave the plane its height (resolve_water_rung, the ladder resolve_water_height
	// answers the height of), and the rungs a mission's load fills from its header's overrides and
	// its terrain (mission_water_rungs) [orig: Game_LoadTerrainDuringConnect @ 0x520710].
	{
		using opennova::env::ResolvedWaterHeight;
		using opennova::env::WaterRung;
		const int no_colour[3] = {0, 0, 0};
		const opennova::env::BmsEnvOverrides none;
		const opennova::env::BmsEnvOverrides flagged =
				opennova::env::bms_env_overrides_from_header(0x1, 24, 0, no_colour, no_colour, 0);
		opennova::env::WaterHeightRungs rungs = opennova::env::mission_water_rungs(flagged, 10.5f, true);
		ok &= expect(rungs.has_mission_override && near(rungs.mission_override, 12.0f) &&
						near(rungs.terrain_height, 10.5f) && rungs.has_loaded_terrain,
				"the header's flagged override in half units, the terrain's height as given");
		rungs = opennova::env::mission_water_rungs(none, 0.0f, false);
		ok &= expect(!rungs.has_mission_override && rungs.mission_override == 0.0f && !rungs.has_loaded_terrain,
				"an unflagged header sets no override");

		opennova::env::Config with_height;
		with_height.water_height = 30.0f;
		with_height.water_height_set = true;
		EnvironmentState env_height;
		env_height.set_config(&with_height, true);
		opennova::env::Config without_height;
		EnvironmentState env_none;
		env_none.set_config(&without_height, true);
		ResolvedWaterHeight resolved =
				opennova::env::resolve_water_rung(opennova::env::mission_water_rungs(flagged, 10.5f, true), &env_height, 0.0f);
		ok &= expect(resolved.rung == WaterRung::Mission && near(resolved.height, 12.0f), "the header's rung first");
		resolved = opennova::env::resolve_water_rung(opennova::env::mission_water_rungs(none, 10.5f, true), &env_height, 0.0f);
		ok &= expect(resolved.rung == WaterRung::Environment && near(resolved.height, 15.0f),
				"the environment's water_height next");
		resolved = opennova::env::resolve_water_rung(opennova::env::mission_water_rungs(none, 10.5f, true), &env_none, 0.0f);
		ok &= expect(resolved.rung == WaterRung::Terrain && near(resolved.height, 10.5f), "then the terrain's");
		resolved = opennova::env::resolve_water_rung(opennova::env::mission_water_rungs(none, 0.0f, true), nullptr, 7.0f);
		ok &= expect(resolved.rung == WaterRung::None && resolved.height == 0.0f,
				"a loaded terrain with no height clears the plane");
		resolved = opennova::env::resolve_water_rung(opennova::env::mission_water_rungs(none, 0.0f, false), nullptr, 7.0f);
		ok &= expect(resolved.rung == WaterRung::None && near(resolved.height, 7.0f),
				"with no source the current plane stands");
	}

	// The mission's config over a file set by name (load_mission_env_config): the header's terrain
	// and environment names with their extensions, its overrides over what the load made.
	{
		class Files : public opennova::FileSource {
		public:
			std::string trn = "water_height 21\r\nwater_murk .3\r\n";
			std::string env = "fog_level 640\r\n";
			bool read(const std::string &name, std::vector<uint8_t> &out) const override {
				const std::string *text = name == "dvxi5.trn" ? &trn : name == "dvxi5.env" ? &env : nullptr;
				if (text == nullptr) return false;
				out.assign(text->begin(), text->end());
				return true;
			}
			uint64_t stamp(const std::string &name) const override {
				return name == "dvxi5.trn" || name == "dvxi5.env" ? 1 : 0;
			}
		} files;
		const int fog_colour[3] = {0, 0, 0};
		const opennova::env::BmsEnvOverrides overrides =
				opennova::env::bms_env_overrides_from_header(0x2, 0, 300, fog_colour, fog_colour, 0);
		opennova::env::MissionEnv loaded;
		ok &= expect(opennova::env::load_mission_env_config(files, "dvxi5", "dvxi5", overrides, loaded) &&
						loaded.environment && near(loaded.config.fog_level, 300.0f) &&
						near(loaded.config.water_height, 21.0f) && near(loaded.config.water_murk, 0.3f),
				"the file set's .trn and .env under the header's fog override");
		ok &= expect(!opennova::env::load_mission_env_config(files, "dvxi5", "gone", overrides, loaded) &&
						!loaded.environment && near(loaded.config.fog_level, 300.0f),
				"a missing .env is skipped, the overrides still over the earlier passes");
	}

	if (!ok) {
		return 1;
	}
	std::printf("environment_state_test: all pins green\n");
	return 0;
}
