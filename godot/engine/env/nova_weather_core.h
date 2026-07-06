#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <env/env_render.h>

namespace godot {

// The per-tick weather state cluster of [orig: Environment_UpdateWeatherTick
// @ 0x57e9b0]: the wind-sway oscillator, both lightning flash sequencers,
// rain fade, and the four color blocks the host smooths (ground/fill,
// directional light, fog, sky ambient). All math lives in libs/env
// (env/env_render.h); this binding owns state and the witnessed
// order-of-operations only. NovaWeather (the node) drives one tick() per
// 62 Hz frame and reads the results back. RE record: docs/env/env-tod-re.md.
class NovaWeatherCore : public RefCounted {
	GDCLASS(NovaWeatherCore, RefCounted)

private:
	opennova::env::WeatherOscillator oscillator;
	opennova::env::LightningSequencers lightning;
	opennova::env::RainState rain;
	opennova::env::WeatherColorBlock fill_block; // ground light
	opennova::env::WeatherColorBlock sun_block;  // directional light (never flashed)
	opennova::env::WeatherColorBlock fog_block;
	opennova::env::WeatherColorBlock sky_block;
	// The iris auto-exposure chain (env #17): modulator-2 -> modulator ->
	// every block, ticked ahead of the color blocks in the witnessed order
	// [orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..;
	//  target wiring @ 0x57e512..0x57e538]. The target defaults to identity
	// until set_exposure_from_iris runs.
	opennova::env::ModulatorChain modulator_chain;
	// The cloud-scroll rate + accumulators [orig: rate ramp @ 0x57eecc,
	// accumulators @ 0x57f1a5..0x57f1d1] — the tick's tail. The rate always
	// RAMPS toward sky_speed << 10 (the snap refreshes only the target), so a
	// fresh core ramps in from 0 exactly like retail from boot.
	opennova::env::CloudScrollState cloud_scroll;
	// The smoothed scalar channels (env #27): fog distance + sky height (+
	// sun-dim/rain/overcast state, consumers pending) — targets refreshed by
	// the host per tick, currents always ramping [orig: the weather tick's
	// scalar tail @ 0x57edd7..0x57ef92; targets-only snap @ 0x57d1e0].
	opennova::env::EnvScalarChannels scalar_channels;
	// OpenNova authoring extension: an ARMED wind timer whose expiry decays
	// the oscillator intensity (x31 >> 5 per tick) back to zero. Unarmed
	// wind never decays — retail's Env_WindScale is a constant
	// (Environment_InitDefaults @ 0x57c1d1 is its only writer), so a set
	// strength holds until the author changes it. Marked as an extension in
	// docs/env/env-tod-re.md.
	int wind_duration_ticks = 0;
	bool wind_armed = false;

protected:
	static void _bind_methods();

public:
	// Snaps all four block states (and their packed colors) to the given
	// colors — the discrete-scrub resync path.
	void snap_colors(const Color &p_fill, const Color &p_sun, const Color &p_fog, const Color &p_sky);

	// One 62 Hz weather tick in the witnessed order: oscillator, wind-decay
	// extension, rain fade, lightning sequencers (additive slot rewrites on
	// epoch), the four block pipelines, then the cloud-scroll ramp +
	// accumulators (the tick's tail).
	void tick(const Color &p_fill_target, const Color &p_sun_target,
			const Color &p_fog_target, const Color &p_sky_target,
			const Color &p_lightning_color, float p_sky_speed);

	// The cloud-scroll sub-tick alone (rate ramp toward sky_speed << 10 +
	// the four accumulators) — for hosts that own no weather colors (the
	// standalone-sky fallback path). tick() calls this itself.
	void tick_cloud_scroll(float p_sky_speed);

	// Raw Env_WindScale units. The witnessed retail value is 256; the
	// oscillator's 15*prev feedback is stable only for intensity <= 273.
	void set_wind_intensity(int p_value);
	int get_wind_intensity() const;
	void set_wind_duration_ticks(int p_ticks);
	int get_wind_duration_ticks() const;

	void trigger_lightning_short();
	void trigger_lightning_long();

	// Set the modulator's exposure target from the OUTDOOR iris sample at the
	// current smoothed colors: gain = iris_gain(light[1], sky[1], ground[1],
	// light_dir, iris params), target = 0x10101 * gain chased over 62 ticks.
	// The outdoor sample assumes full sun visibility (8/8 rays) and no cover —
	// the interior/occlusion sampling of the original's 3-point camera-ray
	// march rides the unhosted interior system (docs/render/
	// render-lighting-re.md D-RLIT-2).
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538;
	//  compute_ambient_light_along_direction @ 0x5c7a00;
	//  terrain_sector_compute_lighting @ 0x5c7550]
	void set_exposure_from_iris(const Vector3 &p_light_dir, float p_iris_percent, float p_iris_center);

	// The modulator's render color / 64 — ColorSrcGlobalGain and the
	// effects/foliage ambient scale [orig: Render_UnpackModulatorToLightScale
	// @ 0x58db30; EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0].
	Vector3 get_color_src_gain() const;

	Color get_fill() const;
	Color get_sun() const;
	Color get_fog() const;
	Color get_sky() const;

	// (smoothed - 0x8000) / 0x8000 * 2 — the shader-facing sway scalar.
	float get_sway_amount() const;
	// ring_index * (TAU / 256) — the shader-facing sway phase.
	float get_sway_phase() const;
	// Last SET flash level / 255.
	float get_lightning_intensity() const;

	// The witnessed per-layer cloud UV translations for a camera at
	// (cam_x, cam_z) world units: U = +cam/4096 - acc*2^-28|29,
	// V = +cam/4096|8192 + acc*2^-28|29 [orig: render_skybox
	// @ 0x5791de..0x579260].
	Vector2 get_cloud_uv_offset1(float p_cam_x, float p_cam_z) const;
	Vector2 get_cloud_uv_offset2(float p_cam_x, float p_cam_z) const;
	// Layer-1 UV drift per second at the current smoothed rate — the water
	// surface's scroll-speed source.
	float get_cloud_uv_rate_per_second() const;

	// env #27: refresh the scalar spring targets (world units); currents ramp.
	void set_scalar_targets(float p_fog_distance, float p_sky_height);
	// The smoothed scalar currents (world units / percent).
	float get_fog_distance() const;
	float get_sky_height() const;
	float get_sun_dim_pct() const;
	float get_rain_pct() const;

	// The witnessed water UV transform (scale, bias, offset_u, offset_v):
	// scale/bias from the fog-distance INT part, offsets from the layer-1
	// cloud accumulators + 32x camera [orig: render_water_surface
	// @ 0x5c3348..0x5c33db].
	Vector4 get_water_uv_state(float p_cam_x, float p_cam_z, float p_fog_distance) const;
};

} // namespace godot
