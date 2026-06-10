#pragma once

#include "env/env.h"

#include <cstdint>

// Engine-faithful runtime math for the environment/atmosphere stack, ported
// from Jointops.exe (retail JO:CA). RE record: docs/env/env-tod-re.md.
// This header is the single source of truth for fog policy, day-phase windows,
// weather smoothing, lightning, sun-glare math, and BMS mission overrides;
// the Godot wrappers under godot/engine delegate here.

namespace opennova::env {

// ---------------------------------------------------------------------------
// Fog [orig: Render_SetFogState @ 0x58a950]
//      [orig: CD3DDevice_SetFogParameters @ 0x677960]
//      [orig: Environment_GetFogEndDistance @ 0x57e3e0]

struct FogParams {
	bool enabled = true;
	bool exponential = false; // fog_type 0
	float start = 0.0f;       // world units (linear modes)
	float end = 0.0f;         // world units
	float exp_density = 0.0f; // ln(64) / end when exponential
};

// fog_type 0: exponential, density ln(64)/end. 1: linear from 0.5 world units
// (the caller's constant). 2/3: linear from (1-overcast)*end*{0.5, 0.25}.
// start == end disables fog. `overcast` is the 0..1 weather blend
// (Env_OvercastBlend / 65536); pass 0 while no weather system drives it.
FogParams compute_fog_params(int fog_type, float fog_end_distance, float overcast = 0.0f);

// Above water the live fog end is the smoothed distance scaled by
// (1 - overcast/2) [orig: Environment_GetFogEndDistance @ 0x57e426].
float fog_end_above_water(float fog_distance, float overcast);

// Underwater visibility comes from water murk:
// (1 - 0.992*(1 - (1-m)(2-m)/2)) * 200 world units, murk 0.8 -> ~25.4
// [orig: Environment_GetFogEndDistance @ 0x57e40d].
float fog_end_underwater(float water_murk);

// ---------------------------------------------------------------------------
// Day phase [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]
// Hardcoded windows: sunrise ramps 05:40->06:20 with the sun/moon switch at
// 06:00; sunset ramps 18:25->19:05 with the switch at 18:45 (20-minute ramps).

struct DayPhase {
	bool is_night = false;
	float blend = 1.0f; // 0..1 ramp toward the current phase's light
};

DayPhase compute_day_phase(float hhmm_time);

// ---------------------------------------------------------------------------
// Integer smoothing [orig: Environment_UpdateWeatherTick @ 0x57e9b0]

// (delta + 7) >> 3 step with overshoot snap to the target (sky height, cloud
// scroll rate).
int smooth_eighth(int current, int target);

// (delta + 31) >> 5 step, clamped to +-step_clamp, result clamped to
// +-max_abs (fog distance, overcast blend).
int spring_step(int current, int target, int step_clamp, int max_abs);

// Per-channel 12.20 color smoothing toward a packed 0x00RRGGBB target with
// per-channel max step and +0x80000 rounding on repack
// [orig: interpolate_weather_color @ 0x57d9e0].
struct ColorChannelState {
	int32_t b_fp = 0;
	int32_t g_fp = 0;
	int32_t r_fp = 0;
	int32_t a_fp = 0;

	void snap_to(uint32_t packed);
	// Steps toward `target_packed` and returns the repacked current color.
	uint32_t step(uint32_t target_packed, int max_step_fp);
};

// ---------------------------------------------------------------------------
// Lightning [orig: Environment_UpdateWeatherTick @ 0x57ec6f, 0x57ed0a]
//           [orig: Environment_SetLightningFlash @ 0x57d320]
// Two flash sequencers, indexed by the timer's remaining-tick value. Sequence A
// fires thunder when its timer reaches 0; sequence B fires a second thunder.

struct LightningStep {
	int tick = 0;  // timer value at which the flash level applies
	int level = 0; // 0..255, scales lightning_rgb into the additive slots
};

inline constexpr LightningStep kLightningSequenceA[] = {
	{10, 200}, {6, 255}, {4, 200}, {2, 255}, {0, 0},
};
inline constexpr LightningStep kLightningSequenceB[] = {
	{31, 200}, {28, 150}, {26, 200}, {24, 150}, {23, 100}, {22, 50}, {20, 0},
};

// Returns the flash level for a timer value, or -1 when the tick is not an
// epoch in the sequence.
int lightning_flash_level(const LightningStep *sequence, int count, int tick);

// Flash injection: lightning_rgb scaled by level into per-block additive
// slots — sky >> 8, fog and skyfog >> 9, ground >> 10
// [orig: Environment_SetLightningFlash @ 0x57d320].
struct LightningAdditives {
	Rgb sky;
	Rgb fog;
	Rgb skyfog;
	Rgb ground;
};
LightningAdditives lightning_additives(const Rgb &lightning_rgb, int level);

// ---------------------------------------------------------------------------
// Sun glare [orig: compute_sun_glare_and_fog_blend @ 0x5ad610]
//           [orig: render_skybox_sun_glow @ 0x5acd00]

struct GlareResult {
	int glare = 0;      // 0..255 additive glare intensity
	int fog_whiten = 0; // 0..40 fog-color whitening
};

// view_dot_sun is the cosine between the view direction and the sun; values
// below 0 yield no glare. occlusion_brightness is the hysteresis state 0..255.
GlareResult compute_sun_glare(float view_dot_sun, int occlusion_brightness);

// Occlusion hysteresis: target = 32 * visible_rays (0..8 rays), brightness
// moves +-16 per frame toward it, clamped 0..255.
int glare_brightness_step(int current, int visible_rays);

// ---------------------------------------------------------------------------
// Derived render colors [orig: Environment_UpdateWeatherTick @ 0x57f0b3..0x57f1b1]

// Terrain directional light = light * 0xB5/256 + sky (saturating); 0xB5 = 0.707.
Rgb combine_terrain_light(const Rgb &light, const Rgb &sky);
// Secondary variant with 0x5A (0.352) light weight.
Rgb combine_terrain_light_low(const Rgb &light, const Rgb &sky);
// Lit water color = water * combined_light * 2 (saturating) [>> 7 on bytes].
Rgb lit_water_color(const Rgb &water, const Rgb &combined_light);
// Fog and skyfog render colors are doubled with saturation — .env fog colors
// are authored at half intensity.
Rgb double_saturate(const Rgb &color);

// ---------------------------------------------------------------------------
// Celestial + sky constants
// [orig: render_skybox @ 0x57960e] sun/moon dome position = camera + dir * 2000
// [orig: render_skybox_layers @ 0x5ac230] layer offsets +64/+16, alpha 0x2000,
// additive submit flag 0x110; glare/star models load under mode 0x300000.
inline constexpr float kCelestialDomeDistance = 2000.0f;
inline constexpr float kCelestialLayerOffsetNear = 16.0f;
inline constexpr float kCelestialLayerOffsetFar = 64.0f;
inline constexpr float kCelestialLayerAlpha = float(0x2000) / 65536.0f;

// Cloud UV scroll [orig: render_skybox @ 0x5791de..0x579260 + weather tick]:
// four accumulators advance per tick by rate * {1, 1, 2/3, 4/3}; layer 1 UV =
// (camera + acc) / 2^28, layer 2 UV = (camera + acc) / 2^29.
inline constexpr double kCloudUvScaleLayer1 = 1.0 / 268435456.0; // 2^-28
inline constexpr double kCloudUvScaleLayer2 = 1.0 / 536870912.0; // 2^-29

// The 21x21 sky dome: 441 vertices / 800 triangles per pass
// [orig: render_skybox draw @ 0x5798dc].
inline constexpr int kSkyDomeVertices = 441;
inline constexpr int kSkyDomeTriangles = 800;

// ---------------------------------------------------------------------------
// BMS mission overrides [orig: Game_LoadTerrainDuringConnect @ 0x520710]
//                       [orig: Game_StartMission @ 0x525371..0x525399]

struct BmsEnvOverrides {
	bool has_water_height = false; // attrib bit 0x1
	float water_height = 0.0f;     // file s16, world half-units (engine <<15)
	bool has_fog_level = false;    // attrib bit 0x2
	float fog_level = 0.0f;        // world units
	bool has_fog_color = false;    // attrib bit 0x4
	Rgb fog_color;
	bool has_water_color = false; // any RGB byte nonzero
	Rgb water_color;
	bool has_water_murk = false; // murk byte nonzero
	float water_murk = 0.0f;     // byte * 0.01
	bool has_start_time = false; // local play only
	int start_time = 0;          // HHMM
};

// Applies the override layer onto a parsed Config (the engine mutates its
// globals; we mutate a copy so the base file stays authoritative).
void apply_bms_overrides(Config &config, const BmsEnvOverrides &overrides);

} // namespace opennova::env
