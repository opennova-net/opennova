#include <runtime/environment/weather_runtime.h>

#include <runtime/renderer/light_runtime.h>

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

WeatherRuntime::WeatherRuntime() = default;

void WeatherRuntime::attach_state(world::WeatherState *state, EnvironmentState *env) {
	standalone_ = state == nullptr;
	if (env != nullptr) {
		env->bind_weather(state);
		if (state == nullptr) {
			env->ensure_standalone_weather_seeded(configured_wind_intensity_);
		}
		state_ = env->weather();
	} else {
		state_ = state != nullptr ? state : &idle_state_;
	}
	resync_colors();
}

void WeatherRuntime::set_wind_strength_pct(float pct) {
	remember_wind_strength_pct(pct);
	state_->set_wind_scale(configured_wind_intensity_);
}

void WeatherRuntime::remember_wind_strength_pct(float pct) {
	configured_wind_intensity_ = static_cast<int>(pct / 100.0f * 256.0f);
}

float WeatherRuntime::wind_strength_pct() const {
	return static_cast<float>(state_->wind_scale()) / 256.0f * 100.0f;
}

void WeatherRuntime::process_delta(EnvironmentState *env, double delta) {
	if (world_tick_driven_) {
		return;
	}
	tick_credit_ += std::max(delta, 0.0) * kTicksPerSecond;
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
			tick_fixed(env);
		}
	}
}

void WeatherRuntime::set_world_tick_driven(bool enabled) {
	if (world_tick_driven_ == enabled) {
		return;
	}
	world_tick_driven_ = enabled;
	tick_credit_ = 0.0;
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
	if (standalone()) {
		state_->mission_start_init();
	}
	for (int i = 0; i < kMissionStartPrewarmTicks; ++i) {
		tick_fixed(env);
	}
}

void WeatherRuntime::reset_for_environment(EnvironmentState *env,
		bool world_tick_driven) {
	// The embedder retains this runtime across missions, but retail's
	// environment start re-seeds the PRNG and clears every transient weather
	// channel. The private state re-seeds from the env's parsed config (a
	// mission's World state is seeded by the embedder's ONE derivation,
	// env::weather_seed_from_config, at its own boundary).
	if (standalone()) {
		if (env != nullptr) {
			env->bind_weather(nullptr);
			state_ = env->weather();
			if (env->is_loaded()) {
				env->reset_standalone_weather(configured_wind_intensity_);
			} else {
				*state_ = world::WeatherState{};
				state_->set_wind_scale(configured_wind_intensity_);
			}
		} else {
			idle_state_ = world::WeatherState{};
			idle_state_.set_wind_scale(configured_wind_intensity_);
			state_ = &idle_state_;
		}
	} else if (env != nullptr) {
		env->bind_weather(state_);
	}
	iris_samples_.clear();
	world_tick_driven_ = world_tick_driven;
	tick_credit_ = 0.0;
	resync_colors();
	tick_weather(env, 0);
}

void WeatherRuntime::tick_render(EnvironmentState *env) {
	tick_weather(env, 1);
}

void WeatherRuntime::tick_fixed(EnvironmentState *env) {
	if (standalone()) {
		world::WeatherTickEvents events;
		state_->tick_sim(nullptr, events);
	}
	tick_weather(env, 1);
}

void WeatherRuntime::resync_colors_now(EnvironmentState *env) {
	resync_colors();
	tick_weather(env, 0);
}

void WeatherRuntime::set_wind_duration_seconds(int seconds) {
	state_->core.set_wind_duration_ticks(6 * seconds);
	if (seconds > 0 && state_->core.oscillator.intensity == 0) {
		state_->set_wind_scale(256);
	}
}

int WeatherRuntime::wind_duration_seconds() const {
	return state_->core.wind_duration_ticks / 6;
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
	WeatherCore &core = state_->core;
	core.set_exposure_from_iris_samples(iris_samples_.data(),
			static_cast<int>(iris_samples_.size()),
			packed_to_rgb01(core.sky_color_blocks.ceiling.pre_mod_color),
			packed_to_rgb01(core.sky_color_blocks.floor.pre_mod_color),
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
	WeatherCore &core = state_->core;
	for (int i = 0; i < kExposureSettleIterations; ++i) {
		feed_exposure_target(env);
		// Modulator-2, the modulator, then every color block against the
		// fresh modulator — the witnessed same-tick order, minus the
		// sequencer/scalar/cloud-scroll legs this seam freezes
		// [orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c].
		const int dim = core.hit_dim.intensity;
		core.modulator_chain.tick(dim);
		const uint32_t modulator_packed = core.modulator_chain.render_color();
		core.sun_block.tick(modulator_packed, dim);
		core.sky_block.tick(modulator_packed, dim);
		core.fill_block.tick(modulator_packed, dim);
		core.fog_block.tick(modulator_packed, dim);
		core.sky_color_blocks.tick_skyfog(modulator_packed, dim);
		core.sky_color_blocks.tick_statics(modulator_packed, dim);
		core.sky_color_blocks.tick_dome(modulator_packed, dim);
	}
	write_weather_state(*env);
}

uint32_t WeatherRuntime::lightning_packed(const EnvironmentState *env) const {
	// Env_LightningColor: the .env lightning_rgb at the seed (envscaled like
	// every global parser color), the `lightning` WAC after — the weather
	// home carries it [orig: @ 0x26c646c].
	if (state_->valid) {
		return state_->lightning_color & 0xFFFFFFu;
	}
	if (env != nullptr && env->config() != nullptr) {
		return pack_rgb(env->lightning_color_target()) & 0xFFFFFFu;
	}
	return 0xFFFFFFu;
}

void WeatherRuntime::tick_weather(EnvironmentState *env, int tick_count) {
	if (env == nullptr || !env->is_loaded()) {
		return;
	}
	// The clock the sim advanced this tick -> the TOD targets
	// [orig: Environment_ComputeTimeOfDayColors(curtime + advance) @ 0x57e9c7].
	if (env->weather() != state_) {
		// A runtime that never attached follows the env's own home.
		if (state_ == &idle_state_) {
			state_ = env->weather();
		} else {
			env->bind_weather(standalone_ ? nullptr : state_);
			state_ = env->weather();
		}
	}
	if (standalone_) {
		env->ensure_standalone_weather_seeded(configured_wind_intensity_);
	}
	env->sync_clock_from_weather();
	// Claim the current render colors + shader globals: from here on the
	// env's update_tod refreshes TARGETS only and this tick's writeback owns
	// the currents — the witnessed split
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40], and the fix for
	// the raw-vs-modulated strobe the per-tick mission clock exposed.
	if (!env->is_weather_driven()) {
		env->set_weather_driven(true);
	}
	WeatherCore &core = state_->core;
	if (!colors_synced_) {
		// Snap TO THE TARGETS — the witnessed snap form
		// [orig: Environment_SnapStateToTargets @ 0x57d1e0]: at mission start
		// the targets ARE the load-time keyframes, and after a discrete scrub
		// (resync_colors) they are the new keyframes. Seeding from the env
		// CURRENTS would re-seed the stale writeback (the currents are
		// weather-owned once driven). The static ceiling/cloud/floor targets
		// are seeded here and by the WAC color commands only — retail never
		// refreshes them per tick (the LABEL_13 seeding @ 0x57dce0).
		core.fill_block.snap(pack_rgb(env->fill_light_target()));
		core.sun_block.snap(pack_rgb(env->sun_light_target()));
		core.fog_block.snap(pack_rgb(env->fog_color_base_target()));
		core.sky_block.snap(pack_rgb(env->sky_ambient_target()));
		core.sky_color_blocks.snap({
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
	if (env->config() != nullptr) {
		feed_exposure_target(env);
	}
	// The eleven TOD-keyframed blocks SNAP to the tick's keyframe colors —
	// channels, render slot and targets — so their step finds nothing to
	// chase: only their lightning additive and the modulation still apply,
	// and the WAC sun/sky/ground/fogcolor/skyfogcolor targets are overwritten
	// before they can act. The three statics (ceiling/cloud/floor) and the two
	// modulators are the only blocks that step, toward their seeded /
	// WAC-written targets [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57de40 — the block writes @ 0x57e078..0x57e3c9 at the top of every
	// weather tick (@ 0x57e9c7) while a keyframe table exists
	// (Env_EnvSnapshotCount @ 0x57de8a); WacCmd_Sun @ 0x4edcd0 writes [11] +
	// the step deltas]. Without a keyframe table the compute returns before
	// the writes: no target is touched here and the eleven blocks chase
	// whatever the load seeded or the WAC wrote.
	const bool keyframed = env->has_tod_keyframes();
	for (int i = 0; i < tick_count; ++i) {
		if (keyframed) {
			SkyWeatherColorBlocks &sky_blocks = core.sky_color_blocks;
			core.sun_block.snap_keyframe(pack_rgb(env->sun_light_target()));
			core.sky_block.snap_keyframe(pack_rgb(env->sky_ambient_target()));
			core.fill_block.snap_keyframe(pack_rgb(env->fill_light_target()));
			core.fog_block.snap_keyframe(pack_rgb(env->fog_color_base_target()));
			sky_blocks.skyfog.snap_keyframe(pack_rgb(env->skyfog_color_target()));
			sky_blocks.skybase.snap_keyframe(pack_rgb(env->sky_base_target()));
			sky_blocks.skybright.snap_keyframe(pack_rgb(env->sky_bright_target()));
			sky_blocks.skyhighlight.snap_keyframe(pack_rgb(env->sky_highlight_target()));
			sky_blocks.cloudbase.snap_keyframe(pack_rgb(env->cloud_base_target()));
			sky_blocks.cloudhighlight.snap_keyframe(pack_rgb(env->cloud_highlight_target()));
			sky_blocks.cloudedge.snap_keyframe(pack_rgb(env->cloud_edge_target()));
		}
		core.tick_render_blocks();
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
	// The smoothed scalar currents (env #27) are read through the env's bound
	// weather view — no copy to keep in step.
}

// --- smoothed reads --------------------------------------------------------

// One formula home: env::WeatherCore (env_weather_core.h) — shared with the
// Godot WeatherCore binding.
float WeatherRuntime::sway_amount() const {
	return state_->core.sway_amount();
}

float WeatherRuntime::sway_phase() const {
	return state_->core.sway_phase();
}

float WeatherRuntime::lightning_intensity() const {
	return state_->core.lightning_intensity();
}

Rgb WeatherRuntime::smooth_fill() const {
	return packed_to_rgb01(state_->core.fill_block.render_color);
}

Rgb WeatherRuntime::smooth_sun() const {
	return packed_to_rgb01(state_->core.sun_block.render_color);
}

Rgb WeatherRuntime::smooth_fog() const {
	return double_saturate(packed_to_rgb01(state_->core.fog_block.render_color));
}

Rgb WeatherRuntime::smooth_sky() const {
	return packed_to_rgb01(state_->core.sky_block.render_color);
}

Rgb WeatherRuntime::smooth_skyfog() const {
	const WeatherCore &core = state_->core;
	const Rgb blended = horizon_blend_skyfog(
			packed_to_rgb01(core.fog_block.render_color),
			packed_to_rgb01(core.sky_color_blocks.skyfog.render_color),
			static_cast<uint32_t>(core.scalar_channels.fog_dist_fp),
			static_cast<uint32_t>(state_->fog_reference_q16));
	return double_saturate(blended);
}

Rgb WeatherRuntime::smooth_ceiling() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.ceiling.render_color);
}

Rgb WeatherRuntime::smooth_cloud() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.cloud.render_color);
}

Rgb WeatherRuntime::smooth_floor() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.floor.render_color);
}

Rgb WeatherRuntime::smooth_sky_base() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.skybase.render_color);
}

Rgb WeatherRuntime::smooth_sky_bright() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.skybright.render_color);
}

Rgb WeatherRuntime::smooth_sky_highlight() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.skyhighlight.render_color);
}

Rgb WeatherRuntime::smooth_cloud_base() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.cloudbase.render_color);
}

Rgb WeatherRuntime::smooth_cloud_highlight() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.cloudhighlight.render_color);
}

Rgb WeatherRuntime::smooth_cloud_edge() const {
	return packed_to_rgb01(state_->core.sky_color_blocks.cloudedge.render_color);
}

Rgb WeatherRuntime::color_src_gain() const {
	const std::array<float, 3> scale = renderer::unpack_modulator_scale(
			state_->core.modulator_chain.render_color() & 0xFFFFFFu);
	return Rgb{scale[0], scale[1], scale[2]};
}

uint32_t WeatherRuntime::terrain_light_combined_rgb() const {
	return pack_rgb(combine_terrain_light(smooth_sun(), smooth_sky())) & 0xFFFFFFu;
}

CloudUvOffsets WeatherRuntime::cloud_uv_offsets(float cam_x,
		float cam_z) const {
	return cloud_scroll_uv_offsets(state_->core.cloud_scroll, cam_x, cam_z);
}

float WeatherRuntime::cloud_uv_rate_per_second() const {
	return opennova::env::cloud_uv_rate_per_second(state_->core.cloud_scroll);
}

WaterUvState WeatherRuntime::water_uv_state(float cam_x, float cam_z,
		float fog_distance) const {
	return opennova::env::water_uv_state(state_->core.cloud_scroll, cam_x, cam_z,
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
	// The modulator /64 gain (iris exposure) for self-lit/effect shaders
	// [orig: Render_UnpackModulatorToLightScale @ 0x58db30].
	globals.color_src_gain = weather.color_src_gain();
	return globals;
}

} // namespace opennova::env
