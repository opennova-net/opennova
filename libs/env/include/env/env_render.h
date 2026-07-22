#pragma once

#include "env/env.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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
// Their hosted consumers span the sky pass, frame clear, and indoor iris.
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
int glare_brightness_step(int current, int target);

// ---------------------------------------------------------------------------
// Celestial bodies + glare occlusion — witnessed at the ENG-2 celestial leg
// [orig: render_celestial_bodies @ 0x5acaa0 (the LIVE sun/moon renderer,
//  was misnamed render_skybox_fog_layers); render_skybox_sun_glow @ 0x5acd00;
//  render_star_field @ 0x5ad9c0]. The old render_skybox_layers @ 0x5ac230 is
//  a caller-less dead variant (its +64/+16 fixed offsets never run).

// Sun/moon bodies place at camera + direction * 64 world units, identity
// rotation, FULL camera height [orig: @ 0x5acaa0, constant flt_7C3DD0].
inline constexpr float kCelestialBodyDistance = 64.0f;

// Body alphas, 16.16 in/out like the originals:
// sun = clamp((1 - overcast) * ((0x640000 - sun_dim + 1) / 100)), where
// sun_dim is the 0..100 (16.16) Env_SunDimPct channel (default 0, no .env
// parser writes it) [orig: @ 0x5acbc1..0x5acbfa].
int celestial_sun_alpha_fixed(int overcast_blend_fixed, int sun_dim_fixed);

// ---------------------------------------------------------------------------
// Star field (env #33) [orig: Star_GenerateInstanceTable @ 0x5ac850 (ex kong
// misnomer init_weather_particles); render_star_field @ 0x5ad9c0]. 256
// camera-anchored billboard instances regenerated per celestial load; per
// render tick each visible star's brightness accumulator EMA-chases its
// twinkle band. Engine axes throughout (x, y ground plane, z up), 16.16.

inline constexpr int kStarInstanceCount = 256;

// The star/weather PRNG: state = rol4(state + rol11(state), 4) ^ 1, low 16
// bits returned [orig: inlined at both sites; the dead standalone step is
// Star_TwinklePrngNext_unused @ 0x5ac010, state Star_TwinklePrng @ 0x840B38].
// Like the water noise field, the retail table content depends on the shared
// state's call history at load — a deterministic reimpl documents its seed.
uint32_t star_prng_next(uint32_t &state);

struct StarInstance {
	int32_t offset_fp[3] = { 0, 0, 0 };  // camera-relative offset, 16.16
	int32_t billboard_param = 12288;     // 12288..13311 (4096-fixed scale)
	int32_t twinkle_add = 1;             // 1..255, clamped to 255 - mask
	int32_t twinkle_mask = 31;           // 31 >> (r & 3): {31, 15, 7, 3}
	int32_t brightness = 0;              // runtime accumulator (BSS-zero at generate)
	int32_t dir_fp[3] = { 0, 0, 0 };     // normalize(offset >> 8), 16.16
};

// Fills out[0..255] with the witnessed per-star generation [orig: @ 0x5ac850]:
// offX/offY = (r - 0x8000) << 9; offZ = ((r + 0x20000) << 6) -
// ((|offX| + |offY|) >> 3) (the dome shaping); billboard = (r & 0x3FF) +
// 12288; add = r & 0xFF (0 -> 1, <= 255 - mask); mask = 31 >> (r & 3);
// brightness untouched; dir = normalize(off >> 8) via the 2^32/len + 0x8000
// rounding divide.
void generate_star_instances(StarInstance *out, uint32_t &prng_state);

// One render-tick twinkle update: brightness = (brightness + add +
// (r16 & mask)) >> 1; returns the new brightness [orig: @ 0x5adb45].
int32_t star_twinkle_tick(StarInstance &star, uint32_t &prng_state);

// The near-light cull: HIDDEN when dot(light_dir_fixed, star_dir) > 64225
// (16.16 ~0.98) [orig: @ 0x5adac4]. light_dir is the direct near-unit active
// light direction in engine axes.
bool star_visible_fixed(const StarInstance &star, const int32_t light_dir_fp[3]);
// moon = clamp01((fogDistInt - 400) / 600) * (1 - overcast)  (the no-fog-
// shader path; the fog-shader path scales fogDistInt * 0.0002)
// [orig: @ 0x5acc40..0x5acccd].
int celestial_moon_alpha_fixed(float fog_distance_world, int overcast_blend_fixed,
                               bool fog_shader_path);

// Glare occlusion (env #14) [orig: render_skybox_sun_glow @ 0x5acd9e..0x5acf7f]:
// TWO jittered rays per frame feed an 8-bit SLIDING window (>>1 per sample,
// bit 0x80 = sample visible), so the window spans the last 4 frames. The ray
// is camera -> camera + sun_dir * 1024 world units, jittered per sample from
// the frame index bits: engine-Y +-16 (bit 0) and +-8 (bit 2), height +-16
// (bit 1). Brightness steps +-16 (dead-band hold) toward
// popcount(window) * 32 * (fog_distance / 1000)  (flt_7DA0C4 = 1/65536000).
struct GlareOcclusionState {
	uint8_t window = 0;        // Glare_OcclusionWindow @ 0x27E2E34
	int brightness = 0;        // Glare_OcclusionBrightness @ 0x27E2E30
	uint32_t jitter_index = 0; // Glare_JitterFrameIndex @ 0x27E5690
};

struct GlareRayJitter {
	float offset_eng_y = 0.0f; // engine Y axis (render/Godot -x)
	float offset_eng_z = 0.0f; // engine Z (height, render/Godot +y)
};

// The jitter offsets for one sample index [orig: @ 0x5ace3b..0x5ace61].
GlareRayJitter glare_ray_jitter(uint32_t jitter_index);

// One frame: advances the window with the two samples' visibility and steps
// the brightness. The host casts the two rays (sample indices jitter_index+1
// and jitter_index+2 BEFORE the call).
void glare_occlusion_tick(GlareOcclusionState &state, bool visible_a, bool visible_b,
                          float fog_distance_world);

// The glow submit alpha, 16.16: dot_view^4 / 2 scaled by the occlusion
// brightness (>> 8), the overcast blend and the SunDim fold
// [orig: @ 0x5acfb8..0x5ad0a9].
int glare_glow_alpha_fixed(int view_dot_fixed, int brightness, int overcast_blend_fixed,
                           int sun_dim_fixed);

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
// to the default — the host analog for a modern renderer is the default.
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
// a single-multiply host shader consumes 2*HALF/255 per channel — 254/255 at
// the default tint (one LSB dark; the witnessed combine is near-identity,
// NOT exact) [orig: PolyTrn_RenderTile @ 0x60df0d, combine pass 0x631].
Rgb tile_overlay_tint_factor(const TerrainTint &tint);

// ---------------------------------------------------------------------------
// Celestial + sky constants
// [orig: render_skybox @ 0x57960e] sun/moon dome position = camera + dir * 2000
// [orig: render_skybox_layers @ 0x5ac230] layer offsets +64/+16, alpha 0x2000,
// additive submit flag 0x110; glare/star models load under mode 0x300000.
// NOTE (celestial-leg re-grill 2026-07-06): 2000 is the DOME VS clip-space
// proximity reference distance ONLY [orig: c14 upload @ 0x57960e] — the
// bodies place at kCelestialBodyDistance (64). The +16/+64 offsets and the
// 0x2000 alpha belong to the caller-less dead variant @ 0x5ac230.
inline constexpr float kCelestialDomeDistance = 2000.0f;
inline constexpr float kCelestialLayerOffsetNear = 16.0f;  // dead variant
inline constexpr float kCelestialLayerOffsetFar = 64.0f;   // dead variant
inline constexpr float kCelestialLayerAlpha = float(0x2000) / 65536.0f; // dead variant

// Cloud UV scroll [orig: render_skybox @ 0x5791de..0x579260 + weather tick]:
// four accumulators advance per tick by rate * {1, 1, 2/3, 4/3}; layer 1 UV =
// (camera + acc) / 2^28, layer 2 UV = (camera + acc) / 2^29.
inline constexpr double kCloudUvScaleLayer1 = 1.0 / 268435456.0; // 2^-28
inline constexpr double kCloudUvScaleLayer2 = 1.0 / 536870912.0; // 2^-29

// The 21x21 sky dome: 441 vertices / 800 triangles per pass
// [orig: render_skybox draw @ 0x5798dc].
inline constexpr int kSkyDomeVertices = 441;
inline constexpr int kSkyDomeTriangles = 800;

// The dome profile is a sphere cap of radius 3072 lowered so the rim
// (radius 1024 = 20 rows x 51.2) sits at y = 0: 3072^2 - 1024^2 = 2^23, so
// y(r) = sqrt(3072^2 - r^2) - sqrt(2^23) and the apex reference height is
// 3072 - sqrt(2^23) ~= 175.6906 — the "175.69" the height scale divides by
// [orig: build_sky_dome_mesh @ 0x578ed4 — v14 = skyHeight / (3072.0 - sqrt(8388608.0))].
inline const double kSkyDomeReferenceHeight = 3072.0 - std::sqrt(8388608.0);

// The sky dome mesh [orig: build_sky_dome_mesh @ 0x578db0] — 21 rings x 21
// columns, FVF 0x212 (XYZ|NORMAL|TEX2, stride 40). Per vertex: radius =
// row * 51.2, theta = col * pi/10, x = sin(theta)*radius, z = cos(theta)*radius,
// y = v14 * (sqrt(3072^2 - radius^2) - sqrt(2^23)) — the Y-only height scale is
// BAKED into the mesh (retail rebuilds on smoothed-height change, gated at
// [orig: Environment_ApplyFogAndAmbient @ 0x57e4f4] -> Terrain_PushSkyDomeHeightFloat
// @ 0x610920 -> SkyDome_SetHeightAndRebuild @ 0x579070; the reimpl folds the
// scale into the vertex shader instead — env #20's ratified structure, so it
// builds once at the reference height). UV1 = (x, z) * 0.003125, UV2 =
// (x, z) * 3/2048. Normal = normalize(x, y_scaled / v14^2, z) — the builder's
// anisotropic normal, NOT the vertex direction [orig: @ 0x578fbb..0x579023];
// zero-length input degenerates to (0,0,0) [orig: @ 0x578fd8]. All math runs
// in double off the binary's float32 literal seeds (51.2f, 0.31415927f,
// 9437184.0f, 0.003125f, 3/2048; 8388608.0 is a double literal — x87
// intermediates approximated as double, stored float32 like the D3D vertex
// buffer). Index winding per quad: (i, i+22, i+21), (i, i+1, i+22)
// [orig: @ 0x578e00..0x578e86].
struct SkyDomeMesh {
	std::vector<float> positions; // xyz triples, kSkyDomeVertices
	std::vector<float> normals;   // xyz triples (anisotropic dome normals)
	std::vector<float> uv1;       // uv pairs, layer 1 (1/320 world scale)
	std::vector<float> uv2;       // uv pairs, layer 2 (3/2048 world scale)
	std::vector<int32_t> indices; // 3 * kSkyDomeTriangles, witnessed winding
};

SkyDomeMesh build_sky_dome_mesh(float sky_height);

// ---------------------------------------------------------------------------
// Water surface — the witnessed pipeline of render_water_surface @ 0x5c32c0
// (the frame pass: FrameFX_RenderBloomPass @ 0x582a5d / @ 0x610650 call it per
// side; camera-side gate against Env_WaterHeightFixed). Per frame it
// regenerates the animated noise texture pair [orig: Water_GenerateNoiseTextures
// @ 0x5c0360], derives the UV scale/bias from the SMOOTHED fog distance and
// the UV offsets from the CLOUD-SCROLL accumulators [orig: @ 0x5c3348..0x5c33db],
// then draws the screen-marched water strips (render_water_strip @ 0x5c1d60
// low detail with sin-table Y displacement; render_water_strip_detailed
// @ 0x5c27d0 high detail, FLAT strips — the animated textures carry the look).
// The strip tessellation itself is a tracked divergence (env #29); this
// section owns the texture + UV math both paths share.

inline constexpr int kWaterNoiseSize = 128; // 128x128 field and textures

// The static tables built once at renderer init [orig:
// Water_InitNoiseFieldAndSineLut @ 0x5c01a0, called from
// Water_InitSurfaceShaders @ 0x5c19b0 (call site @ 0x5c19f8; renamed from the
// kong misnomer Terrain_InitShaders at REN-4 - it builds the WATER surface
// shader/material set)]: a normalized random field and the 128 + 64*sin(2*pi*i/256)
// byte LUT (truncating float->int like the original ftol).
struct WaterNoiseTables {
	uint8_t field[kWaterNoiseSize * kWaterNoiseSize]; // Water_NoiseField
	uint8_t sine_lut[256];                            // Water_SineLut
};

// One step of the init PRNG [orig: PRNG_Next16 @ 0x6130a0]:
// state = rol4(state + rol11(state)) ^ 1; the caller consumes state & 0xFFFF.
uint32_t water_noise_prng_step(uint32_t state);

// Builds the tables with the witnessed algorithm. Retail's field CONTENT
// depends on the shared PRNG's state at Water_InitSurfaceShaders time (a
// value-history quirk, recorded in env-tod-re.md); the reimpl seeds from the
// boot state 0 for a deterministic, witnessed-faithful instance.
WaterNoiseTables water_init_noise_tables();

// The per-frame wave phase [orig: Water_WavePhase = counter * 0x3000000
// @ 0x5c0374; render_water_strip consumes phase + 0x200000, += 0x55555555
// per row, sin table index = value >> 22].
inline constexpr uint32_t kWaterWavePhasePerFrame = 0x3000000u;

// Passes 1+2 of Water_GenerateNoiseTextures: animate the field through the
// LUT (per byte: lut[(uint8)(field + (counter << (field & 1)))] — two speed
// classes), then the toroidal 9-tap kernel (3x corners + 4x cross, >> 5),
// folded to a ridge intensity i = max(0, 128 - |k - 128|) and packed as
// A = 255 - max(0, i*i >> 9), R = G = B = i. out_pixels holds 128*128 ARGB.
void water_noise_color_pixels(uint32_t *out_pixels, const WaterNoiseTables &tables,
                              uint32_t frame_counter);

// Pass 3: the DuDv/normal map [orig: @ 0x5c07c2..0x5c087d, MMX]: per pixel,
// from the color texture's intensity byte c (blue channel):
// R = sat8(2 * satsub8(c - c_up)) + 0x80 (wrapping), G = same against c_left,
// B = 0xFF, A = 0; rows and columns wrap toroidally.
void water_noise_normal_pixels(uint32_t *out_pixels, const uint32_t *color_pixels);

// The shared UV transform state [orig: render_water_surface @ 0x5c3348..0x5c33db]:
// scale = 0.99996948 * w / (w - 0.2) with w = the INTEGER part of the smoothed
// fog distance (the word read at Env_FogDistCurrent+2); bias = 0.2 * scale.
// Offsets ride the LAYER-1 CLOUD accumulators with a 32x camera term:
// u = (uint32)(acc_26C680C + 32 * camX_eng_fixed) * 2^-28,
// v = (uint32)(acc_26C6810 - 32 * camY_eng_fixed) * 2^-28. In the render basis
// (d3d = (-engY, engZ, engX)) that is u = cam_z/128 + acc_l1_v * 2^-28 and
// v = cam_x/128 + acc_l1_u * 2^-28 — both accumulator terms POSITIVE here
// (the water pass's own sign structure, unlike the sky layers).
struct WaterUvState {
	float scale = 1.0f;
	float bias = 0.0f;
	float offset_u = 0.0f;
	float offset_v = 0.0f;
};

WaterUvState water_uv_state(const CloudScrollState &scroll, float cam_x, float cam_z,
                            float fog_distance_world);

// ---------------------------------------------------------------------------
// Water strip tessellation (env #29) — the screen-space row march of the
// DETAILED water surface tier [orig: render_water_strip_detailed @ 0x5c27d0,
// substrate terrain_project_sector_to_screen @ 0x5c0bf0 + clip_line_to_viewport
// @ 0x5c0a30; caller render_water_surface @ 0x5c3492/@ 0x5c3542]. The water
// plane projects to a screen block, rows advance along the screen march
// direction with an adaptive per-row stride, each row's screen line clips to
// the viewport, and the row emits 3 vertices (left / mid / right of the
// clipped span) unprojected through the cached inverted view matrix
// [orig: Math_InvertMatrix4x4_Float_ToStatic @ 0x611960]. The LOW tier
// (render_water_strip @ 0x5c1d60, water detail <= 1: 40-byte verts, sin-table
// Y displacement, the dbl_7DBF70 = 229.5 alpha-scale swap) is the remaining
// unported variant — the host runs the detailed path (detail > 1).
//
// Caller-arg semantics witnessed at the call sites [orig: @ 0x5c3489..0x5c3497
// camera-above (blend material): (height, 0, nightvision); @ 0x5c3539..0x5c3547
// underwater (opaque material): (height, underwater_view, nightvision)]: arg 2
// is the UNDERWATER-VIEW pass (env-tod-re.md's "isReflection" reading — the
// V-flip/murk-skip variant is the underwater view), arg 3 the NIGHTVISION
// redraw.

// The camera/view state the originals read from renderer globals. All floats
// are render-basis (d3d = (-engY, engZ, engX)); matrices are D3D row-major
// row-vector (v' = v * M), so world-space camera basis vectors sit in the
// view matrix COLUMNS.
struct WaterStripView {
	float view[16];     // world->view [orig: viewMatrix @ 0xA7845C]
	float view_inv[16]; // its inverse, cached per pass [orig: @ 0x611960 result]
	// Host projection converted to the render basis/row-vector convention.
	// X/Y clip rows and clip-W are complete: perspective/frustum use depth W,
	// orthographic uses constant W, and the translation/shear terms preserve
	// off-center host projections. The witnessed retail path is the centered
	// perspective subset [orig: mat @ 0x2721980; m11 @ 0x2721994].
	float proj[16];
	// Camera world-basis rows of the render context's camera matrix
	// [orig: flt_27219C0 row 0 (right) / row 2 (forward), Math_CopyVec3Row0/2
	// @ 0x611fb0/@ 0x611f70].
	float cam_right[3] = {1.0f, 0.0f, 0.0f};
	float cam_forward[3] = {0.0f, 0.0f, 1.0f};
	// Camera position, render basis, 16.16 fixed like the camera block the
	// originals fild [orig: 0xA78364 (eng X = render z) / 0xA78368 (eng Y,
	// negated = render x) / 0xA7836C (eng Z = render y)].
	int32_t cam_x_fp = 0;
	int32_t cam_y_fp = 0;
	int32_t cam_z_fp = 0;
	// Viewport rect + center, pixels [orig: 0xA78384/0xA78388 min,
	// 0xA7838C/0xA78390 max, 0xA783A4/0xA783A8 center]. The projection maps
	// x/(2w) across (max - min); the clip rect right/bottom edges are max + 1.
	int32_t vp_min_x = 0;
	int32_t vp_min_y = 0;
	int32_t vp_max_x = 0;
	int32_t vp_max_y = 0;
	int32_t vp_center_x = 0;
	int32_t vp_center_y = 0;
	// The pass fog end distance, 16.16 [orig: Environment_GetFogEndDistance
	// @ 0x57e3e0, called with the underwater flag @ 0x5c28a2].
	int32_t fog_end_fp = 0;
};

// The 40-byte screen block [orig: terrain_project_sector_to_screen @ 0x5c0bf0]:
// the water plane at the strip's height projected at camera +
// horizontal-forward x 2000 -> origin [0..1]; the screen delta of a
// 1000-unit horizontal RIGHT step (view matrix column 0) -> row_delta [2..3]
// (dy forced 1e-6 when 0 [orig: @ 0x5c0deb]); camera + horizontal-forward x
// 1000 -> ref_point [4..5]; march_dir [6..7] = normalize(ref - origin),
// degenerate (0, 1); visible [8] = in-viewport OR row-line-crosses OR the
// halfplane test (the reference point and the viewport center strictly on
// the same side of the row line through the origin — marching in from
// off-screen), with the origin clamped onto the entry edge when only the
// halfplane passes
// [orig: @ 0x5c0fd1..0x5c1023]; origin_row_visible [9] keeps the
// pre-halfplane value.
struct WaterScreenBlock {
	float origin[2] = {0.0f, 0.0f};
	float row_delta[2] = {0.0f, 0.0f};
	float ref_point[2] = {0.0f, 0.0f};
	float march_dir[2] = {0.0f, 0.0f};
	int32_t visible = 0;
	int32_t origin_row_visible = 0;
};

void water_project_plane_to_screen(const WaterStripView &view, int32_t plane_height_fp,
                                   WaterScreenBlock &out);

// One row's screen line clipped to the viewport rect
// [orig: clip_line_to_viewport @ 0x5c0a30]. The line passes through (x0, y0)
// with slope dx_over_dy (the block's row_delta ratio [orig: @ 0x5c291e]);
// endpoints seed at x = min_x and x = max_x + 1 through the 1/slope form,
// then clamp against y = min_y / max_y + 1 through the slope form; crossed
// is true when both endpoints land inside the rect.
struct WaterRowClip {
	float left[2] = {0.0f, 0.0f};  // out[0..1]
	float right[2] = {0.0f, 0.0f}; // out[2..3]
	bool crossed = false;          // out[4]
};

void water_clip_row_to_viewport(const WaterStripView &view, float x0, float y0,
                                float dx_over_dy, WaterRowClip &out);

// The adaptive row-march stride: steps = clamp(int(row_rhw * 500), 2, 9),
// re-derived per row from the row's homogeneous 1/w (= 1/view-depth of the
// left vertex) [orig: @ 0x5c30c7..0x5c30eb, flt_7D6FB4 = 500.0; low tier
// @ 0x5c265a]. The underwater pass never re-derives — it keeps the boot
// stride 4 [orig: var init @ 0x5c286d, the outWidth gate @ 0x5c30c5].
int water_strip_stride(float row_rhw);

// The per-vertex depth ("fog W") chain: rhw = 1/t, z = (t * uv_scale -
// uv_bias) * rhw, clamped to [8.0422355e-05 (0x38A8A8AC), 0.99993896
// (0x3F7FFC00 = 1 - 2^-14)] [orig: @ 0x5c2c0c..0x5c2c4a; clamp constants
// flt_7DBF7C / flt_7C4658]. uv_scale/uv_bias are the WaterUvState pair
// (flt_8412B0/B4) — the same globals serve the UV transform and this
// projective depth curve (z hits uv_scale's 0.99996948 * w/(w-0.2) shape,
// = 0.99996948 exactly at t = fog-int w).
float water_strip_depth(float view_depth, float uv_scale, float uv_bias);

// Depth clamp bounds [orig: flt_7DBF7C @ 0x5c2c35; flt_7C4658 @ 0x5c2c1f].
// env-tod-re.md's "[4e-5, 1 - 2^-15]" was the low-tier approximation; the
// detailed tier's witnessed bits are these.
inline constexpr float kWaterStripDepthMin = 8.0422355e-05f; // 0x38A8A8AC
inline constexpr float kWaterStripDepthMax = 0.99993896f;    // 0x3F7FFC00 = 1 - 2^-14

// The per-row color pipeline [orig: @ 0x5c2d3f..0x5c2ef6] — diffuse and
// specular are ROW-CONSTANT (written to all 3 vertices @ 0x5c2f0a..0x5c2f2b).
// base = 1 - murk (underwater view: 1 — the murk term is skipped
// [orig: @ 0x5c2d4c]; nightvision: the flat 0.1 [orig: flt_7C69F4
// @ 0x5c2d5a]); k = 0.2 + 0.8*base [orig: flt_7C6F9C/flt_7C3340];
// sin = |dy|/dist of the right-edge camera ray [orig: @ 0x5c2dd2..0x5c2def].
// Normal path [orig: @ 0x5c2e22..0x5c2e9d]:
//   brightness = int(lerp(192*k, 38.4*k, sin))        [flt_7DBFA4/flt_7DBFA8]
//   alpha_term = int(lerp(0.0, 229.5*base, sin))      [flt_7C3284/flt_7DBFA0]
//   a = clamp(int(t * 2^24 / fog_end_fp), 0, 255)     [dbl_7DBF98 = 2^24]
//   dist_alpha = 255 - a*a/255
//   diffuse = (alpha_term * dist_alpha / 255) << 24 | 0x10101 * brightness
// Underwater view [orig: @ 0x5c2df3..0x5c2e20]: diffuse = 0xFFFFFFFF and
// dist_alpha = clamp(255 - int(t * 2^24 / fog_end_fp), 0, 255) — LINEAR, no
// square. (The doc's "x255 <-> x229.5 doubles" swap is the LOW tier's
// dbl_7DBF70 @ 0x5c244b; the detailed tier multiplies 2^24 on both paths and
// its 229.5 is the float alpha_term scale.)
// Specular [orig: @ 0x5c2eb5..0x5c2ef4]: (dist_alpha << 24) |
// WaterColorLit RGB * int(lerp(255*(1-base), 128*(1-base), sin)) >> 8
// [flt_7CA29C/flt_7C461C]; the nightvision redraw drops the RGB
// [orig: @ 0x5c2ef8].
struct WaterRowColors {
	uint32_t diffuse = 0;
	uint32_t specular = 0;
};

WaterRowColors water_strip_row_colors(float row_view_depth, const float right_delta[3],
                                      int32_t fog_end_fp, float water_murk,
                                      uint32_t water_color_lit_packed,
                                      bool underwater_view, bool nightvision);

// Inputs the strip builder reads beside the view block.
struct WaterStripParams {
	int32_t plane_height_fp = 0;     // Env_WaterHeightFixed (16.16 render y)
	bool underwater_view = false;    // caller arg 2 [orig: @ 0x5c3540]
	bool nightvision = false;        // caller arg 3 [orig: @ 0x5c348e/@ 0x5c353f]
	float water_murk = 0.8f;         // Env_WaterMurk @ 0x26c6458
	uint32_t water_color_lit = 0;    // Env_WaterColorLit @ 0x26c6804 (0x00RRGGBB)
	float uv_scale = 1.0f;           // flt_8412B0 (WaterUvState::scale)
	float uv_bias = 0.0f;            // flt_8412B4 (WaterUvState::bias)
};

// The emitted rows, 3 vertices each (left / mid / right), field-for-field the
// witnessed 64-byte FVF 0x1404C4 vertex (XYZRHW | DIFFUSE | SPECULAR | TEX4,
// texcoord sizes 2/3/3/2) [orig: FVF push @ 0x5c28e1]. Texcoord 3 duplicates
// texcoord 0 in retail (@ 0x5c3095..0x5c30bf) and is not stored twice here.
// World positions are recoverable as (uv0 * 32, plane height): uv0 is the
// unprojected world x/z * 0.03125 [orig: flt_7DBFAC @ 0x5c2899].
struct WaterStripRows {
	std::vector<float> screen_pos;  // x, y pixel pairs        (+0x00/+0x04)
	std::vector<float> depth;       // the clamped depth/fog W (+0x08)
	// Reciprocal clip W. Retail perspective makes this 1 / view depth;
	// the host orthographic extension carries constant clip W = 1.
	std::vector<float> rhw;                                 // (+0x0C)
	std::vector<uint32_t> diffuse;  // packed ARGB             (+0x10)
	std::vector<uint32_t> specular; // packed ARGB             (+0x14)
	std::vector<float> uv0;         // world x/32, z/32 pairs  (+0x18)
	// The texm3x2 reflection-bump rows [orig: @ 0x5c2f83..0x5c3067]:
	// t1 = (right.x, right.z) * (-min(rhw, 0.05)/2), screen U = (sx-minX)/W;
	// t2 = (fwd.x, fwd.z) * (-5*min(rhw, 0.05)), screen V = vbase -
	// (sy-minY)/H with vbase = 1 - min(297*rhw + 0.15, 2)/256
	// [flt_7C59B0=-0.5, flt_7DBF94=-5, flt_7C68E8=0.05, flt_7DBF68=297,
	//  flt_7C6FA4=0.15, flt_7C3B90=2, flt_7C3DD4=1/128]. The underwater view
	// flips t2's V to 1 - V on all three vertices [orig: @ 0x5c306f..0x5c3085].
	std::vector<float> t1; // 3 per vertex                     (+0x20)
	std::vector<float> t2; // 3 per vertex                     (+0x2C)
};

// The march loop of the detailed tier [orig: render_water_strip_detailed
// @ 0x5c27d0]: project the plane, march rows from the block origin along
// march_dir by the adaptive stride, clip each row (hunting backward by
// single steps up to stride-1 when the line left the viewport
// [orig: @ 0x5c297d..0x5c29c3]), emit 3 vertices per row, stop at the
// 1024-row cap [orig: /192 counter @ 0x5c3135]. Returns the row count.
int water_build_strip_rows(const WaterStripView &view, const WaterStripParams &params,
                           WaterStripRows &out);

inline constexpr int kWaterStripMaxRows = 1024;

// Rows submit as <=5-row triangle-strip batches stepping 4 rows (1-row
// overlap) through the static index table: vertex_count = 8*rows - 10
// vertices locked per batch, drawn as a TRIANGLESTRIP of vertex_count - 2
// primitives [orig: @ 0x5c3195 (8n-10); DrawPrimitive(5, start, 8n-12)
// @ 0x5c3209; batch walk @ 0x5c3164..0x5c329e]. (env-tod-re.md's "8*rows-10
// primitives" is the witnessed VERTEX count; the draw call submits two
// fewer.) Batches with fewer than 2 rows are skipped.
struct WaterStripBatch {
	int32_t first_row = 0;
	int32_t rows = 0;
	int32_t vertex_count = 0;    // 8*rows - 10
	int32_t primitive_count = 0; // vertex_count - 2
};

std::vector<WaterStripBatch> water_strip_batches(int row_count);

// The static strip index table [orig: word_841328] — a batch of n rows
// consumes the first 8n-10 entries; entry values are row-relative vertex
// indices (global vertex = 3*first_row + entry). Row-pair blocks
// {3k+3, 3k, 3k+4, 3k+1, 3k+5, 3k+2} joined by {3k+2, 3k+6} degenerate
// stitches.
inline constexpr uint16_t kWaterStripIndexTable[30] = {
	3, 0, 4, 1, 5, 2, 2, 6, 6, 3, 7, 4, 8, 5, 5, 9,
	9, 6, 10, 7, 11, 8, 8, 12, 12, 9, 13, 10, 14, 11,
};

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
