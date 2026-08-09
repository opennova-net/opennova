#include "env/env_weather_core.h"

#include <algorithm>

namespace opennova::env {

namespace {

Rgb packed_to_rgb01(uint32_t packed) {
	Rgb c;
	c.r = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
	c.g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
	c.b = static_cast<float>(packed & 0xFF) / 255.0f;
	return c;
}

} // namespace

void WeatherCore::tick(uint32_t fill_target, uint32_t sun_target,
		uint32_t fog_target, uint32_t sky_target, uint32_t lightning_packed,
		float sky_speed) {
	fill_block.target = fill_target;
	sun_block.target = sun_target;
	fog_block.target = fog_target;
	sky_block.target = sky_target;

	// [orig: Environment_UpdateWeatherTick @ 0x57e9b0] tick order: the
	// oscillator, rain fade, the lightning sequencers (whose epoch hits
	// rewrite the additive slots via Environment_SetLightningFlash
	// @ 0x57d320 — sky >> 8, fog >> 9, ground >> 10, the directional slot
	// zeroed), then every color block's step/additive/modulate pipeline.
	oscillator.tick();
	// Authoring extension (see header): only an ARMED, expired gust decays.
	if (wind_duration_ticks > 0) {
		--wind_duration_ticks;
	}
	if (wind_armed && wind_duration_ticks == 0) {
		if (oscillator.intensity > 0) {
			oscillator.intensity = (oscillator.intensity * 31) >> 5;
		} else {
			wind_armed = false;
		}
	}
	rain.tick();
	// The scalar spring channels step between the sequencers and the color
	// blocks — the witnessed in-tick position [orig: the scalar tail
	// @ 0x57edd7..0x57ef92 runs before the 16 interpolate_weather_color
	// calls @ 0x57ef97..] (env #27).
	scalar_channels.tick();
	if (lightning.tick()) {
		const LightningAdditivesPacked additives = lightning_additives_packed(
				lightning_packed & 0xFFFFFFu, lightning.level);
		sky_block.additive = additives.sky;
		fog_block.additive = additives.fog;
		sky_color_blocks.set_skyfog_additive(additives.skyfog);
		fill_block.additive = additives.ground;
		sun_block.additive = 0;
	}
	// The iris modulator chain (env #17, REN-5): modulator-2 then the
	// modulator tick FIRST, then every color block modulates against the
	// modulator's fresh render color — the witnessed same-tick order
	// [orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..
	//  0x57f03c: 0x26c6678 modulator2, 0x26c6644 modulator, then the color
	//  blocks]. Rain enters as the witnessed per-block blend factor.
	modulator_chain.tick(rain.intensity);
	const uint32_t modulator_packed = modulator_chain.render_color();
	sun_block.tick(modulator_packed, rain.intensity);
	sky_block.tick(modulator_packed, rain.intensity);
	fill_block.tick(modulator_packed, rain.intensity);
	fog_block.tick(modulator_packed, rain.intensity);
	sky_color_blocks.tick_skyfog(modulator_packed, rain.intensity);
	sky_color_blocks.tick_statics(modulator_packed, rain.intensity);
	sky_color_blocks.tick_dome(modulator_packed, rain.intensity);

	// The tick's tail [orig: accumulators @ 0x57f1a5..0x57f1d1].
	tick_cloud_scroll(sky_speed);
}

void WeatherCore::tick_cloud_scroll(float sky_speed) {
	// rate_target = sky_speed << 10, the atol parse scale
	// [orig: TimeOfDay_ParseProperty @ 0x57cc0d; ramp @ 0x57eecc].
	cloud_scroll.tick(static_cast<int>(sky_speed) << 10);
}

void WeatherCore::set_wind_intensity(int value) {
	oscillator.intensity = std::max(0, value);
}

void WeatherCore::set_wind_duration_ticks(int ticks) {
	wind_duration_ticks = std::max(0, ticks);
	wind_armed = wind_duration_ticks > 0;
}

void WeatherCore::set_exposure_from_outdoor_iris(float light_x, float light_y,
		float light_z, float iris_percent, float iris_center) {
	// The iris inputs are the blocks' [1] slots (step + lightning additive,
	// pre modulation) / 255 [orig: terrain_sector_compute_lighting @ 0x5c7550
	// reads Env_LightBlock[1]/Env_SkyBlock[1]/Env_GroundBlock[1]]; the outdoor
	// directional term keeps full sun visibility (8/8 rays).
	const int gain = iris_gain(
			packed_to_rgb01(sun_block.pre_mod_color),
			packed_to_rgb01(sky_block.pre_mod_color),
			packed_to_rgb01(fill_block.pre_mod_color),
			light_x, light_y, light_z, iris_center, iris_percent);
	// target = 0x10101 * gain, chased over 62 ticks (1 s)
	// [orig: @ 0x57e512..0x57e538 -> ColorBlock_SetStepDeltas @ 0x57d940].
	modulator_chain.set_exposure_target(gain);
}

void WeatherCore::set_exposure_from_iris_samples(const int32_t *samples,
		int count, const Rgb &ceiling, const Rgb &floor, float light_x,
		float light_y, float light_z, float iris_percent, float iris_center) {
	// The in-world marched exposure: three samples along the clipped 8-unit
	// camera ray, each classified by the embedder (indoor / indoor-without-
	// interior-data / outdoor sun level 0..8), each run through the iris
	// curve, the INT gains averaged /3 [orig:
	// compute_ambient_light_along_direction @ 0x5c7a00 — samples at hit,
	// hit+(cam-hit)/3, hit+2(cam-hit)/3; (s0+s1+s2)/3 @ 0x5c7b45].
	if (samples == nullptr || count <= 0) {
		set_exposure_from_outdoor_iris(light_x, light_y, light_z,
				iris_percent, iris_center);
		return;
	}
	const Rgb zero_rgb{};
	int sum = 0;
	for (int i = 0; i < count; ++i) {
		const int32_t sample = samples[i];
		int gain;
		if (sample == kIrisSampleIndoorNoData) {
			// Indoor hit on an entity with no interior data: every input stays
			// zero, the curve's base/(2m) limb diverges, the clamp serves 255
			// [orig: the pool_entry[12]==0 skip to the curve @ 0x5c7652].
			gain = 255;
		} else if (sample == kIrisSampleIndoor) {
			// Indoors: directional zeroed, sky/ground replaced by the static
			// ceiling/floor indoor ambient blocks
			// [orig: @ 0x5c7660..0x5c76fe — Env_CeilingBlock/Env_FloorBlock].
			gain = iris_gain(zero_rgb, ceiling, floor,
					light_x, light_y, light_z, iris_center, iris_percent);
		} else {
			// Outdoors: the directional block scaled by the sun-occlusion level
			// (8 minus one per blocked ray, 3 rays -> 5..8)
			// [orig: @ 0x5c7784..0x5c77d7 -> light_scale = level/8/255 @ 0x5c77e9].
			const int level = std::clamp(static_cast<int>(sample), 0, 8);
			Rgb dir = packed_to_rgb01(sun_block.pre_mod_color);
			const float scale = static_cast<float>(level) / 8.0f;
			dir.r *= scale;
			dir.g *= scale;
			dir.b *= scale;
			gain = iris_gain(dir,
					packed_to_rgb01(sky_block.pre_mod_color),
					packed_to_rgb01(fill_block.pre_mod_color),
					light_x, light_y, light_z, iris_center, iris_percent);
		}
		sum += gain;
	}
	// (s0 + s1 + s2) / 3 in integer form [orig: @ 0x5c7b45].
	modulator_chain.set_exposure_target(sum / count);
}

} // namespace opennova::env
