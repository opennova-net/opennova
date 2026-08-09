// The weather/light smoothing owner — the orchestration half of the witnessed
// weather tick, ported verbatim from nova_weather.gd (2026-08-09
// de-scripting). env::WeatherCore (engine/formats/env) owns the witnessed
// state cluster of [orig: Environment_UpdateWeatherTick @ 0x57e9b0]: the wind
// PRNG/sway oscillator, both lightning flash sequencers
// ([orig: Environment_SetLightningFlash @ 0x57d320] SET-per-epoch additives),
// rain fade, and all fourteen non-modulator color-block pipelines
// ([orig: interpolate_weather_color @ 0x57d9e0]). This runtime owns the
// embedding: the 62 Hz tick-credit accumulator (autonomous render-delta or
// world-driven exact ticks), the deterministic mission reset epoch, the
// 255-tick mission-start prewarm, the per-tick target-feed/snap/writeback
// sequence, the iris-exposure handoff, and the typed network environment
// snapshot/sample projection. The shell node owns only device work (node
// resolution, the _process hook, shader-global pushes).
// RE record: docs/env/env-tod-re.md.
#pragma once

#include "environment/environment_state.h"

#include <env/env_water_render.h>
#include <env/env_weather_core.h>

#include <cstdint>
#include <vector>

namespace opennova::env {

// The host-facing exact native sample (S2C 0x0A phase-2). The weather owns
// the live scalar currents; the environment state owns the authored targets
// and mission clock.
struct NetEnvSnapshot {
	int fog_target_q16 = 0;
	int fog_current_q16 = 0;
	int fog_accel_clamp = 0;
	int tod_fixed24 = 0;
	int tod_advance_per_tick = 0;
	int quake_ticks = 0;
	int cloud_scroll_rate_target = 0;
	int rain_pct_current_q16 = 0;
	int overcast_blend_q16 = 0;
	int precipitation_kind = 0;
};

class WeatherRuntime {
public:
	static constexpr float kWeatherTickHz = 62.0f;
	static constexpr int kMaxCatchupTicks = 31;
	// Retail settles the newly initialized environment through 255 complete
	// weather ticks before gameplay/network publication
	// [orig: sub_57F1E0 @ 0x57f878..0x57f880].
	static constexpr int kMissionStartPrewarmTicks = 255;

	// Env_WindScale units: 100% maps to 256 [orig: Environment_InitDefaults
	// @ 0x57c1d1, its only writer]. The oscillator's 15*prev feedback term is
	// stable only for intensity <= 273 — a 0..8192 mapping drove the 32-bit
	// state divergent (docs/env/env-tod-re.md).
	void set_wind_strength_pct(float pct);
	float wind_strength_pct() const;

	// The autonomous render-delta accumulator (standalone/editor previews).
	// No-ops while world-tick-driven; zero-quantum frames still run a
	// zero-tick publish so scrubs land.
	void process_delta(EnvironmentState *env, double delta);

	// The world composer owns the recovered 62 Hz weather/TOD accumulator
	// while a mission runtime is active.
	void set_world_tick_driven(bool enabled);
	bool world_tick_driven() const { return world_tick_driven_; }

	// Enter world-driven mode and snap every block at the mission's authored
	// T0 before the first clock advance. Lazy-snapping after T1 would skip
	// retail's first target chase.
	void prepare_world_driven(EnvironmentState *env);
	// Start an independently ticking environment from the same deterministic
	// mission reset epoch used by world-driven play.
	void prepare_autonomous(EnvironmentState *env);

	// The 255-tick settle; TOD targets are recomputed before every tick
	// [orig: sub_57F1E0 @ 0x57f878..0x57f880].
	void prewarm_mission_start(EnvironmentState *env);

	// Advance exactly one recovered weather tick. The embedder advances the
	// integer mission clock immediately before this call, so every target
	// read sees curtime + advance like Environment_UpdateWeatherTick.
	void tick_fixed(EnvironmentState *env);

	// Snap the smoothing state to the env's current targets on the next tick
	// — call after discrete TOD scrubs so the rendered runtime doesn't lag.
	void resync_colors() { colors_synced_ = false; }
	// Discrete runtime scrubs must update the rendered currents even while
	// the mission transport is paused (no world-driven tick runs): a
	// zero-tick refresh snaps the core to the new targets and writes them
	// back without advancing wind, lightning, rain, or the mission clock.
	void resync_colors_now(EnvironmentState *env);

	// The witnessed wire-unit packing (q16 = round(x*65536),
	// cloud_scroll_rate_target = sky_speed << 10, fixed24 TOD). Returns false
	// when no loaded environment backs the snapshot.
	bool network_snapshot(const EnvironmentState *env,
			NetEnvSnapshot &out) const;

	// Project one received phase-2 sample into both owners: the environment
	// takes TOD/fog/cloud metadata; the core reconstructs the exact scalar
	// targets and fog acceleration units while retaining the local smoothed
	// currents. Publishes immediately (zero-tick) even when this render frame
	// contains no 62 Hz quantum.
	void apply_network_sample(EnvironmentState *env, const NetEnvSample &sample);

	void trigger_lightning_short() { core_.lightning.trigger_short(); }
	void trigger_lightning_long() { core_.lightning.trigger_long(); }

	// The OpenNova authoring extension's unit conversion (6*seconds ticks;
	// arming auto-installs intensity 256 on a becalmed oscillator).
	void set_wind_duration_seconds(int seconds);
	int wind_duration_seconds() const;

	// The marched iris-exposure samples (D-RLIT-2): the in-world shell stamps
	// per-sample classification codes each frame; empty keeps the outdoor
	// fallback sample (editor previews with no world)
	// [orig: compute_ambient_light_along_direction @ 0x5c7a00].
	void set_iris_samples(const int32_t *samples, int count);
	void clear_iris_samples() { iris_samples_.clear(); }

	float sway_amount() const;
	float sway_phase() const;
	float lightning_intensity() const;

	Rgb smooth_fill() const;
	Rgb smooth_sun() const;
	// Fog color blocks operate in authored half-intensity bytes; the derived
	// render color doubles only after smoothing, lightning and modulation.
	Rgb smooth_fog() const;
	Rgb smooth_sky() const;
	// Retail overwrites skyfog[0] with the horizon blend in undoubled space,
	// then applies the same saturating x2 as fog.
	Rgb smooth_skyfog() const;
	Rgb smooth_ceiling() const;
	Rgb smooth_cloud() const;
	Rgb smooth_floor() const;
	Rgb smooth_sky_base() const;
	Rgb smooth_sky_bright() const;
	Rgb smooth_sky_highlight() const;
	Rgb smooth_cloud_base() const;
	Rgb smooth_cloud_highlight() const;
	Rgb smooth_cloud_edge() const;

	// The modulator's render color / 64 — ColorSrcGlobalGain
	// [orig: Render_UnpackModulatorToLightScale @ 0x58db30].
	Rgb color_src_gain() const;

	// The witnessed cloud-scroll UV translations for a camera at
	// (cam_x, cam_z) world units [orig: render_skybox @ 0x5791de..0x579260]
	// and the water UV transform sharing the layer-1 accumulators
	// [orig: render_water_surface @ 0x5c3348..0x5c33db].
	CloudUvOffsets cloud_uv_offsets(float cam_x, float cam_z) const;
	float cloud_uv_rate_per_second() const;
	WaterUvState water_uv_state(float cam_x, float cam_z,
			float fog_distance) const;

	// The boxed witnessed state cluster (read-only for consumers).
	const WeatherCore &core() const { return core_; }
	WeatherCore &core() { return core_; }

	// One embedding tick batch (0 = publish-only). Public for the shell's
	// zero-tick refresh paths; the accumulator entries above call it.
	void tick_weather(EnvironmentState *env, int tick_count);

private:
	void reset_for_environment(EnvironmentState *env, bool world_tick_driven);
	void write_weather_state(EnvironmentState &env);

	WeatherCore core_;
	int configured_wind_intensity_ = 256;
	bool colors_synced_ = false;
	// 64-bit like the shell accumulator it replaces: at exactly 1/62 s per
	// frame the credit must land on 1.0, not a float ULP below it.
	double tick_credit_ = 0.0;
	bool world_tick_driven_ = false;
	std::vector<int32_t> iris_samples_;
};

// The shader-global refresh the weather tick publishes each write-back — the
// same value block the standalone owner builds, plus the live sway pair and
// the modulator gain. The deliberate split: fill/sky read back through the
// env's NVG-gated public getters while sun/fog publish the smoother directly.
struct WeatherShaderGlobals {
	EnvShaderGlobals base;
	Rgb color_src_gain;
};

WeatherShaderGlobals build_weather_shader_globals(
		const EnvironmentState &env, const WeatherRuntime &weather);

} // namespace opennova::env
