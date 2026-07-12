#include "env/nova_weather_core.h"

#include <renderer/light_runtime.h>

#include <algorithm>

using namespace godot;

namespace {

// Same rounding as NovaColorSmoother's packer (and the GUT vector hex idiom):
// byte = int(channel * 255 + 0.5).
uint32_t color_to_packed(const Color &color) {
	const auto to_byte = [](float v) -> uint32_t {
		return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
	};
	return to_byte(color.b) | (to_byte(color.g) << 8) | (to_byte(color.r) << 16) | (to_byte(color.a) << 24);
}

Color packed_to_color(uint32_t packed) {
	return Color(
			static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
			static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
			static_cast<float>(packed & 0xFF) / 255.0f,
			static_cast<float>((packed >> 24) & 0xFF) / 255.0f);
}

} // namespace

void NovaWeatherCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snap_colors", "fill", "sun", "fog", "sky"), &NovaWeatherCore::snap_colors);
	ClassDB::bind_method(D_METHOD("tick", "fill_target", "sun_target", "fog_target", "sky_target", "lightning_color",
								  "sky_speed"),
			&NovaWeatherCore::tick);
	ClassDB::bind_method(D_METHOD("tick_cloud_scroll", "sky_speed"), &NovaWeatherCore::tick_cloud_scroll);
	ClassDB::bind_method(D_METHOD("set_scalar_targets", "fog_distance", "sky_height"), &NovaWeatherCore::set_scalar_targets);
	ClassDB::bind_method(D_METHOD("get_fog_distance"), &NovaWeatherCore::get_fog_distance);
	ClassDB::bind_method(D_METHOD("get_sky_height"), &NovaWeatherCore::get_sky_height);
	ClassDB::bind_method(D_METHOD("get_sun_dim_pct"), &NovaWeatherCore::get_sun_dim_pct);
	ClassDB::bind_method(D_METHOD("get_rain_pct"), &NovaWeatherCore::get_rain_pct);
	ClassDB::bind_method(D_METHOD("get_cloud_uv_offset1", "cam_x", "cam_z"), &NovaWeatherCore::get_cloud_uv_offset1);
	ClassDB::bind_method(D_METHOD("get_cloud_uv_offset2", "cam_x", "cam_z"), &NovaWeatherCore::get_cloud_uv_offset2);
	ClassDB::bind_method(D_METHOD("get_cloud_uv_rate_per_second"), &NovaWeatherCore::get_cloud_uv_rate_per_second);
	ClassDB::bind_method(D_METHOD("get_water_uv_state", "cam_x", "cam_z", "fog_distance"),
			&NovaWeatherCore::get_water_uv_state);
	ClassDB::bind_method(D_METHOD("set_wind_intensity", "value"), &NovaWeatherCore::set_wind_intensity);
	ClassDB::bind_method(D_METHOD("get_wind_intensity"), &NovaWeatherCore::get_wind_intensity);
	ClassDB::bind_method(D_METHOD("set_wind_duration_ticks", "ticks"), &NovaWeatherCore::set_wind_duration_ticks);
	ClassDB::bind_method(D_METHOD("get_wind_duration_ticks"), &NovaWeatherCore::get_wind_duration_ticks);
	ClassDB::bind_method(D_METHOD("trigger_lightning_short"), &NovaWeatherCore::trigger_lightning_short);
	ClassDB::bind_method(D_METHOD("trigger_lightning_long"), &NovaWeatherCore::trigger_lightning_long);
	ClassDB::bind_method(D_METHOD("set_exposure_from_iris", "light_dir", "iris_percent", "iris_center"),
			&NovaWeatherCore::set_exposure_from_iris);
	ClassDB::bind_method(D_METHOD("get_color_src_gain"), &NovaWeatherCore::get_color_src_gain);
	ClassDB::bind_method(D_METHOD("get_fill"), &NovaWeatherCore::get_fill);
	ClassDB::bind_method(D_METHOD("get_sun"), &NovaWeatherCore::get_sun);
	ClassDB::bind_method(D_METHOD("get_fog"), &NovaWeatherCore::get_fog);
	ClassDB::bind_method(D_METHOD("get_sky"), &NovaWeatherCore::get_sky);
	ClassDB::bind_method(D_METHOD("get_wave_osc_ring0"), &NovaWeatherCore::get_wave_osc_ring0);
	ClassDB::bind_method(D_METHOD("get_sway_amount"), &NovaWeatherCore::get_sway_amount);
	ClassDB::bind_method(D_METHOD("get_sway_phase"), &NovaWeatherCore::get_sway_phase);
	ClassDB::bind_method(D_METHOD("get_lightning_intensity"), &NovaWeatherCore::get_lightning_intensity);
}

void NovaWeatherCore::snap_colors(const Color &p_fill, const Color &p_sun, const Color &p_fog, const Color &p_sky) {
	fill_block.snap(color_to_packed(p_fill));
	sun_block.snap(color_to_packed(p_sun));
	fog_block.snap(color_to_packed(p_fog));
	sky_block.snap(color_to_packed(p_sky));
}

void NovaWeatherCore::tick(const Color &p_fill_target, const Color &p_sun_target,
		const Color &p_fog_target, const Color &p_sky_target,
		const Color &p_lightning_color, float p_sky_speed) {
	fill_block.target = color_to_packed(p_fill_target);
	sun_block.target = color_to_packed(p_sun_target);
	fog_block.target = color_to_packed(p_fog_target);
	sky_block.target = color_to_packed(p_sky_target);

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
		const opennova::env::LightningAdditivesPacked additives =
				opennova::env::lightning_additives_packed(
						color_to_packed(p_lightning_color) & 0xFFFFFFu, lightning.level);
		sky_block.additive = additives.sky;
		fog_block.additive = additives.fog;
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
	fill_block.tick(modulator_packed, rain.intensity);
	sun_block.tick(modulator_packed, rain.intensity);
	fog_block.tick(modulator_packed, rain.intensity);
	sky_block.tick(modulator_packed, rain.intensity);

	// The tick's tail [orig: accumulators @ 0x57f1a5..0x57f1d1].
	tick_cloud_scroll(p_sky_speed);
}

void NovaWeatherCore::tick_cloud_scroll(float p_sky_speed) {
	// rate_target = sky_speed << 10, the atol parse scale
	// [orig: TimeOfDay_ParseProperty @ 0x57cc0d; ramp @ 0x57eecc].
	cloud_scroll.tick(static_cast<int>(p_sky_speed) << 10);
}

void NovaWeatherCore::set_wind_intensity(int p_value) {
	oscillator.intensity = std::max(0, p_value);
}

int NovaWeatherCore::get_wind_intensity() const {
	return oscillator.intensity;
}

void NovaWeatherCore::set_wind_duration_ticks(int p_ticks) {
	wind_duration_ticks = std::max(0, p_ticks);
	wind_armed = wind_duration_ticks > 0;
}

int NovaWeatherCore::get_wind_duration_ticks() const {
	return wind_duration_ticks;
}

void NovaWeatherCore::trigger_lightning_short() {
	lightning.trigger_short();
}

void NovaWeatherCore::trigger_lightning_long() {
	lightning.trigger_long();
}

void NovaWeatherCore::set_exposure_from_iris(const Vector3 &p_light_dir, float p_iris_percent, float p_iris_center) {
	// The iris inputs are the blocks' [1] slots (step + lightning additive,
	// pre modulation) / 255 [orig: terrain_sector_compute_lighting @ 0x5c7550
	// reads Env_LightBlock[1]/Env_SkyBlock[1]/Env_GroundBlock[1]]; the outdoor
	// directional term keeps full sun visibility (8/8 rays).
	const auto to_rgb = [](uint32_t packed) {
		opennova::env::Rgb c;
		c.r = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
		c.g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
		c.b = static_cast<float>(packed & 0xFF) / 255.0f;
		return c;
	};
	const int gain = opennova::env::iris_gain(
			to_rgb(sun_block.pre_mod_color),
			to_rgb(sky_block.pre_mod_color),
			to_rgb(fill_block.pre_mod_color),
			p_light_dir.x, p_light_dir.y, p_light_dir.z,
			p_iris_center, p_iris_percent);
	// target = 0x10101 * gain, chased over 62 ticks (1 s)
	// [orig: @ 0x57e512..0x57e538 -> ColorBlock_SetStepDeltas @ 0x57d940].
	modulator_chain.set_exposure_target(gain);
}

Vector3 NovaWeatherCore::get_color_src_gain() const {
	const std::array<float, 3> scale =
			renderer::unpack_modulator_scale(modulator_chain.render_color() & 0xFFFFFFu);
	return Vector3(scale[0], scale[1], scale[2]);
}

Color NovaWeatherCore::get_fill() const {
	return packed_to_color(fill_block.render_color);
}

Color NovaWeatherCore::get_sun() const {
	return packed_to_color(sun_block.render_color);
}

Color NovaWeatherCore::get_fog() const {
	return packed_to_color(fog_block.render_color);
}

Color NovaWeatherCore::get_sky() const {
	return packed_to_color(sky_block.render_color);
}

int NovaWeatherCore::get_wave_osc_ring0() const {
	return oscillator.osc_ring[0];
}

float NovaWeatherCore::get_sway_amount() const {
	return static_cast<float>(oscillator.smoothed - 0x8000) / 32768.0f * 2.0f;
}

float NovaWeatherCore::get_sway_phase() const {
	return static_cast<float>(oscillator.ring_index) * (Math_TAU / 256.0f);
}

float NovaWeatherCore::get_lightning_intensity() const {
	return static_cast<float>(lightning.level) / 255.0f;
}

Vector2 NovaWeatherCore::get_cloud_uv_offset1(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			opennova::env::cloud_scroll_uv_offsets(cloud_scroll, p_cam_x, p_cam_z);
	return Vector2(offsets.u1, offsets.v1);
}

Vector2 NovaWeatherCore::get_cloud_uv_offset2(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			opennova::env::cloud_scroll_uv_offsets(cloud_scroll, p_cam_x, p_cam_z);
	return Vector2(offsets.u2, offsets.v2);
}

float NovaWeatherCore::get_cloud_uv_rate_per_second() const {
	return opennova::env::cloud_uv_rate_per_second(cloud_scroll);
}

Vector4 NovaWeatherCore::get_water_uv_state(float p_cam_x, float p_cam_z, float p_fog_distance) const {
	const opennova::env::WaterUvState state =
			opennova::env::water_uv_state(cloud_scroll, p_cam_x, p_cam_z, p_fog_distance);
	return Vector4(state.scale, state.bias, state.offset_u, state.offset_v);
}

void NovaWeatherCore::set_scalar_targets(float p_fog_distance, float p_sky_height) {
	// Targets only — the currents always ramp, exactly like the witnessed
	// mission-start snap [orig: Environment_SnapStateToTargets @ 0x57d1e0:
	// Env_FogDistTarget <- Env_FogLevelFixed, sky target <- Env_SkyHeightFixed].
	scalar_channels.fog_dist_target_fp = static_cast<int32_t>(p_fog_distance * 65536.0f);
	scalar_channels.sky_height_target_fp = static_cast<int32_t>(p_sky_height * 65536.0f);
}

float NovaWeatherCore::get_fog_distance() const {
	return static_cast<float>(scalar_channels.fog_dist_fp) / 65536.0f;
}

float NovaWeatherCore::get_sky_height() const {
	return static_cast<float>(scalar_channels.sky_height_fp) / 65536.0f;
}

float NovaWeatherCore::get_sun_dim_pct() const {
	return static_cast<float>(scalar_channels.sun_dim_fp) / 65536.0f;
}

float NovaWeatherCore::get_rain_pct() const {
	return static_cast<float>(scalar_channels.rain_pct_fp) / 65536.0f;
}
