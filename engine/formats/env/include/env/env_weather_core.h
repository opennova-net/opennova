// The per-tick weather state cluster and its witnessed order-of-operations —
// the [orig: Environment_UpdateWeatherTick @ 0x57e9b0] aggregate over the
// env_weather.h pieces: the wind-sway oscillator, rain fade, both lightning
// flash sequencers, the smoothed scalar channels, the iris modulator chain,
// the fourteen color-block pipelines in retail order, and the cloud-scroll
// tail. The shell adapter (godot/src/env/nova_weather_core.*) owns only
// the Godot boxing over this struct. RE record: docs/env/env-tod-re.md.
#ifndef OPENNOVA_ENV_WEATHER_CORE_H
#define OPENNOVA_ENV_WEATHER_CORE_H

#include "env/env_weather.h"

#include <cstdint>

namespace opennova::env {

struct WeatherCore {
	WeatherOscillator oscillator;
	LightningSequencers lightning;
	RainState rain;
	WeatherColorBlock fill_block; // ground light
	WeatherColorBlock sun_block;  // directional light (never flashed)
	WeatherColorBlock fog_block;
	WeatherColorBlock sky_block;
	SkyWeatherColorBlocks sky_color_blocks;
	// The iris auto-exposure chain (env #17): modulator-2 -> modulator -> every
	// block, ticked ahead of the color blocks in the witnessed order
	// [orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..;
	//  target wiring @ 0x57e512..0x57e538]. The target defaults to identity
	// until an exposure setter runs.
	ModulatorChain modulator_chain;
	// The cloud-scroll rate + accumulators [orig: rate ramp @ 0x57eecc,
	// accumulators @ 0x57f1a5..0x57f1d1] — the tick's tail. The rate always
	// RAMPS toward sky_speed << 10 (the snap refreshes only the target), so a
	// fresh core ramps in from 0 exactly like retail from boot.
	CloudScrollState cloud_scroll;
	// The smoothed scalar channels (env #27): fog distance + sky height (+
	// sun-dim/rain/overcast state) — targets refreshed by the embedder per tick.
	EnvScalarChannels scalar_channels;
	// OpenNova authoring extension: an ARMED wind timer whose expiry decays
	// the oscillator intensity (x31 >> 5 per tick) back to zero. Unarmed
	// wind never decays — retail's Env_WindScale is a constant
	// (Environment_InitDefaults @ 0x57c1d1 is its only writer), so a set
	// strength holds until the author changes it. Marked as an extension in
	// docs/env/env-tod-re.md.
	int wind_duration_ticks = 0;
	bool wind_armed = false;

	// Per-sample classification codes for set_exposure_from_iris_samples.
	// Outdoor samples carry their sun-occlusion level 0..8 directly.
	static constexpr int32_t kIrisSampleIndoor = -1;
	static constexpr int32_t kIrisSampleIndoorNoData = -2;

	// One 62 Hz weather tick in the witnessed order: oscillator, wind-decay
	// extension, rain fade, lightning sequencers (whose epoch hits rewrite the
	// additive slots [orig: Environment_SetLightningFlash @ 0x57d320 —
	// sky >> 8, fog >> 9, ground >> 10, the directional slot zeroed]), the
	// scalar springs, modulator-2 then modulator, the block pipelines against
	// the fresh modulator color, then the cloud-scroll tail. Color targets and
	// the lightning color are packed 0x00RRGGBB (targets may carry alpha).
	void tick(uint32_t fill_target, uint32_t sun_target, uint32_t fog_target,
			uint32_t sky_target, uint32_t lightning_packed, float sky_speed);

	// The cloud-scroll sub-tick alone (rate ramp toward sky_speed << 10, the
	// atol parse scale [orig: TimeOfDay_ParseProperty @ 0x57cc0d; ramp
	// @ 0x57eecc], plus the accumulators) — for embedders with no weather
	// colors (the standalone-sky fallback path). tick() calls this itself.
	void tick_cloud_scroll(float sky_speed);

	// Raw Env_WindScale units; arming semantics per the extension note above.
	void set_wind_intensity(int value);
	void set_wind_duration_ticks(int ticks);

	// The outdoor iris sample at the current smoothed colors: gain =
	// iris_gain(light[1], sky[1], ground[1], light_dir, iris params) —
	// full sun visibility (8/8 rays), no cover — chased into the modulator
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538;
	//  terrain_sector_compute_lighting @ 0x5c7550 reads
	//  Env_LightBlock[1]/Env_SkyBlock[1]/Env_GroundBlock[1]].
	void set_exposure_from_outdoor_iris(float light_x, float light_y,
			float light_z, float iris_percent, float iris_center);

	// The shader-facing sway pair and lightning intensity — ONE home for the
	// WeatherRuntime owner and the Godot binding (they must never diverge).
	// sway_amount = (oscillator.smoothed - 0x8000) / 0x8000 * 2 (the signed
	// wind strength the vegetation-sway shaders scale by), sway_phase =
	// oscillator.ring_index * (2*pi / 256) (the ring position as an angle),
	// lightning_intensity = the last SET flash level / 255.
	float sway_amount() const;
	float sway_phase() const;
	float lightning_intensity() const;

	// The in-world marched exposure: one classification per sample
	// (kIrisSampleIndoor / kIrisSampleIndoorNoData / outdoor sun level 0..8);
	// each runs the iris curve — indoors against the static ceiling/floor
	// indoor ambient with a zeroed directional, outdoors against
	// light[1]*level/8, sky[1], ground[1] — and the INT gains average into the
	// modulator target. No samples -> the outdoor form above.
	// [orig: compute_ambient_light_along_direction @ 0x5c7a00;
	//  terrain_sector_compute_lighting @ 0x5c7550 — indoor swap @ 0x5c7660..,
	//  no-interior-data 255 @ 0x5c7652, sun level @ 0x5c7784..0x5c77e9;
	//  average @ 0x5c7b45]
	void set_exposure_from_iris_samples(const int32_t *samples, int count,
			const Rgb &ceiling, const Rgb &floor, float light_x, float light_y,
			float light_z, float iris_percent, float iris_center);
};

} // namespace opennova::env

#endif // OPENNOVA_ENV_WEATHER_CORE_H
