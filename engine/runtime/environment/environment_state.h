// The runtime TOD environment state owner — the state half of the
// witnessed environment cluster.
// Engine equivalents (docs/env/env-tod-re.md):
// - [orig: Environment_UpdateWeatherTick @ 0x57e9b0] advances time per tick.
// - [orig: Environment_ComputeTimeOfDayColors @ 0x57de40] interpolates
//   keyframe colors and selects sun-vs-moon light by the hardcoded day-phase
//   windows.
// - [orig: Environment_ApplyFogAndAmbient @ 0x57e440] pushes fog state;
//   fog/skyfog render colors are doubled with saturation (@ 0x57f17c).
// It owns: the .env keyframe table plus the .trn/overcast.def table the
// overcast blend cross-fades against, the keyframe-TARGET vs smoothed-CURRENT
// color split shared with the weather tick, the NVG hemisphere rewrite, the
// thermal-view overrides (world block, fog, clear, terrain ramps), and the
// change-gated env generation every lit consumer keys on. The mission
// clock, the weather scalars (fog distance, sky height, sun dim, rain,
// overcast, cloud scroll, quake, fog type, precipitation kind) live in ONE
// home, world::WeatherState (runtime/world/weather_state.h); this state reads
// them through a bound view. The shell node owns only device work: shader
// global pushes, EnvLightState publication, and texture handles.
#pragma once

#include <formats/env/env.h>
#include <runtime/world/weather_state.h>

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
};

// The fog state selected for one rendered scene pass. Retail re-applies this
// block for every pass; the underwater branch is derived from water murk and
// Env_WaterColorLit without mutating the authored/current weather state.
// [orig: Environment_ApplyFogAndAmbient @ 0x57e440]
struct SceneFogValues {
	Rgb color;
	float start = 0.0f;
	float end = 0.0f;
	int type = 0;
};

// The env-derived terrain lighting + fog uniforms used by terrain.gdshader:
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

class EnvironmentState {
public:
	// weather_ may alias standalone_weather_: a copy would point into its
	// source.
	EnvironmentState() = default;
	EnvironmentState(const EnvironmentState &) = delete;
	EnvironmentState &operator=(const EnvironmentState &) = delete;
	static constexpr int kHoursPerDay = 24;
	// One day in the HHMM time-of-day encoding (0..2400): the wrap modulus
	// every HHMM consumer shares.
	static constexpr double kHhmmDay = 2400.0;
	static constexpr double kMinutesPerHour = 60.0;
	static constexpr double kHhmmHourScale = 100.0;
	static constexpr double kClockMinutesPerDay = kHoursPerDay * kMinutesPerHour;
	static constexpr int kFixed24OneHour = 1 << 24;
	static constexpr int kTodDayFixed24 = kHoursPerDay * kFixed24OneHour;

	// Attach/detach the parsed .env document (the live, mission-override-
	// layered view). `loaded` mirrors the document's parse state: a non-null
	// unparsed doc keeps serving its global colors while TOD interpolation
	// stays inert.
	void set_config(const Config *config, bool loaded);
	// The overcast table: the .trn + overcast.def keyframes the overcast blend
	// cross-fades the .env colors against (env #16) [orig:
	// Environment_LoadTimeOfDayConfig @ 0x57db30 — the first parse pass into
	// Env_TrnSnapshotTable @ 0x26c7414]. Null = no table (a clear-weather
	// mission renders the .env table alone).
	void set_overcast_config(const Config *overcast);
	// The weather view this state reads its scalars, fog type, precipitation
	// kind, overcast blend and clock through: the World's WeatherState on a
	// mission, else this state's own standalone home (null binds it back).
	// An unseeded home serves the parsed .env values with a static clock.
	void bind_weather(world::WeatherState *weather) {
		weather_ = weather != nullptr ? weather : &standalone_weather_;
	}
	const world::WeatherState *weather() const { return weather_; }
	world::WeatherState *weather() { return weather_; }
	// The standalone weather home (previews, fixtures, an environment with no
	// simulation behind it) — the ONE WeatherState implementation, owned here
	// so a render owner without a World still ticks the retail weather.
	world::WeatherState &standalone_weather() { return standalone_weather_; }
	bool weather_is_standalone() const { return weather_ == &standalone_weather_; }
	// The bound weather is seeded (a mission T0 ran).
	bool weather_live() const { return weather_->valid; }
	// A standalone home no owner seeded yet takes the loaded config's values
	// (a render owner attaching without the embedder's prepare call).
	void ensure_standalone_weather_seeded(int wind_scale);
	// The weather tick's writeback: the render TOD only, never the clock.
	void set_render_time_of_day(double hhmm);
	// Seed the standalone home from the loaded config + the remembered clock
	// (the ONE derivation, env::weather_seed_from_config); the mission's World
	// home is seeded by the embedder at its boundary instead.
	void reset_standalone_weather(int wind_scale = 256);
	// Refresh only the parse flag (a document reload/edit event) — network
	// overrides survive, unlike a document replacement.
	void set_loaded(bool loaded) { loaded_ = loaded; }
	const Config *config() const { return config_; }
	bool is_loaded() const { return config_ != nullptr && loaded_; }
	// True once update_tod found a .env keyframe table — retail's
	// Env_EnvSnapshotCount != 0, the gate on the per-tick keyframe write into
	// the eleven TOD blocks [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57de8a].
	bool has_tod_keyframes() const { return tod_valid_; }

	// HHMM setter: wraps into [0, 2400) and recomputes TOD when loaded.
	void set_time_of_day(double hhmm);
	double time_of_day() const { return time_of_day_; }

	// The core witnessed TOD orchestration: sun/moon direction recompute, the
	// 06:00/18:45 sun-vs-moon phase select, keyframe interpolation of BOTH
	// tables cross-faded by the weather's overcast blend [orig:
	// Environment_ComputeTimeOfDayColors @ 0x57de40 -> Environment_LerpKeyframeSet
	// @ 0x57c3b0 over (env, trn, clamp(Env_OvercastBlend))], the
	// standalone-owner raw color writes vs weather-driven target-only refresh,
	// the fog/skyfog undoubled-blend-then-double derivation, and the
	// generation bump. The shell decides afterwards whether to push shader
	// globals (standalone owners only).
	void update_tod();

	// --- the mission clock (the weather home owns it) -----------------------

	// Remember the BMS clock for the standalone home and (re)seed its clock
	// fields [orig: Game_StartMission @ 0x525371 (start_time << 16);
	// Environment_SetTodAdvanceRate @ 0x57d170, the 60-minute floor]. A
	// bound World home keeps its own (the embedder seeded it).
	void configure_mission_clock(int start_time_q8_8, int minutes_per_day);
	// Advance the STANDALONE home by complete weather ticks (sim legs; the
	// TOD recompute follows); a bound World home advances in its kernel.
	void advance_mission_clock(int ticks);
	// The debug clock seam (minute-of-day form so linear sliders contain no
	// impossible 12:79 values): the TOD command on the bound home
	// [orig: WacCmd_Tod @ 0x4edc70]. False on a non-finite/out-of-range input.
	bool debug_set_mission_minute_of_day(double minute_of_day);
	double mission_minute_of_day() const;
	// The bound home's exact 8.24 clock.
	int mission_time_fixed24() const;

	// --- the weather-home reads (world::WeatherState carries the cites) ----

	// The clock: the bound weather's 8.24 accumulator as HHMM; the render
	// owner pushes it through set_time_of_day each tick.
	void sync_clock_from_weather();
	int quake_ticks() const;
	float rain_current() const;          // Env_RainPctCurrent / 65536
	float overcast_blend() const;        // Env_OvercastBlend / 65536
	int precipitation_kind() const;      // 0 rain, 1 snow
	bool raining() const;                // Env_RainPctCurrent > 48

	// --- HHMM conversion statics ------------------------------------------

	static double hhmm_to_minute_of_day(double hhmm);
	static uint32_t hhmm_to_fixed24(double hhmm);

	// --- the weather-driven split -----------------------------------------

	// TRUE once a weather tick drives this environment: the tick then OWNS the
	// current render colors + the smoothed fog distance + the shader globals
	// (its per-frame writeback), and update_tod refreshes only the keyframe
	// TARGETS — the witnessed split [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57de40 refreshes target slots; the smoothers own the currents].
	// Without this split the mission clock's per-tick TOD writes alternate RAW
	// keyframe colors against the weather's modulated writeback — the whole
	// scene then strobes between the two at the tick/frame beat (the
	// 2026-07-13 "black flicker" regression). Standalone owners without a
	// weather node keep the direct writes.
	void set_weather_driven(bool driven) { weather_driven_ = driven; }
	bool is_weather_driven() const { return weather_driven_; }

	// --- NVG --------------------------------------------------------------

	// active is already gated to the view where retail applies NVG lighting
	// (first person). The raw NVG toggle remains simulation-owned. Returns
	// true when the state actually changed (the shell then refreshes only the
	// two affected shader channels).
	bool set_nvg_view(bool active, int gain);
	int nvg_gain() const { return nvg_gain_; }

	// --- the thermal view -------------------------------------------------

	// The local player's thermal-imaging view, already resolved by the view
	// frame (world::LocalPlayerViewFrame carries the two gates and their
	// witnesses). `world` = the CanFire verdict AND the equipped weapon def's
	// Thermal bit: the flat grey world lighting block, the 0x808080 device
	// fog and frame clear. `terrain` = the def bit in first person alone:
	// the flat terrain ramps. NVG in first person outranks the terrain
	// ramps, while the world block's thermal grey outranks the NVG rewrite.
	// Returns true when the state actually changed.
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c837c..0x5c843c;
	//  Render_ProcessMainSceneFrame @ 0x5ca2da..0x5ca2e3 (the per-frame
	//  latch), @ 0x5ca771..0x5ca778 (clear), @ 0x5ca83c (fog);
	//  Render_TerrainScene @ 0x610d10..0x610ea1]
	bool set_thermal_view(bool world, bool terrain);
	bool thermal_view() const { return thermal_view_; }
	bool thermal_terrain_view() const { return thermal_terrain_view_; }

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
	// The witnessed per-frame clear SELECTION [orig:
	// Render_ProcessMainSceneFrame @ 0x5ca771..0x5ca792 + the indoors gate
	// @ 0x5c1597]: BLACK while the blink indoors bit is set (the
	// Env_SkyfogBlock clear runs only when it is clear), the flat 0x808080
	// grey while the thermal view is latched (it outranks the water test
	// [orig: @ 0x5ca771..0x5ca778]), the horizon-blended skyfog above water,
	// and underwater the lit water color — water x combined terrain light,
	// the same derived chain the water surface renders with [orig:
	// @ 0x5ca78b]. Every branch serves RENDER-SPACE (x2-gained) colors for
	// the modulate2x-path device Clear (D-RMAT-7).
	Rgb frame_clear_color_for(bool indoors, bool above_water) const;
	// The interior pair, NVG-gated like sky/ground but with the modulator's
	// R term on all three channels (apply_nvg_hemi_gain_r).
	Rgb ceiling_color() const;
	Rgb cloud_tint() const { return cloud_tint_rt_; }
	Rgb floor_color() const;
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
	// The packed Env_TerrainColorRecip the loaded terrain_rgb parses to (the
	// 0x808080 boot default without a config) — the terrain light pass's
	// per-channel factor source, bytes x 1/128 at the consumer.
	// [orig: TimeOfDay_ParseProperty @ 0x57ca60..0x57cae3;
	//  Environment_InitDefaults @ 0x57c065; EffectWorld_TickInstancesAndLightScale
	//  @ 0x5aa21d..0x5aa23f]
	uint32_t terrain_color_recip_packed() const;

	// --- water / directions / phase ---------------------------------------

	Rgb water_color() const;
	bool has_water_height() const;
	float water_height() const;
	Vec3 sun_direction() const { return sun_dir_; }
	Vec3 moon_direction() const { return moon_dir_; }
	// Sun by day, moon by night
	// [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	Vec3 light_direction() const { return is_night_phase() ? moon_dir_ : sun_dir_; }
	bool is_night_phase() const { return weather_->is_night_phase(); }
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

	// --- the smoothed scalar currents (env #27; the weather home's springs) --

	// The SMOOTHED fog distance when a live weather drives it (env #27):
	// the authored value is the spring TARGET, the served value ramps
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
	// The authored murk, or retail's fresh-install default 0.8 when no .env
	// config is loaded — the ONE source for every murk consumer
	// [orig: the default env register block, water murk 0.8; clamp @ 0x57CBA9].
	float water_murk() const;
	float sky_speed() const;
	// The SMOOTHED sky height when the weather tick drives it (env #27):
	// retail eighth-snaps toward the parsed value and rebuilds the dome only
	// as the SMOOTHED height moves [orig: Env_SkyHeightCurrent @ 0x26c6858
	// eighth-snap @ 0x57ee97; the dome rebuild gate @ 0x57e4f4].
	float sky_height() const;
	float sky_height_target() const;
	// The smoothed Env_SunDimPct channel (0..100; the `sunfade` WAC writes its
	// target, the unwritten max clamp pins the current at 0 in retail) — dims
	// the sun body + glare [orig: @ 0x26c6830 spring @ 0x57ee17; consumers
	// @ 0x5acbc1/0x5acfb8].
	float sun_dim_pct() const;

	// --- typed value builders for the shell's device legs -----------------

	// The current world lighting/fog record — the witnessed block mapping:
	// dir_color <- the light block (sun/moon), hemi_sky <- the sky block,
	// hemi_ground <- the ground block, gain <- the modulator /64; the NVG
	// rewrite rides the getters and the thermal grey override the tail
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090
	//  (thermal grey @ 0x5c837c..0x5c843c); ColorSrcGlobalGain bind
	//  @ 0x58e05d; sun/moon select Environment_GetLightDirectionFloat
	//  @ 0x57d870].
	// Returns false when not loaded (the shell publishes its retail-noon
	// defaults record instead); `default_dir` is the fallback for a
	// near-zero light direction.
	bool build_light_values(const Vec3 &default_dir,
			WorldLightValues &out, bool underwater_view = false) const;
	// The standalone-owner global refresh (weather absent): wind sway rests at
	// its 1.0/0.0 idle values. Its sun/sky pair is the terrain pair below
	// (foliage inherits the terrain's two device constants).
	EnvShaderGlobals build_shader_globals(bool underwater_view = false) const;
	// The terrain c1 light / c0 sky pair plus the pass fog; the thermal
	// terrain ramps and the NVG sky blend select here
	// [orig: Render_TerrainScene @ 0x610d10..0x610ea1].
	TerrainEnvUniforms build_terrain_uniforms(bool underwater_view = false) const;
	// The per-pass device fog: lit water underwater, else the thermal
	// 0x808080, else the weather fog block
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e471..0x57e4ad].
	SceneFogValues build_scene_fog(bool underwater_view) const;

	// The one witnessed render-eye/waterline rule. The device fog selector is
	// STRICT below [orig: is_underwater = view_z < waterline, the
	// Environment_ApplyFogAndAmbient selector feed @ 0x57E44C], while the
	// murk scissor's cmp/jg gate INCLUDES exact equality [orig: the scissor
	// draw gate @ 0x5C96C5..0x5C96FA — jg skips only strictly-above]. Both
	// decisions must come from one sampled eye height.
	struct RenderEyeClassification {
		bool underwater_view = false;
		bool underwater_overlay_view = false;
	};
	static RenderEyeClassification classify_render_eye(float eye_y,
			float water_height, bool water_active) {
		if (!water_active) return {};
		return {eye_y < water_height, eye_y <= water_height};
	}

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
	const Config *overcast_config_ = nullptr;
	world::WeatherState standalone_weather_;
	world::WeatherState *weather_ = &standalone_weather_;
	int clock_start_q8_8_ = 12 << 8;
	int clock_minutes_per_day_ = 1440;
	bool clock_configured_ = false;
	bool loaded_ = false;

	double time_of_day_ = 1200.0;

	TodState tod_{};
	bool tod_valid_ = false;
	// The unseeded default light direction: 45 degrees up, in the render-float
	// basis (a unit vector, not a weight; sqrt(2)/2 by construction).
	static constexpr Vec3 kDefaultSunDirRender{0.0f, 0.70710678f, 0.70710678f};
	Vec3 sun_dir_ = kDefaultSunDirRender;
	Vec3 moon_dir_{};
	Vec3 light_dir_ = kDefaultSunDirRender;
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
	// color_src_gain*f/10 here [orig: NVG world-light gain rewrite
	// @ 0x5c8205..0x5c82a1].
	Rgb apply_nvg_hemi_gain(const Rgb &color) const;
	// The ceiling/floor form: the modulator's R term on all three channels
	// [orig: @ 0x5c82a5..0x5c82e9].
	Rgb apply_nvg_hemi_gain_r(const Rgb &color) const;
	// The thermal view's two shell-fed gates (see set_thermal_view).
	bool thermal_view_ = false;
	bool thermal_terrain_view_ = false;

	bool weather_driven_ = false;
	int64_t env_generation_ = 0;

};

} // namespace opennova::env
