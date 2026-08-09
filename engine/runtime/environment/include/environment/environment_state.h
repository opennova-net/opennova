// The runtime/editor TOD environment state owner — the state half of the
// witnessed environment cluster, ported verbatim from nova_environment.gd
// (2026-08-09 de-scripting). Engine equivalents (docs/env/env-tod-re.md):
// - [orig: Environment_UpdateWeatherTick @ 0x57e9b0] advances time per tick.
// - [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolates
//   keyframe colors and selects sun-vs-moon light by the hardcoded day-phase
//   windows.
// - [orig: Environment_ApplyFogAndAmbient @ 0x57e440] pushes fog state;
//   fog/skyfog render colors are doubled with saturation (@ 0x57f17c).
// It owns: the one mission TOD clock (8.24 accumulator, env/tod_clock.h), the
// keyframe-TARGET vs smoothed-CURRENT color split shared with the weather
// tick, the S2C 0x0A phase-2 network overrides, the NVG hemisphere rewrite,
// the env #27 smoothed scalar currents, and the change-gated env generation
// every lit consumer keys on. The shell node owns only device work: shader
// global pushes, EnvLightState publication, texture handles, the day_speed
// authoring scrub knob.
#pragma once

#include <env/env.h>
#include <env/tod_clock.h>

#include <cstdint>

namespace opennova::env {

// The world lighting/fog record stamped onto lit materials — the engine
// mirror of the shell's EnvLightValues (ADR 0017 typed record)
// [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0;
//  CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090;
//  ColorSrcGlobalGain bind @ 0x58e05d].
struct WorldLightValues {
	Rgb hemi_sky;
	Vec3 dir{};
	Rgb dir_color;
	Rgb hemi_ground;
	Rgb ceiling;
	Rgb floor_color;
	Rgb gain{1.0f, 1.0f, 1.0f};
	bool fog_enabled = false;
	Rgb fog_color;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	int fog_type = 0;
};

// The opennova_* global shader parameters one refresh pushes (the shell's
// RenderingServer legs consume this as plain values).
struct EnvShaderGlobals {
	Rgb fill_light;
	Rgb sun_light;
	Rgb sky_ambient;
	Vec3 sun_direction{};
	Rgb fog_color;
	float fog_end = 0.0f;
	float fog_start = 0.0f;
	int fog_type = 0;
	float wind_sway_amount = 1.0f;
	float wind_sway_phase = 0.0f;
};

// The env-derived terrain lighting + fog uniforms (terrain.gdshader /
// terrain_editor.gdshader share them via terrain_lighting.gdshaderinc):
// c1 <- the light block, c0 <- the sky block — the witnessed terrain PS
// constants (fill/ground does not reach the terrain surface)
// [orig: terrain_setup_lighting_and_shader @ 0x604420;
//  init_terrain_lighting_color_ramps @ 0x604ee0].
struct TerrainEnvUniforms {
	Rgb sun_light;
	Rgb sky_ambient;
	Vec3 sun_direction{};
	Rgb tile_overlay_tint;
	Rgb fog_color;
	float fog_end = 0.0f;
	float fog_start = 0.0f;
	int fog_type = 0;
};

// One decoded S2C 0x0A phase-2 sample, environment half — the wire view
// reconstructed into native units exactly once here.
struct NetEnvSample {
	int fog_dist = 0;           // u16 world units
	int fog_accel = 0;          // u16 16.16 step clamp (weather core half)
	int rain_pct = 0;           // u8 (weather core half)
	int overcast = 0;           // u8 (weather core half)
	int cloud_scroll = 0;       // u8 sky-speed byte
	int quake_ticks = 0;        // u8 countdown seed
	int tod_fixed = 0;          // u16, <<13 into the 8.24 accumulator
	int precipitation_kind = 0; // u8
};

class EnvironmentState {
public:
	static constexpr int kHoursPerDay = 24;
	// One day in the HHMM time-of-day encoding (0..2400): the wrap modulus
	// every HHMM consumer shares.
	static constexpr double kHhmmDay = 2400.0;
	static constexpr double kMinutesPerHour = 60.0;
	static constexpr double kHhmmHourScale = 100.0;
	static constexpr double kClockMinutesPerDay = kHoursPerDay * kMinutesPerHour;
	static constexpr int kFixed24OneHour = 1 << 24;
	static constexpr int kTodDayFixed24 = kHoursPerDay * kFixed24OneHour;
	static constexpr int kDefaultMinutesPerDay = 1440;
	static constexpr int kDefaultStartHour = 12;

	// Attach/detach the parsed .env document (the live, mission-override-
	// layered view). Clears every remote network override — a replacement .env
	// resets remote state exactly like the shell's document setter did.
	// `loaded` mirrors the document's parse state: a non-null unparsed doc
	// keeps serving its global colors while TOD interpolation stays inert.
	void set_config(const Config *config, bool loaded);
	// Refresh only the parse flag (a document reload/edit event) — network
	// overrides survive, unlike a document replacement.
	void set_loaded(bool loaded) { loaded_ = loaded; }
	const Config *config() const { return config_; }
	bool is_loaded() const { return config_ != nullptr && loaded_; }

	// HHMM setter: wraps into [0, 2400) and recomputes TOD when loaded.
	void set_time_of_day(double hhmm);
	double time_of_day() const { return time_of_day_; }

	// The core witnessed TOD orchestration: sun/moon direction recompute, the
	// 06:00/18:45 sun-vs-moon phase select, keyframe interpolation, the
	// standalone-owner raw color writes vs weather-driven target-only refresh,
	// the fog/skyfog undoubled-blend-then-double derivation, the fog distance
	// reseed, and the generation bump. The shell decides afterwards whether to
	// push shader globals (standalone owners only).
	void update_tod();

	// --- the one runtime mission clock (env/tod_clock.h) ------------------

	// Start the clock from the BMS header. The Q8.8 widening, the day wrap,
	// and retail's 60-minute floor are tod_clock.h's
	// [orig: Game_StartMission @ 0x525371;
	//  Environment_SetTodAdvanceRate @ 0x57d170].
	void configure_mission_clock(int start_time_q8_8, int minutes_per_day);
	int mission_advance_per_tick() const { return mission_advance_per_tick_; }
	static double mission_start_time_hhmm(int start_time_q8_8);

	// Advance by completed logic ticks using the exact native integer
	// increment [orig: Env_TodAdvancePerTick =
	// 0x18000000 / (3720 * minutes_per_day) @ 0x57d108].
	void advance_mission_clock(int ticks);

	// The debug clock seam (minute-of-day form so linear sliders contain no
	// impossible 12:79 values). Returns false on a non-finite/out-of-range
	// input. Updating the fixed-point accumulator is essential: merely
	// assigning the HHMM view would be overwritten by the next world-driven
	// weather tick.
	bool debug_set_mission_minute_of_day(double minute_of_day);
	double mission_minute_of_day() const;

	// Exact retail-native mission clock used by the 0x0A phase-2 projection.
	// Consumers must not reconstruct this value from the display clock.
	int mission_time_fixed24() const { return mission_time_fixed24_; }

	// --- network phase-2 overrides ---------------------------------------

	// Apply one decoded S2C 0x0A phase-2 sample: only the wire-carried values
	// override; the local .env remains the source for colors, fog type, sky
	// height, and every other unreplicated channel. Retail writes the received
	// rain/overcast bytes into TARGET globals; the weather core retains the
	// local currents and writes their 62 Hz chase back through
	// set_smoothed_scalars().
	void apply_network_sample(const NetEnvSample &sample);
	void clear_network_state();
	bool network_active() const { return network_environment_active_; }
	int network_quake_ticks() const;
	float network_rain_current() const;
	float overcast_blend() const;
	int network_precipitation_kind() const;

	// --- HHMM conversion statics ------------------------------------------

	static double minute_of_day_to_hhmm(double minute_of_day);
	static double hhmm_to_minute_of_day(double hhmm);
	static double fixed24_to_hhmm(int value);
	static double hours_to_hhmm(double hours);

	// --- the weather-driven split -----------------------------------------

	// TRUE once a weather tick drives this environment: the tick then OWNS the
	// current render colors + the smoothed fog distance + the shader globals
	// (its per-frame writeback), and update_tod refreshes only the keyframe
	// TARGETS — the witnessed split [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57de40 refreshes target slots; the smoothers own the currents].
	// Without this split the mission clock's per-tick TOD writes alternate RAW
	// keyframe colors against the weather's modulated writeback — the whole
	// scene then strobes between the two at the tick/frame beat (the
	// 2026-07-13 "black flicker" regression). Standalone owners (the editor
	// env preview without a weather node) keep the direct writes.
	void set_weather_driven(bool driven) { weather_driven_ = driven; }
	bool is_weather_driven() const { return weather_driven_; }

	// --- NVG --------------------------------------------------------------

	// active is already gated to the view where retail applies NVG lighting
	// (first person). The raw NVG toggle remains simulation-owned. Returns
	// true when the state actually changed (the shell then refreshes only the
	// two affected shader channels).
	bool set_nvg_view(bool active, int gain);
	bool nvg_view_active() const { return nvg_view_active_; }
	int nvg_gain() const { return nvg_gain_; }

	// --- current render colors (the smoothed/current slots) ---------------

	Rgb sun_light() const { return sun_light_; }
	Rgb fill_light() const;  // NVG-gated
	Rgb sky_ambient() const; // NVG-gated; the SMOOTHED sky block when driven
	Rgb fog_color() const { return fog_color_rt_; }
	Rgb skyfog_color() const { return skyfog_color_rt_; }
	// The frame CLEAR color (divergence #21, closed): the POST-BLEND DOUBLED
	// skyfog render color — see get_frame_clear_color in the RE record; the
	// dome pass fogs toward the SAME doubled value so the rim seam is
	// invisible [orig: Environment_UpdateWeatherTick @ 0x57e9b0 blend
	// @ 0x57f037..0x57f0a1, doubling @ 0x57f1b1; consumer
	// Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca7bf; dome fog sub_579CB0;
	// device Clear @ 0x677100; defaults @ 0x57c0b0 / 0x60fca3].
	Rgb frame_clear_color() const { return skyfog_color_rt_; }
	Rgb ceiling_color() const { return ceiling_color_rt_; }
	Rgb cloud_tint() const { return cloud_tint_rt_; }
	Rgb floor_color() const { return floor_color_rt_; }
	Rgb sky_base() const { return sky_base_rt_; }
	Rgb sky_bright() const { return sky_bright_rt_; }
	Rgb sky_highlight() const { return sky_highlight_rt_; }
	// Cloud-pass color blocks (sky dome VS constants c24/c27/c26)
	// [orig: render_skybox uploads @ 0x57934a..0x57936f].
	Rgb cloud_base() const { return cloud_base_rt_; }
	Rgb cloud_highlight() const { return cloud_highlight_rt_; }
	Rgb cloud_edge() const { return cloud_edge_rt_; }
	Rgb color_src_gain() const { return color_src_gain_; }

	// --- keyframe TARGET getters ------------------------------------------
	// Read straight from the interpolated keyframe state, never from the
	// smoothed values written back by the weather tick
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 refreshes every
	// color block's target slot each frame; the smoothers chase keyframe
	// colors, not their own output].

	Rgb fill_light_target() const;
	Rgb sun_light_target() const;
	// The render fog target is the keyframe color doubled, saturating
	// [orig: Environment_UpdateWeatherTick @ 0x57f17c].
	Rgb fog_color_target() const;
	// WeatherColorBlock chases the authored half-intensity color; doubling is
	// the derived tail after smoothing/modulation [orig: @ 0x57f17c].
	Rgb fog_color_base_target() const;
	Rgb sky_ambient_target() const;
	Rgb skyfog_color_target() const;
	Rgb ceiling_color_target() const;
	Rgb cloud_tint_target() const;
	Rgb floor_color_target() const;
	Rgb lightning_color_target() const;
	Rgb sky_base_target() const;
	Rgb sky_bright_target() const;
	Rgb sky_highlight_target() const;
	Rgb cloud_base_target() const;
	Rgb cloud_highlight_target() const;
	Rgb cloud_edge_target() const;

	// --- terrain ----------------------------------------------------------

	Rgb terrain_tint() const;
	// Identity is FAITHFUL for the terrain surface: the terrain texture bake
	// consumer of terrain_rgb is DEAD CODE in retail — the bake buffer is
	// written and freed but its three readers (0x606ce0, 0x606c30,
	// Terrain_GetColorMapBilinear @ 0x606d80) have zero xrefs (full .text
	// E8/E9 scan), so the GPU terrain textures ship untinted
	// (docs/env/env-tod-re.md #19). The live terrain_rgb consumer in this path
	// is the .til tile overlay (tile_overlay_tint below); the re-grilled
	// foliage lightmap constant is its neutral :fd/detail average, not
	// terrain_rgb.
	Rgb terrain_lighting_attenuation() const { return Rgb{1.0f, 1.0f, 1.0f}; }
	// The .til tile-overlay tint: DIFFUSE = HALF(terrain_rgb) on the quad
	// under a TEXTURE x DIFFUSE MODULATE2X combine, folded to one shader
	// multiply — 254/255 at the default tint (witnessed near-identity, one LSB
	// dark). [orig: PolyTrn_SetTerrainTintColors @ 0x605e20;
	//  PolyTrn_RenderTile @ 0x60df0d]
	Rgb tile_overlay_tint() const;

	// --- water / directions / phase ---------------------------------------

	Rgb water_color() const;
	bool has_water_height() const;
	float water_height() const;
	Vec3 sun_direction() const { return sun_dir_; }
	Vec3 moon_direction() const { return moon_dir_; }
	// Sun by day, moon by night
	// [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	Vec3 light_direction() const { return light_dir_; }
	bool is_night_phase() const { return is_night_; }
	// 0..1 ramp toward the current phase across the 20-minute sunrise/sunset
	// windows [orig: Environment_ComputeTimeOfDayColors @ 0x57de99].
	float day_phase_blend() const { return day_phase_blend_; }
	Rgb sun_color() const;
	Rgb moon_color() const;

	// --- the weather per-tick writeback seam (change-gated bumps) ---------

	void set_fill_light(const Rgb &value);
	void set_sun_light(const Rgb &value);
	void set_fog_color_rt(const Rgb &value);
	// The sky block joins the writeback set — entity hemi_sky serves the
	// smoothed+modulated block like fill/sun/fog [orig: Env_SkyBlock[0]
	// consumed by the entity-constants writer @ 0x5c8090].
	void set_sky_ambient_rt(const Rgb &value);
	void set_static_colors_rt(const Rgb &ceiling, const Rgb &cloud,
			const Rgb &floor_color);
	void set_sky_colors_rt(const Rgb &skyfog, const Rgb &sky_base,
			const Rgb &sky_bright, const Rgb &sky_highlight,
			const Rgb &cloud_base, const Rgb &cloud_highlight,
			const Rgb &cloud_edge);
	// The modulator /64 gain (the iris auto-exposure reaching shaders),
	// written back per tick by the weather like the smoothed colors —
	// ColorSrcGlobalGain [orig: Render_UnpackModulatorToLightScale @ 0x58db30;
	//  bind @ 0x58e05d].
	void set_color_src_gain(const Rgb &value);

	// Monotonic generation, bumped only when a lighting/fog value lit
	// consumers read actually changes. Lets a model skip its per-material
	// environment push with one int compare; the shell publishes the typed
	// light record whenever this moves.
	int64_t env_generation() const { return env_generation_; }

	// --- env #27 smoothed scalar currents ---------------------------------

	float fog_distance() const { return fog_distance_; }
	// The SMOOTHED fog distance when the weather tick drives it (env #27):
	// the scrub/keyframe value is the spring TARGET, the served value ramps
	// [orig: Env_FogDistCurrent @ 0x26c681c <- the (d+31)>>5 spring
	// @ 0x57edd7; targets-only snap @ 0x57d1e0]. Every consumer (dome c9,
	// water UV state, object/terrain fog ends, the frame clear) reads through
	// here, so the ramp reaches them all.
	float fog_level() const;
	float fog_level_target() const;
	// Policy lives in env_render [orig: Render_SetFogState @ 0x58a950].
	// Consumes the smoothed CURRENT end so both bounds stay on the same curve
	// during the 62 Hz fog spring.
	float fog_start() const;
	float fog_end_distance() const;
	int fog_type() const;
	float sky_speed() const;
	// The SMOOTHED sky height when the weather tick drives it (env #27):
	// retail eighth-snaps toward the parsed value and rebuilds the dome only
	// as the SMOOTHED height moves [orig: Env_SkyHeightCurrent @ 0x26c6858
	// eighth-snap @ 0x57ee97; the dome rebuild gate @ 0x57e4f4].
	float sky_height() const;
	float sky_height_target() const;
	// env #27: the weather tick pushes the smoothed scalar currents back here
	// (the same writeback seam as the smoothed colors), so every scalar
	// consumer serves the ramped values.
	void set_smoothed_scalars(float fog_distance, float sky_height,
			float sun_dim_pct, float rain_current, float overcast_blend);
	// The smoothed Env_SunDimPct channel (0..100; default 0 — nothing writes
	// the target in stock data) — dims the sun body + glare
	// [orig: @ 0x26c6830 spring @ 0x57ee17; consumers @ 0x5acbc1/0x5acfb8].
	float sun_dim_pct() const { return sun_dim_smoothed_; }

	// --- typed value builders for the shell's device legs -----------------

	// The current world lighting/fog record — the witnessed block mapping:
	// dir_color <- the light block (sun/moon), hemi_sky <- the sky block,
	// hemi_ground <- the ground block, gain <- the modulator /64
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090;
	//  ColorSrcGlobalGain bind @ 0x58e05d;
	//  sun/moon select Environment_GetLightDirectionFloat @ 0x57d870].
	// Returns false when not loaded (the shell publishes its retail-noon
	// defaults record instead); `default_dir` is the fallback for a
	// near-zero light direction.
	bool build_light_values(const Vec3 &default_dir,
			WorldLightValues &out) const;
	// The standalone-owner global refresh (weather absent): wind sway rests at
	// its 1.0/0.0 idle values.
	EnvShaderGlobals build_shader_globals() const;
	TerrainEnvUniforms build_terrain_uniforms() const;

	// Global (non-TOD) colors stay raw on the document so editor/export
	// round-trips do not bake envscale into authored values. The runtime view
	// performs the same byte quantize -> envscale truncation -> saturation as
	// the retail parser [orig: Color_ScaleRGBAndPack @ 0x57f890;
	// divergence #8].
	static float scale_global_channel(float channel, float envscale);
	static Rgb scale_global_color(const Rgb &color, float envscale);

	// Fog/skyfog blocks operate in undoubled authored bytes; render colors
	// double with saturation [orig: @ 0x57f17c]. The skyfog render color is
	// horizon-blended at the retail 1024 reference, then doubled.
	static Rgb double_rgb(const Rgb &value);
	static Rgb derive_skyfog_render_color(const Rgb &fog_raw,
			const Rgb &skyfog_raw, float fog_distance);

private:
	Rgb global_color_target(const Rgb &color) const;
	void bump_env_generation() { ++env_generation_; }

	const Config *config_ = nullptr;
	bool loaded_ = false;

	double time_of_day_ = 1200.0;

	TodState tod_{};
	bool tod_valid_ = false;
	Vec3 sun_dir_{0.0f, 0.70710678f, 0.70710678f};
	Vec3 moon_dir_{};
	Vec3 light_dir_{0.0f, 0.70710678f, 0.70710678f};
	bool is_night_ = false;
	float day_phase_blend_ = 1.0f;
	Rgb fill_light_{0.4f, 0.45f, 0.55f};
	Rgb sky_ambient_rt_{0.3f, 0.4f, 0.6f};
	Rgb sun_light_{0.9f, 0.85f, 0.75f};
	Rgb fog_color_rt_{0.5f, 0.7f, 0.9f};
	Rgb skyfog_color_rt_{0.5f, 0.7f, 0.9f};
	Rgb ceiling_color_rt_{0.5f, 0.5f, 0.5f};
	Rgb cloud_tint_rt_{0.5f, 0.5f, 0.5f};
	Rgb floor_color_rt_{0.5f, 0.5f, 0.5f};
	Rgb sky_base_rt_{0.3f, 0.4f, 0.6f};
	Rgb sky_bright_rt_{0.3f, 0.4f, 0.6f};
	Rgb sky_highlight_rt_{0.5f, 0.5f, 0.5f};
	Rgb cloud_base_rt_{0.3f, 0.4f, 0.6f};
	Rgb cloud_highlight_rt_{0.5f, 0.5f, 0.5f};
	Rgb cloud_edge_rt_{0.3f, 0.4f, 0.6f};
	// ColorSrcGlobalGain — the modulator /64 (iris exposure), weather-written.
	Rgb color_src_gain_{1.0f, 1.0f, 1.0f};
	// The NVG world-lighting rewrite is a view concern, so the shell supplies
	// the already camera-gated (first-person-visible) state. Gain remains in
	// the retail 0..4 range even while the effect is inactive.
	bool nvg_view_active_ = false;
	int nvg_gain_ = 0;
	// Applies the first-person-visible NVG hemisphere rewrite. color_src_gain
	// is the retail modulator byte unpacked as /64, so modulator*f/640 becomes
	// color_src_gain*f/10 here [orig: NVG world-light gain rewrite].
	Rgb apply_nvg_hemi_gain(const Rgb &color) const;

	float fog_distance_ = 1000.0f;
	int mission_time_fixed24_ = kDefaultStartHour * kFixed24OneHour;
	int mission_advance_per_tick_ = tod_advance_per_tick(kDefaultMinutesPerDay);

	// A remote authority's phase-2 state overrides only the values actually
	// carried on the wire.
	bool network_environment_active_ = false;
	float network_fog_target_ = 0.0f;
	float network_sky_speed_ = 0.0f;
	int network_quake_ticks_ = 0;
	float network_rain_current_ = 0.0f;
	float network_overcast_blend_ = 0.0f;
	int network_precipitation_kind_ = 0;

	bool weather_driven_ = false;
	int64_t env_generation_ = 0;

	// env #27 smoothed scalar currents (negative = not driven; parsed
	// fallback).
	float fog_dist_smoothed_ = -1.0f;
	float sky_height_smoothed_ = -1.0f;
	float sun_dim_smoothed_ = 0.0f;
};

} // namespace opennova::env
