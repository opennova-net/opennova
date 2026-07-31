#pragma once

#include "env/env.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Weather-side render state math: fog policy, the day-phase selector, the
// fixed-point smoothing/oscillator/lightning/rain machinery behind
// Environment_UpdateWeatherTick, the weather color blocks + modulator chain,
// cloud scroll, derived render colors, iris auto-exposure, and terrain tint.
// Split from the retired env_render.h umbrella (W3-7).

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

// The weather tick's smoothed scalar channels — the scalar tail that runs
// between the lightning sequencers and the 16 color-block smoothers
// [orig: Environment_UpdateWeatherTick @ 0x57edd7..0x57ef92]: fog distance
// (spring, {cur, target, parsed, step, max} @ 0x26c681c..), sun-dim percent
// (spring @ 0x26c6830.. — dims the sun body + glare; default 0, no .env
// keyword), sky height (eighth-snap @ 0x26c6858/5c), rain percent (spring
// @ 0x26c6880.. Env_RainPct*, REN-6-identified; >48 drives weather-particle
// fall decay), overcast blend (spring @ 0x26c6894..). The camera FOV
// eighth-snap (@ 0x26c6844) rides the camera system, not this struct. All
// values are 16.16 fixed (percent channels are 16.16 percent: 0x640000 =
// 100). The per-channel step/max clamps are COMMAND-driven (the weather
// command setter @ 0x57f1e0 computes |target-cur|/ticks; net apply 0x00A) —
// they default effectively-unclamped here and tighten when the weather
// command wiring lands. The mission-start snap refreshes TARGETS ONLY
// [orig: Environment_SnapStateToTargets @ 0x57d1e0] — the smoothed currents
// always RAMP in from their previous values, exactly like the cloud-scroll
// rate.
struct EnvScalarChannels {
	static constexpr int32_t kUnclamped = 0x40000000;

	int32_t fog_dist_fp = 1024 << 16; // [orig defaults: Environment_InitDefaults @ 0x57c010]
	int32_t fog_dist_target_fp = 1024 << 16;
	int32_t fog_step_fp = kUnclamped;
	int32_t fog_max_fp = kUnclamped;

	int32_t sun_dim_fp = 0;
	int32_t sun_dim_target_fp = 0;
	int32_t sun_dim_step_fp = kUnclamped;
	int32_t sun_dim_max_fp = kUnclamped;

	int32_t sky_height_fp = 175 << 16; // the authoring default (the raw-200 boot quirk is divergence #12)
	int32_t sky_height_target_fp = 175 << 16;

	int32_t rain_pct_fp = 0;
	int32_t rain_pct_target_fp = 0;
	int32_t rain_step_fp = kUnclamped;
	int32_t rain_max_fp = kUnclamped;

	int32_t overcast_fp = 0;
	int32_t overcast_target_fp = 0;
	int32_t overcast_step_fp = kUnclamped;
	int32_t overcast_max_fp = kUnclamped;

	// One 62 Hz step of every channel, in the witnessed in-tick order
	// (fog -> sun-dim -> [FOV: camera-side] -> sky height -> [cloud scroll:
	// CloudScrollState] -> rain -> overcast).
	void tick();
};

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

// Packed-byte form of the same computation, exact to the MMX sequence
// (pmullw then psrlw per slot; the directional-light slot is explicitly
// ZEROED — lightning never brightens the sun) [orig: Environment_SetLightningFlash
// @ 0x57d320]. lightning_packed is 0x00RRGGBB.
struct LightningAdditivesPacked {
	uint32_t sky = 0;    // >> 8
	uint32_t fog = 0;    // >> 9
	uint32_t skyfog = 0; // >> 9
	uint32_t ground = 0; // >> 10
};
LightningAdditivesPacked lightning_additives_packed(uint32_t lightning_packed, int level);

// The two flash sequencer timers. tick() decrements active timers and looks
// the remaining value up in the epoch tables; when an epoch fires, the level
// is SET (never max-combined — each Environment_SetLightningFlash call
// overwrites the additive slots). Epoch 0 also fires thunder SoundBank
// triggers in retail (id 0 / id 0x80 via @ 0x527b90) — deferred to WAC
// weather (env #15). Trigger writes: the short sequence arms timer A at 16,
// the long arms timer B at 32 (both sequences' largest epoch + 1 tick).
// [orig: Environment_UpdateWeatherTick @ 0x57ec6f (A) / @ 0x57ed0a (B);
//  reset Environment_SnapStateToTargets @ 0x57d1e0]
struct LightningSequencers {
	int timer_a = 0; // Env_LightningTimerA
	int timer_b = 0; // Env_LightningTimerB
	int level = 0;   // last SET flash level, 0..255

	void trigger_short() { timer_a = 16; }
	void trigger_long() { timer_b = 32; }
	// One 62 Hz tick; returns true when an epoch (re)set the level this tick.
	bool tick();
};

// ---------------------------------------------------------------------------
// Weather oscillator — the wind-sway / wave PRNG state
// [orig: Environment_UpdateWeatherTick @ 0x57e9b0: PRNG rol-9 + signed-carry
//  step @ 0x57e9fc..0x57ea16, amplitude + 256-entry rings + spring smoothing
//  @ 0x57ea42..0x57eaed; seed 0x12333333 at mission start (the mov imm32 at
//  @ 0x57d2ff — 0x12345633 was a reimpl transcription error, env #25)
//  Environment_SnapStateToTargets @ 0x57d1e0; Env_WindScale default 256
//  Environment_InitDefaults @ 0x57c1d1. The quake path re-rolls the PRNG per
//  displaced entity (@ 0x57eb8e) — reroll() is that step.]
//
// The carry idiom is SIGNED: ((int32)rotated >> 31) & 0x1ABB09 adds 0x1ABB09
// when bit 31 is set (x86 cdq/and/add). A logical-shift port adds 0 or 1 and
// silently forks the sequence from the first negative rotate — the GDScript
// port carried exactly that bug until this port (docs/env/env-tod-re.md).
struct WeatherOscillator {
	uint32_t prng = 0x12333333u; // Env_WeatherPrng [orig: seed imm32 @ 0x57d2ff]
	int intensity = 256;         // Env_WindScale [orig: default @ 0x57c1d1]
	int prev_noise = 0;          // dword_26C7764
	int pos = 0;                 // dword_26C7758 (spring position, 0x8000 rest)
	int smoothed = 0;            // dword_26C775C (clamped 0..0xFFFF)
	int velocity = 0;            // dword_26C7760
	uint8_t ring_index = 0;      // Env_WaveRingIndex
	int32_t amp_ring[256] = {};  // Env_WaveAmpRing (0xFFFF - 2*amp, floor 0)
	int32_t osc_ring[256] = {};  // Env_WaveOscRing (smoothed history)

	// Advances the PRNG one step and returns the new word.
	uint32_t reroll();
	// One 62 Hz oscillator tick; returns the tick's scaled amplitude.
	int tick();
};

// ---------------------------------------------------------------------------
// Rain fade [orig: Environment_UpdateWeatherTick @ 0x57eaf9 (decay);
//            factor consumed per color block in interpolate_weather_color
//            @ 0x57d9e0; reset @ 0x57d1e0]

struct RainState {
	int intensity = 0; // Env_RainIntensity (0..0x8000 attenuates fully)
	int fade_rate = 0; // Env_RainFadeRate

	void tick() {
		intensity -= fade_rate;
		if (intensity < 0) {
			intensity = 0;
		}
	}
};

// (0x8000 - intensity), zeroed when intensity exceeds 0x8000 unsigned —
// the per-block modulator blend factor [orig: interpolate_weather_color
// @ 0x57d9e0].
int rain_blend_factor(int rain_intensity);

// ---------------------------------------------------------------------------
// Weather color block — the full per-block pipeline of
// [orig: interpolate_weather_color @ 0x57d9e0] (16 such blocks tick per
// frame @ 0x57ef9c..0x57f032): 12.20 step toward the packed target under
// PER-CHANNEL max rates, saturating add of the lightning additive slot
// (paddusb), then the modulator x rain blend
//   out_c = min(255, ((c * m) >> 1) * (rain_factor >> 4) >> 16)
// (pmullw / psrlw 1 / pmulhw / packuswb). The modulator chain is the iris
// auto-exposure consumer (env #17): most blocks modulate against the
// modulator block, the modulator against modulator-2, modulator-2 against
// the constant identity bytes. Identity modulator byte = 64.

inline constexpr uint32_t kModulatorIdentityPacked = 0x40404040u;

struct WeatherColorBlock {
	uint32_t render_color = 0;  // [0] post-modulation packed (the render read)
	uint32_t pre_mod_color = 0; // [1] step + additive, pre modulation
	ColorChannelState channels; // [2..5] the 12.20 accumulators
	// [6..9] per-channel max step rates (b, g, r, a) in 12.20; the parse-time
	// default is effectively unclamped (255 << 20).
	int32_t max_rate[4] = {0x0FF00000, 0x0FF00000, 0x0FF00000, 0x0FF00000};
	uint32_t target = 0;   // [11] packed target the step chases
	uint32_t additive = 0; // [12] the lightning flash slot

	// Snaps the accumulators and both packed colors to `packed`.
	void snap(uint32_t packed);
	// One 62 Hz tick [orig: interpolate_weather_color @ 0x57d9e0].
	void tick(uint32_t modulator_packed, int rain_intensity);
	// Sets the per-channel max step rates so the current accumulators reach
	// `target` in `frames` ticks (rounded division; frames 0 clamps to 1)
	// [orig: ColorBlock_SetStepDeltas @ 0x57d940].
	void set_step_deltas(int frames);
};

// The ten weather color blocks beyond the world-lighting quartet: skyfog, the
// three static ceiling/cloud/floor blocks, then the six sky/cloud TOD ramps.
// Their world-driven consumers span the sky pass, frame clear, and indoor iris.
// The three explicit tick methods preserve the witnessed grouping and order
// while every block reads the same fresh modulator.
// [orig: Environment_UpdateWeatherTick @ 0x57efc9..0x57f03c]
struct SkyWeatherColorBlocks {
	static constexpr std::size_t kCount = 10;

	WeatherColorBlock skyfog;
	WeatherColorBlock ceiling;
	WeatherColorBlock cloud;
	WeatherColorBlock floor;
	WeatherColorBlock skybase;
	WeatherColorBlock skybright;
	WeatherColorBlock skyhighlight;
	WeatherColorBlock cloudbase;
	WeatherColorBlock cloudhighlight;
	WeatherColorBlock cloudedge;

	void snap(const std::array<uint32_t, kCount> &packed_colors);
	void set_targets(const std::array<uint32_t, kCount> &packed_colors);
	void set_skyfog_additive(uint32_t packed_additive);
	void tick_skyfog(uint32_t modulator_packed, int rain_intensity);
	void tick_statics(uint32_t modulator_packed, int rain_intensity);
	void tick_dome(uint32_t modulator_packed, int rain_intensity);
};

// ---------------------------------------------------------------------------
// The modulator chain (env #17 — the iris auto-exposure consumer, ported at
// REN-5). Two dedicated blocks head the weather tick: modulator-2 modulates
// against the constant identity bytes, the modulator against modulator-2, and
// every color block against the modulator — the witnessed call order is
// modulator2, modulator, then the color blocks (same-tick propagation)
// [orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..0x57f03c —
//  0x26c6678 modulator2, 0x26c6644 modulator, then light/sky/ground/fog/...].
//
// The modulator's target is the player's iris auto-exposure sample replicated
// to gray (0x10101 * gain) and chased over 62 ticks (1 s)
// [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538 —
//  compute_ambient_light_along_direction @ 0x5c7a00 averages the iris gain at
//  3 points marched from the camera-ray hit back toward the camera; the pure
//  outdoor sample is iris_gain() below]. Consumers: every block's [0] render
//  color (the multiply in WeatherColorBlock::tick), and the /64 render scales
//  (ColorSrcGlobalGain + the effects/foliage/point-light ambient scale —
//  renderer::unpack_modulator_scale, [orig: @ 0x58db30; @ 0x5aaef0]).

struct ModulatorChain {
	WeatherColorBlock modulator;  // 0x26c6644
	WeatherColorBlock modulator2; // 0x26c6678

	ModulatorChain() {
		modulator.snap(kModulatorIdentityPacked);
		modulator2.snap(kModulatorIdentityPacked);
	}

	// Set the exposure target from an iris gain (0..255, 64 = identity):
	// target = 0x10101 * gain, chased over 62 ticks
	// [orig: @ 0x57e512..0x57e538].
	void set_exposure_target(int gain) {
		modulator.target = 0x10101u * static_cast<uint32_t>(gain < 0 ? 0 : (gain > 255 ? 255 : gain));
		modulator.set_step_deltas(62);
	}

	// One 62 Hz tick, ahead of the color blocks; the color blocks then tick
	// with `render_color()` [orig: block order @ 0x57ef97..].
	void tick(int rain_intensity) {
		modulator2.tick(kModulatorIdentityPacked, rain_intensity);
		modulator.tick(modulator2.render_color, rain_intensity);
	}

	uint32_t render_color() const { return modulator.render_color; }
};

// ---------------------------------------------------------------------------
// Cloud scroll accumulators
// [orig: Environment_UpdateWeatherTick — rate smoothing toward
//  Env_SkySpeedFixed (sky_speed << 10) @ 0x57eecc (the mission-start SNAP
//  @ 0x57d2da refreshes only the TARGET — the rate always ramps); the four
//  accumulators advance {1, 1, 2/3, 4/3} x rate with TRUNCATING integer /3
//  @ 0x57f1a5..0x57f1d1. render_skybox consumes them as texture-transform
//  translations (@ 0x5791de..0x579260): layer 1
//  U = -(camY_eng + acc_26C6810) * 2^-28, V = +(camX_eng + acc_26C680C) * 2^-28;
//  layer 2 U = -(camY + acc_26C6818[4/3]) * 2^-29, V = +(camX + acc_26C6814[2/3])
//  * 2^-29. In the render basis (Math_FixedPointToFloat3_YNegated @ 0x611210:
//  d3d = (-engY, engZ, engX)/65536) that is U = +camX_render/4096 - acc*2^-28
//  and V = +camZ_render/4096 + acc*2^-28 — the accumulator term is NEGATIVE
//  on U.]

struct CloudScrollState {
	int rate = 0;         // Env_CloudScrollRate (ramps toward the target)
	int32_t acc_l1_v = 0; // dword_26C680C (render V axis, layer 1)
	int32_t acc_l1_u = 0; // dword_26C6810 (render U axis, layer 1, negated)
	int32_t acc_l2_v = 0; // dword_26C6814 (rate - rate/3, render V axis)
	int32_t acc_l2_u = 0; // dword_26C6818 (rate + rate/3, render U axis, negated)

	// One 62 Hz tick; rate_target = sky_speed << 10.
	void tick(int rate_target);
};

// The final per-layer UV translations for a camera at (cam_x, cam_z) render/
// world units [orig: render_skybox @ 0x5791de..0x579260 — see the axis map
// above; 2^-12 = the 16.16 camera fixed value / 2^28].
struct CloudUvOffsets {
	float u1 = 0.0f, v1 = 0.0f; // layer 1 (UV1, 1/320 world scale)
	float u2 = 0.0f, v2 = 0.0f; // layer 2 (UV2, 3/2048 world scale)
};

CloudUvOffsets cloud_scroll_uv_offsets(const CloudScrollState &scroll,
                                       float cam_x, float cam_z);

// The layer-1 UV drift per second at the current rate — 62 ticks of `rate`
// through the 2^-28 UV scale. The single home of the "sky_speed * 1024 * 62 /
// 2^28" factor the water surface derives its scroll speed from.
float cloud_uv_rate_per_second(const CloudScrollState &scroll);

// Cloud UV scroll [orig: render_skybox @ 0x5791de..0x579260 + weather tick]:
// four accumulators advance per tick by rate * {1, 1, 2/3, 4/3}; layer 1 UV =
// (camera + acc) / 2^28, layer 2 UV = (camera + acc) / 2^29.
inline constexpr double kCloudUvScaleLayer1 = 1.0 / 268435456.0; // 2^-28
inline constexpr double kCloudUvScaleLayer2 = 1.0 / 536870912.0; // 2^-29


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

// Horizon blend (the frame-clear color): when the smoothed fog distance sits
// (unsigned 16.16) strictly below fog_dist_reference/2, the skyfog render
// color is overwritten with fog*(1-t) + skyfog*t, where
// t = ((dist - ref/4) << 16) / (ref/4) clamped at 0 — pure fog color at
// <= ref/4, a linear fade across [ref/4, ref/2], untouched skyfog above.
// Operates on UNDOUBLED colors (retail doubles AFTER the blend), replicating
// the original's per-byte MMX sequence exactly (bytes x0x101 >> 1, pmulhw
// against t/~t >> 1, paddsw, >> 6, packuswb) — including its low-byte loss
// (a 1/255 channel blends to 0 at t=0).
// [orig: Environment_UpdateWeatherTick @ 0x57e9b0, blend @ 0x57f037..0x57f0a1;
//  frame-clear consumer Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca7bf,
//  device Clear @ 0x677100; dome-fog consumer sub_579CB0 @ 0x579cb0]
Rgb horizon_blend_skyfog(const Rgb &fog, const Rgb &skyfog,
                         uint32_t fog_dist_fixed, uint32_t fog_dist_reference_fixed);

// The reference distance's retail default, 1024.0 in 16.16. Terrain init may
// lower it to 768.0 by adapter caps, but the session authority is forced back
// to the default — the reimpl analog for a modern renderer is the default.
// [orig: Environment_InitDefaults @ 0x57c0b0; Terrain_Init @ 0x60fc9a/0x60fca3]
inline constexpr uint32_t kFogDistReferenceDefault = 1024u << 16;

// ---------------------------------------------------------------------------
// Iris auto-exposure [orig: terrain_sector_compute_lighting @ 0x5c7550]
// The engine's global auto-exposure gain (0..255; 64 = identity, 6.6 fixed /
// modulator units). A pure function of the outdoor light blocks + the .env
// iris_center / iris_percent. The modulator CHAIN that applies this gain to the
// color blocks is the runtime consumer (env #17, WITNESSED-READY-DEFERRED);
// this is only the witnessed curve, ready for that chain. Constants:
// lum = 0.25*(r+b) + 0.5*g; base = iris_center*64; sqrt(dx^2+dz^2) horiz weight;
// 0.707 sky+ground horiz term; gain = 0.01*(iris_percent*base/(2m) +
// (100-iris_percent)*base), clamped [0,255].

// Luminance of a color the iris way: 0.25*(r+b) + 0.5*g.
float iris_luminance(const Rgb &c);

// The iris auto-exposure gain. `directional`/`sky`/`ground` are the outdoor
// light blocks (0..1 per channel; indoors substitutes ceiling/floor for
// sky/ground and zeroes directional). `dir_x/y/z` is the direct near-unit
// light getter direction. Returns the modulator gain, int-truncated and clamped [0,255].
int iris_gain(const Rgb &directional, const Rgb &sky, const Rgb &ground,
              float dir_x, float dir_y, float dir_z,
              float iris_center, float iris_percent);

// ---------------------------------------------------------------------------
// Terrain tint (terrain_rgb)
// [orig: PolyTrn_SetTerrainTintColors @ 0x605e20; sole caller Terrain_Init
//  @ 0x60fc42, source Env_TerrainColorPacked @ 0x26c67f4]
// Two globals derived once from the packed terrain color: FULL = c|FF000000
// (@ 0x31a1824), HALF = ((c>>1)&0x7F7F7F)|FF000000 (@ 0x31a1828). Retail has
// three consumers, one of them dead: the 256x256 quarter-res texture-bake
// buffer is written and freed but its three readers (0x606ce0, 0x606c30,
// Terrain_GetColorMapBilinear @ 0x606d80) have ZERO xrefs (full .text E8/E9
// scan) — the GPU terrain textures ship untinted, so an untinted terrain
// surface is the faithful behavior, not a divergence. The two LIVE consumers:
// the .til tile-overlay quad (DIFFUSE = HALF on all four vertices under a
// TEXTURE x DIFFUSE MODULATE2X combine — caps toggle dword_32656AC defaults
// true [orig: PolyTrn_RenderTile @ 0x60df0d -> render_water_quad @ 0x604700])
// and the foliage lightmap sample below.

struct TerrainTint {
	uint32_t full = 0xFFFFFFFFu; // c | 0xFF000000
	uint32_t half = 0xFF7F7F7Fu; // ((c >> 1) & 0x7F7F7F) | 0xFF000000
};

// Splits the two tint globals from a packed 0x00RRGGBB terrain color.
TerrainTint terrain_tint_from_packed(uint32_t terrain_color_packed);

// Packs a parsed terrain_rgb (bytes/255 floats) back to the engine's byte
// color, then splits. The retail default 255,255,255 yields FULL 0xFFFFFFFF /
// HALF 0xFF7F7F7F.
TerrainTint terrain_tint_from_rgb(const Rgb &terrain_rgb);

// Foliage lightmap tint [orig: sample_terrain_colormap_tinted @ 0x606030]: per
// channel min((texel_c * FULL_c) >> 7, 255), alpha passthrough. 128 is
// identity; the default 0xFF tint is a ~2x saturating brighten.
uint32_t foliage_lightmap_tint(uint32_t texel_argb, uint32_t full_tint);

// The tile-overlay combine runs TEXTURE x DIFFUSE(HALF) under MODULATE2X, so
// a single-multiply reimpl shader consumes 2*HALF/255 per channel — 254/255 at
// the default tint (one LSB dark; the witnessed combine is near-identity,
// NOT exact) [orig: PolyTrn_RenderTile @ 0x60df0d, combine pass 0x631].
Rgb tile_overlay_tint_factor(const TerrainTint &tint);


} // namespace opennova::env
