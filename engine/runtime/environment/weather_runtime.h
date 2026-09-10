// The weather's render owner — the color half of the witnessed weather tick
// over ONE weather home. world::WeatherState (runtime/world/weather_state.h)
// owns the retail globals and runs the sim legs of
// [orig: Environment_UpdateWeatherTick @ 0x57e9b0] once per 62.5 Hz logic
// tick (the clock, the oscillator, the hit dim, the quake, the lightning
// sequencers, the scalar springs, the cloud-scroll ramp); this runtime runs
// the color legs against the SAME core right after each sim tick (the
// kernel's world::IWeatherRenderTick hook): the TOD target refresh from the
// advanced clock, the iris exposure retarget, modulator-2 -> modulator ->
// every color block ([orig: interpolate_weather_color @ 0x57d9e0]), the
// cloud-scroll accumulators, and the writeback of the smoothed colors into
// env::EnvironmentState. It also owns the standalone embedding (no
// simulation: the editor preview, the GUT fixtures) — a private WeatherState
// ticked from the render delta at the sim's 62.5 Hz, the deterministic
// mission reset epoch, and the 255-tick mission-start settle. The shell node
// owns only device work (node resolution, the hook registration, shader-
// global pushes). RE record: docs/env/env-tod-re.md.
#pragma once

#include <runtime/environment/environment_state.h>
#include <base/io/tick_rate.h>

#include <formats/env/env_water_render.h>
#include <formats/env/env_weather_core.h>
#include <runtime/world/weather_state.h>

#include <cstdint>
#include <vector>

namespace opennova::env {

class WeatherRuntime {
public:
	// state_ may alias idle_state_: a copy would point into its source.
	WeatherRuntime(const WeatherRuntime &) = delete;
	WeatherRuntime &operator=(const WeatherRuntime &) = delete;
	// The weather runs on the simulation clock — ONE clock, 62.5 Hz
	// [orig: Game_ProcessMainFrame @ 0x526774 per drained 16 ms quantum].
	static constexpr int kMaxCatchupTicks = 31;
	// Retail settles the newly initialized environment through 255 complete
	// weather ticks before gameplay/network publication
	// [orig: Environment_MissionStartInit @ 0x57f878..0x57f880].
	static constexpr int kMissionStartPrewarmTicks = 255;

	WeatherRuntime();

	// --- the weather home --------------------------------------------------
	// Attach the World's weather (a mission) or detach back to the env's
	// standalone home (null). The env's bound view follows.
	void attach_state(world::WeatherState *state, EnvironmentState *env);
	world::WeatherState &state() { return *state_; }
	const world::WeatherState &state() const { return *state_; }
	// True while ticking an env's standalone home (no simulation behind it):
	// the runtime then runs the sim legs itself.
	bool standalone() const { return standalone_; }

	// Env_WindScale units: 100% maps to 256 [orig: Environment_InitDefaults
	// @ 0x57c1d1, its only writer]. The oscillator's 15*prev feedback term is
	// stable only for intensity <= 273 — a 0..8192 mapping drove the 32-bit
	// state divergent (docs/env/env-tod-re.md).
	void set_wind_strength_pct(float pct);
	float wind_strength_pct() const;
	// Record the strength the next seed carries without touching the attached
	// home — the embedder's command layer already wrote it there.
	void remember_wind_strength_pct(float pct);

	// --- the standalone embedding ------------------------------------------
	// The autonomous render-delta accumulator for standalone owners: every
	// due 62.5 Hz quantum runs one FULL weather tick (sim legs on the private
	// state, then the color legs). No-ops while world-driven; zero-quantum
	// frames still run a zero-tick publish so scrubs land.
	void process_delta(EnvironmentState *env, double delta);
	// World-driven: the simulation's kernel ticks the weather; this runtime
	// only answers the per-tick render hook. Standalone: process_delta drives.
	void set_world_tick_driven(bool enabled);
	bool world_tick_driven() const { return world_tick_driven_; }
	// Enter world-driven mode and snap every block at the mission's authored
	// T0 before the first tick. Lazy-snapping after T1 would skip retail's
	// first target chase.
	void prepare_world_driven(EnvironmentState *env);
	// Start an independently ticking environment from the same deterministic
	// mission reset epoch used by world-driven play (the private state seeded
	// from the env's parsed config).
	void prepare_autonomous(EnvironmentState *env);
	// The standalone 255-tick settle after the seed [orig:
	// Environment_MissionStartInit @ 0x57f1e0 + @ 0x57f878..0x57f880]. A
	// mission's settle runs in the kernel (MissionKernel::
	// settle_weather_mission_start) through the render hook.
	void prewarm_mission_start(EnvironmentState *env);

	// --- the ticks -----------------------------------------------------------
	// The render legs for the tick the sim just ran (the hook target): the
	// clock -> TOD targets, the iris retarget, the blocks, the cloud-scroll
	// accumulators, the writeback [orig: @ 0x57ef97..0x57f1d1].
	void tick_render(EnvironmentState *env);
	// One complete weather tick: standalone owners run the private state's
	// sim legs then the render legs; a world-driven runtime (the kernel owns
	// the sim legs) runs only the render legs.
	void tick_fixed(EnvironmentState *env);

	// Snap the smoothing state to the env's current targets on the next tick
	// — call after discrete TOD scrubs so the rendered runtime doesn't lag.
	void resync_colors() { colors_synced_ = false; }
	// Discrete runtime scrubs must update the rendered currents even while
	// the mission transport is paused (no world-driven tick runs): a
	// zero-tick refresh snaps the core to the new targets and writes them
	// back without advancing wind, lightning, springs, or the mission clock.
	void resync_colors_now(EnvironmentState *env);

	// The sun-veil exposure stop-down (0..40) — forwarded once per render
	// frame to modulator-2's witnessed target writer
	// (env::ModulatorChain::set_sun_veil_stopdown carries the cites).
	void set_sun_veil_stopdown(int stopdown) {
		state_->core.modulator_chain.set_sun_veil_stopdown(stopdown);
	}

	// The frozen-fixture exposure settle (capture/refresh seam only — live
	// play reaches the same state through the normal per-tick chase): run
	// ONLY the witnessed per-tick exposure legs — the iris retarget
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538], the
	// modulator-chain tick, and every color block's modulate stage
	// [orig: Environment_UpdateWeatherTick block sequence
	// @ 0x57ef97..0x57f03c] — to the chase's fixed point at the current pose,
	// then write back. Wind, lightning, springs, cloud-scroll accumulators,
	// and the mission clock are deliberately untouched.
	void settle_exposure(EnvironmentState *env);

	// The WAC `flash` / `farflash` hooks [orig: Env_TriggerLightningFlashA
	// @ 0x4ed500 / B @ 0x4ed510] on the attached state.
	void trigger_lightning_short() { state_->command_flash(); }
	void trigger_lightning_long() { state_->command_far_flash(); }

	// The OpenNova authoring extension's unit conversion (6*seconds ticks;
	// arming auto-installs intensity 256 on a becalmed oscillator).
	void set_wind_duration_seconds(int seconds);
	int wind_duration_seconds() const;

	// The marched iris-exposure samples (D-RLIT-2): the in-world shell stamps
	// per-sample classification codes each frame; empty keeps the outdoor
	// fallback sample when no world supplies classification codes
	// [orig: compute_ambient_light_along_direction @ 0x5c7a00].
	void set_iris_samples(const int32_t *samples, int count);

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
	// Env_TerrainLightCombined packed 0x00RRGGBB — light x 0xB5/256 + sky,
	// saturating [orig: @ 0x57f0b3..0x57f0d5]; the precipitation drops' color.
	uint32_t terrain_light_combined_rgb() const;

	// The witnessed cloud-scroll UV translations for a camera at
	// (cam_x, cam_z) world units [orig: render_skybox @ 0x5791de..0x579260]
	// and the water UV transform sharing the layer-1 accumulators
	// [orig: render_water_surface @ 0x5c3348..0x5c33db].
	CloudUvOffsets cloud_uv_offsets(float cam_x, float cam_z) const;
	float cloud_uv_rate_per_second() const;
	WaterUvState water_uv_state(float cam_x, float cam_z,
			float fog_distance) const;

	// The boxed witnessed state cluster (read-only for consumers).
	const WeatherCore &core() const { return state_->core; }
	WeatherCore &core() { return state_->core; }

	// One embedding tick batch (0 = publish-only). Public for the shell's
	// zero-tick refresh paths; the accumulator entries above call it.
	void tick_weather(EnvironmentState *env, int tick_count);

private:
	void reset_for_environment(EnvironmentState *env, bool world_tick_driven);
	// The per-tick iris exposure retarget shared by tick_weather and
	// settle_exposure [orig: Environment_ApplyFogAndAmbient
	// @ 0x57e512..0x57e538].
	void feed_exposure_target(EnvironmentState *env);
	void write_weather_state(EnvironmentState &env);
	uint32_t lightning_packed(const EnvironmentState *env) const;

	// Never null: a detached runtime points at its own idle home so the
	// smoothed reads stay valid before an env binds.
	world::WeatherState idle_state_;
	world::WeatherState *state_ = &idle_state_;
	bool standalone_ = true;
	int configured_wind_intensity_ = 256;
	bool colors_synced_ = false;
	// 64-bit like the shell accumulator it replaces: at exactly 1/62.5 s per
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
		const EnvironmentState &env, const WeatherRuntime &weather,
		bool underwater_view = false);

} // namespace opennova::env
