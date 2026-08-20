#include "environment/weather_runtime.h"

#include <renderer/light_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace opennova::env {

namespace {

// Same rounding as ColorSmoother's packer (and the GUT vector hex idiom):
// byte = int(channel * 255 + 0.5); alpha packs opaque like a Godot Color's
// default 1.0 did.
uint32_t pack_rgb(const Rgb &color) {
	const auto to_byte = [](float v) -> uint32_t {
		return static_cast<uint32_t>(
				std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
	};
	return to_byte(color.b) | (to_byte(color.g) << 8) |
			(to_byte(color.r) << 16) | (255u << 24);
}

Rgb packed_to_rgb01(uint32_t packed) {
	Rgb c;
	c.r = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
	c.g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
	c.b = static_cast<float>(packed & 0xFF) / 255.0f;
	return c;
}

} // namespace

void WeatherRuntime::set_wind_strength_pct(float pct) {
	configured_wind_intensity_ = static_cast<int>(pct / 100.0f * 256.0f);
	core_.set_wind_intensity(configured_wind_intensity_);
}

float WeatherRuntime::wind_strength_pct() const {
	return static_cast<float>(configured_wind_intensity_) / 256.0f * 100.0f;
}

void WeatherRuntime::process_delta(EnvironmentState *env, double delta) {
	if (world_tick_driven_) {
		return;
	}
	tick_credit_ += std::max(delta, 0.0) *
			static_cast<double>(kWeatherTickHz);
	int tick_count = static_cast<int>(std::floor(tick_credit_ + 1.0e-9));
	if (tick_count > 0) {
		tick_credit_ = std::max(0.0, tick_credit_ - static_cast<double>(tick_count));
		if (tick_count > kMaxCatchupTicks) {
			tick_count = kMaxCatchupTicks;
			tick_credit_ = 0.0;
		}
	}
	if (tick_count <= 0) {
		tick_weather(env, 0);
	} else {
		for (int i = 0; i < tick_count; ++i) {
			tick_weather(env, 1);
		}
	}
}

int WeatherRuntime::consume_world_tick_credits(double delta) {
	if (!world_tick_driven_) {
		return 0;
	}
	tick_credit_ += std::max(delta, 0.0) *
			static_cast<double>(kWeatherTickHz);
	int tick_count = static_cast<int>(std::floor(tick_credit_ + 1.0e-9));
	if (tick_count <= 0) {
		return 0;
	}
	tick_credit_ = std::max(0.0, tick_credit_ - static_cast<double>(tick_count));
	if (tick_count > kMaxCatchupTicks) {
		tick_count = kMaxCatchupTicks;
		tick_credit_ = 0.0;
	}
	return tick_count;
}

void WeatherRuntime::set_world_tick_driven(bool enabled) {
	if (world_tick_driven_ == enabled) {
		return;
	}
	world_tick_driven_ = enabled;
	tick_credit_ = 0.0f;
}

void WeatherRuntime::prepare_world_driven(EnvironmentState *env) {
	reset_for_environment(env, true);
}

void WeatherRuntime::prepare_autonomous(EnvironmentState *env) {
	reset_for_environment(env, false);
}

void WeatherRuntime::prewarm_mission_start(EnvironmentState *env) {
	if (env == nullptr) {
		return;
	}
	for (int i = 0; i < kMissionStartPrewarmTicks; ++i) {
		env->advance_mission_clock(1);
		tick_weather(env, 1);
	}
}

void WeatherRuntime::reset_for_environment(EnvironmentState *env,
		bool world_tick_driven) {
	// The embedder retains this runtime across missions, but retail's
	// environment start re-seeds the PRNG and clears every transient weather
	// channel. A fresh core is the single complete reset for oscillator/rings,
	// lightning, rain, color/modulator blocks, scalar springs, and
	// cloud-scroll accumulators.
	core_ = WeatherCore{};
	core_.set_wind_intensity(configured_wind_intensity_);
	if (env != nullptr && env->is_loaded()) {
		// Target refresh [orig: Environment_SnapStateToTargets @ 0x57d1e0].
		core_.scalar_channels.fog_dist_target_fp =
				static_cast<int32_t>(env->fog_level_target() * 65536.0f);
		core_.scalar_channels.sky_height_target_fp =
				static_cast<int32_t>(env->sky_height_target() * 65536.0f);
		// Local mission initialization snaps scalar currents after authored
		// targets are installed. Network receive deliberately never calls
		// this.
		core_.scalar_channels.snap_currents_to_targets();
	}
	iris_samples_.clear();
	world_tick_driven_ = world_tick_driven;
	tick_credit_ = 0.0f;
	resync_colors();
	tick_weather(env, 0);
}

void WeatherRuntime::tick_fixed(EnvironmentState *env) {
	tick_weather(env, 1);
}

void WeatherRuntime::resync_colors_now(EnvironmentState *env) {
	resync_colors();
	tick_weather(env, 0);
}

bool WeatherRuntime::network_snapshot(const EnvironmentState *env,
		NetEnvSnapshot &out) const {
	if (env == nullptr || !env->is_loaded()) {
		return false;
	}
	const float fog_current =
			static_cast<float>(core_.scalar_channels.fog_dist_fp) / 65536.0f;
	out.fog_target_q16 = static_cast<int>(
			std::lround(env->fog_level_target() * 65536.0f));
	out.fog_current_q16 = static_cast<int>(std::lround(fog_current * 65536.0f));
	out.fog_accel_clamp = core_.scalar_channels.fog_step_fp;
	out.tod_fixed24 = env->mission_time_fixed24();
	out.tod_advance_per_tick = env->mission_advance_per_tick();
	out.quake_ticks = env->network_quake_ticks();
	out.cloud_scroll_rate_target = static_cast<int>(
			std::lround(env->sky_speed() * 1024.0f));
	out.rain_pct_current_q16 = core_.scalar_channels.rain_pct_fp;
	out.overcast_blend_q16 = core_.scalar_channels.overcast_fp;
	out.precipitation_kind = env->network_precipitation_kind();
	return true;
}

void WeatherRuntime::apply_network_sample(EnvironmentState *env,
		const NetEnvSample &sample) {
	if (env == nullptr) {
		return;
	}
	env->apply_network_sample(sample);
	core_.scalar_channels.apply_network_sample(
			static_cast<uint16_t>(std::clamp(sample.fog_dist, 0, 0xFFFF)),
			static_cast<uint16_t>(std::clamp(sample.fog_accel, 0, 0xFFFF)),
			static_cast<uint8_t>(std::clamp(sample.rain_pct, 0, 0xFF)),
			static_cast<uint8_t>(std::clamp(sample.overcast, 0, 0xFF)));
	// Publish immediately even when this render frame contains no 62 Hz
	// quantum.
	tick_weather(env, 0);
}

void WeatherRuntime::set_wind_duration_seconds(int seconds) {
	core_.set_wind_duration_ticks(6 * seconds);
	if (seconds > 0 && core_.oscillator.intensity == 0) {
		core_.set_wind_intensity(256);
	}
}

int WeatherRuntime::wind_duration_seconds() const {
	return core_.wind_duration_ticks / 6;
}

void WeatherRuntime::set_iris_samples(const int32_t *samples, int count) {
	iris_samples_.assign(samples, samples + std::max(count, 0));
}

void WeatherRuntime::feed_exposure_target(EnvironmentState *env) {
	if (env == nullptr || env->config() == nullptr) {
		return;
	}
	// The iris auto-exposure target (env #17): the marched in-world gain
	// when the shell stamps samples, else the outdoor fallback — chased by
	// the modulator over 62 ticks; retail re-targets every render pass,
	// i.e. every tick
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538;
	//  compute_ambient_light_along_direction @ 0x5c7a00;
	//  curve terrain_sector_compute_lighting @ 0x5c7550].
	const Vec3 sun_dir = env->sun_direction();
	core_.set_exposure_from_iris_samples(iris_samples_.data(),
			static_cast<int>(iris_samples_.size()),
			packed_to_rgb01(core_.sky_color_blocks.ceiling.pre_mod_color),
			packed_to_rgb01(core_.sky_color_blocks.floor.pre_mod_color),
			sun_dir.x, sun_dir.y, sun_dir.z,
			env->config()->iris_percent, env->config()->iris_center);
}

void WeatherRuntime::settle_exposure(EnvironmentState *env) {
	if (env == nullptr || !env->is_loaded()) {
		return;
	}
	// A frozen fixture never runs tick_weather, so the modulator chain holds
	// its mission-reset identity snap and every published color keeps the
	// flat gain (the D-RLIT-2 fixture starvation). Iterate the witnessed
	// per-tick exposure legs to their fixed point at this pose. The retarget
	// recomputes the 62-tick step deltas from the shrinking distance every
	// iteration — the live asymptotic chase — so the worst-case 12.20
	// distance (255 render bytes) needs ~1000 iterations to quantize onto
	// the target; 1024 covers it.
	// A zero-tick pass first: claim the currents and run any pending
	// resync snap exactly like a live tick would before the chase.
	tick_weather(env, 0);
	constexpr int kExposureSettleIterations = 1024;
	for (int i = 0; i < kExposureSettleIterations; ++i) {
		feed_exposure_target(env);
		// Modulator-2, the modulator, then every color block against the
		// fresh modulator — the witnessed same-tick order, minus the
		// sequencer/scalar/cloud-scroll legs this seam freezes
		// [orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c].
		core_.modulator_chain.tick(core_.rain.intensity);
		const uint32_t modulator_packed = core_.modulator_chain.render_color();
		core_.sun_block.tick(modulator_packed, core_.rain.intensity);
		core_.sky_block.tick(modulator_packed, core_.rain.intensity);
		core_.fill_block.tick(modulator_packed, core_.rain.intensity);
		core_.fog_block.tick(modulator_packed, core_.rain.intensity);
		core_.sky_color_blocks.tick_skyfog(modulator_packed,
				core_.rain.intensity);
		core_.sky_color_blocks.tick_statics(modulator_packed,
				core_.rain.intensity);
		core_.sky_color_blocks.tick_dome(modulator_packed,
				core_.rain.intensity);
	}
	write_weather_state(*env);
}

void WeatherRuntime::tick_weather(EnvironmentState *env, int tick_count) {
	if (env == nullptr || !env->is_loaded()) {
		return;
	}
	// Claim the current render colors + shader globals: from here on the
	// env's update_tod refreshes TARGETS only and this tick's writeback owns
	// the currents — the witnessed split
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40], and the fix for
	// the raw-vs-modulated strobe the per-tick mission clock exposed.
	if (!env->is_weather_driven()) {
		env->set_weather_driven(true);
	}
	if (!colors_synced_) {
		// Snap TO THE TARGETS — the witnessed snap form
		// [orig: Environment_SnapStateToTargets @ 0x57d1e0]: at mission start
		// the targets ARE the load-time keyframes, and after a discrete scrub
		// (resync_colors) they are the new keyframes. Seeding from the env
		// CURRENTS would re-seed the stale writeback (the currents are
		// weather-owned once driven).
		core_.fill_block.snap(pack_rgb(env->fill_light_target()));
		core_.sun_block.snap(pack_rgb(env->sun_light_target()));
		core_.fog_block.snap(pack_rgb(env->fog_color_base_target()));
		core_.sky_block.snap(pack_rgb(env->sky_ambient_target()));
		core_.sky_color_blocks.snap({
				pack_rgb(env->skyfog_color_target()),
				pack_rgb(env->ceiling_color_target()),
				pack_rgb(env->cloud_tint_target()),
				pack_rgb(env->floor_color_target()),
				pack_rgb(env->sky_base_target()),
				pack_rgb(env->sky_bright_target()),
				pack_rgb(env->sky_highlight_target()),
				pack_rgb(env->cloud_base_target()),
				pack_rgb(env->cloud_highlight_target()),
				pack_rgb(env->cloud_edge_target()),
		});
		colors_synced_ = true;
	}
	if (tick_count <= 0) {
		write_weather_state(*env);
		return;
	}
	uint32_t lightning_packed = pack_rgb(Rgb{1.0f, 1.0f, 1.0f});
	if (env->config() != nullptr) {
		// lightning_rgb is a global parser color, so it takes the same
		// envscale engine view as the world-driven static blocks and water.
		lightning_packed = pack_rgb(env->lightning_color_target());
		feed_exposure_target(env);
	}
	// The smoothers chase the TOD keyframe targets, never their own written-
	// back output [orig: Environment_ComputeTimeOfDayColors @ 0x57de40
	// refreshes every block's target slot ahead of the weather tick]. The
	// cloud-scroll rate ramps toward sky_speed << 10 at the tick's tail
	// [orig: @ 0x57eecc; accumulators @ 0x57f1a5..0x57f1d1].
	// env #27: refresh the scalar spring targets from the PARSED values before
	// the tick (retail refreshes targets ahead of the smoothers; the snap
	// touches targets only — currents always ramp [orig: @ 0x57d1e0]).
	core_.scalar_channels.fog_dist_target_fp =
			static_cast<int32_t>(env->fog_level_target() * 65536.0f);
	core_.scalar_channels.sky_height_target_fp =
			static_cast<int32_t>(env->sky_height_target() * 65536.0f);
	core_.sky_color_blocks.set_targets({
			pack_rgb(env->skyfog_color_target()),
			pack_rgb(env->ceiling_color_target()),
			pack_rgb(env->cloud_tint_target()),
			pack_rgb(env->floor_color_target()),
			pack_rgb(env->sky_base_target()),
			pack_rgb(env->sky_bright_target()),
			pack_rgb(env->sky_highlight_target()),
			pack_rgb(env->cloud_base_target()),
			pack_rgb(env->cloud_highlight_target()),
			pack_rgb(env->cloud_edge_target()),
	});
	for (int i = 0; i < tick_count; ++i) {
		core_.tick(pack_rgb(env->fill_light_target()),
				pack_rgb(env->sun_light_target()),
				pack_rgb(env->fog_color_base_target()),
				pack_rgb(env->sky_ambient_target()),
				lightning_packed,
				env->sky_speed());
	}
	write_weather_state(*env);
}

void WeatherRuntime::write_weather_state(EnvironmentState &env) {
	env.set_fill_light(smooth_fill());
	env.set_sun_light(smooth_sun());
	env.set_fog_color_rt(smooth_fog());
	// The sky block joins the writeback set — entity hemi_sky serves the
	// smoothed+modulated block like fill/sun/fog [orig: Env_SkyBlock[0]
	// consumed by the entity-constants writer @ 0x5c8090].
	env.set_sky_ambient_rt(smooth_sky());
	env.set_static_colors_rt(smooth_ceiling(), smooth_cloud(), smooth_floor());
	env.set_sky_colors_rt(smooth_skyfog(), smooth_sky_base(),
			smooth_sky_bright(), smooth_sky_highlight(), smooth_cloud_base(),
			smooth_cloud_highlight(), smooth_cloud_edge());
	env.set_color_src_gain(color_src_gain());
	// env #27 writeback: the smoothed scalar currents flow back through the
	// env seam so every consumer (dome, water, object/terrain fog, the frame
	// clear) serves the ramp.
	env.set_smoothed_scalars(
			static_cast<float>(core_.scalar_channels.fog_dist_fp) / 65536.0f,
			static_cast<float>(core_.scalar_channels.sky_height_fp) / 65536.0f,
			static_cast<float>(core_.scalar_channels.sun_dim_fp) / 65536.0f,
			static_cast<float>(core_.scalar_channels.rain_pct_fp) / 65536.0f,
			static_cast<float>(core_.scalar_channels.overcast_fp) / 65536.0f);
}

// --- smoothed reads --------------------------------------------------------

// One formula home: env::WeatherCore (env_weather_core.h) — shared with the
// Godot WeatherCore binding.
float WeatherRuntime::sway_amount() const {
	return core_.sway_amount();
}

float WeatherRuntime::sway_phase() const {
	return core_.sway_phase();
}

float WeatherRuntime::lightning_intensity() const {
	return core_.lightning_intensity();
}

Rgb WeatherRuntime::smooth_fill() const {
	return packed_to_rgb01(core_.fill_block.render_color);
}

Rgb WeatherRuntime::smooth_sun() const {
	return packed_to_rgb01(core_.sun_block.render_color);
}

Rgb WeatherRuntime::smooth_fog() const {
	return double_saturate(packed_to_rgb01(core_.fog_block.render_color));
}

Rgb WeatherRuntime::smooth_sky() const {
	return packed_to_rgb01(core_.sky_block.render_color);
}

Rgb WeatherRuntime::smooth_skyfog() const {
	const Rgb blended = horizon_blend_skyfog(
			packed_to_rgb01(core_.fog_block.render_color),
			packed_to_rgb01(core_.sky_color_blocks.skyfog.render_color),
			static_cast<uint32_t>(core_.scalar_channels.fog_dist_fp),
			1024u << 16);
	return double_saturate(blended);
}

Rgb WeatherRuntime::smooth_ceiling() const {
	return packed_to_rgb01(core_.sky_color_blocks.ceiling.render_color);
}

Rgb WeatherRuntime::smooth_cloud() const {
	return packed_to_rgb01(core_.sky_color_blocks.cloud.render_color);
}

Rgb WeatherRuntime::smooth_floor() const {
	return packed_to_rgb01(core_.sky_color_blocks.floor.render_color);
}

Rgb WeatherRuntime::smooth_sky_base() const {
	return packed_to_rgb01(core_.sky_color_blocks.skybase.render_color);
}

Rgb WeatherRuntime::smooth_sky_bright() const {
	return packed_to_rgb01(core_.sky_color_blocks.skybright.render_color);
}

Rgb WeatherRuntime::smooth_sky_highlight() const {
	return packed_to_rgb01(core_.sky_color_blocks.skyhighlight.render_color);
}

Rgb WeatherRuntime::smooth_cloud_base() const {
	return packed_to_rgb01(core_.sky_color_blocks.cloudbase.render_color);
}

Rgb WeatherRuntime::smooth_cloud_highlight() const {
	return packed_to_rgb01(core_.sky_color_blocks.cloudhighlight.render_color);
}

Rgb WeatherRuntime::smooth_cloud_edge() const {
	return packed_to_rgb01(core_.sky_color_blocks.cloudedge.render_color);
}

Rgb WeatherRuntime::color_src_gain() const {
	const std::array<float, 3> scale = renderer::unpack_modulator_scale(
			core_.modulator_chain.render_color() & 0xFFFFFFu);
	return Rgb{scale[0], scale[1], scale[2]};
}

CloudUvOffsets WeatherRuntime::cloud_uv_offsets(float cam_x,
		float cam_z) const {
	return cloud_scroll_uv_offsets(core_.cloud_scroll, cam_x, cam_z);
}

float WeatherRuntime::cloud_uv_rate_per_second() const {
	return opennova::env::cloud_uv_rate_per_second(core_.cloud_scroll);
}

WaterUvState WeatherRuntime::water_uv_state(float cam_x, float cam_z,
		float fog_distance) const {
	return opennova::env::water_uv_state(core_.cloud_scroll, cam_x, cam_z,
			fog_distance);
}

WeatherShaderGlobals build_weather_shader_globals(
		const EnvironmentState &env, const WeatherRuntime &weather,
		bool underwater_view) {
	// The env's public hemisphere getters include the camera-gated NVG gain
	// rewrite; publishing the smoother directly would bypass it each tick.
	WeatherShaderGlobals globals;
	globals.base.fill_light = env.fill_light();
	globals.base.sun_light = weather.smooth_sun();
	globals.base.sky_ambient = env.sky_ambient();
	globals.base.fog_color = weather.smooth_fog();
	// Terrain tile DOT3 and foliage follow the current environment light
	// (sun by day, moon by night), not the always-solar sky highlight vector
	// [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	globals.base.sun_direction = env.light_direction();
	globals.base.fog_end = env.fog_end_distance();
	globals.base.fog_start = env.fog_start();
	globals.base.fog_type = env.fog_type();
	if (underwater_view) {
		const SceneFogValues fog = env.build_scene_fog(true);
		globals.base.fog_color = fog.color;
		globals.base.fog_end = fog.end;
		globals.base.fog_start = fog.start;
		globals.base.fog_type = fog.type;
	}
	globals.base.wind_sway_amount =
			std::max(0.25f, std::fabs(weather.sway_amount()));
	globals.base.wind_sway_phase = weather.sway_phase();
	// The modulator /64 gain (iris exposure) for self-lit/effect shaders
	// [orig: Render_UnpackModulatorToLightScale @ 0x58db30].
	globals.color_src_gain = weather.color_src_gain();
	return globals;
}

} // namespace opennova::env
