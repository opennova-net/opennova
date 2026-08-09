#include "env/nova_weather_core.h"

#include <env/env_water_render.h> // water_uv_state rides the layer-1 cloud accumulators

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

opennova::env::Rgb packed_to_rgb01(uint32_t packed) {
	opennova::env::Rgb c;
	c.r = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
	c.g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
	c.b = static_cast<float>(packed & 0xFF) / 255.0f;
	return c;
}

opennova::env::Rgb color_to_rgb(const Color &color) {
	opennova::env::Rgb c;
	c.r = color.r;
	c.g = color.g;
	c.b = color.b;
	return c;
}

Color rgb_to_color(const opennova::env::Rgb &rgb) {
	return Color(rgb.r, rgb.g, rgb.b);
}

} // namespace

void NovaWeatherCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snap_colors", "fill", "sun", "fog", "sky"), &NovaWeatherCore::snap_colors);
	ClassDB::bind_method(
			D_METHOD("snap_sky_colors", "skyfog", "ceiling", "cloud", "floor", "skybase", "skybright", "skyhighlight",
					"cloudbase", "cloudhighlight", "cloudedge"),
			&NovaWeatherCore::snap_sky_colors);
	ClassDB::bind_method(
			D_METHOD("set_sky_color_targets", "skyfog", "ceiling", "cloud", "floor", "skybase", "skybright", "skyhighlight",
					"cloudbase", "cloudhighlight", "cloudedge"),
			&NovaWeatherCore::set_sky_color_targets);
	ClassDB::bind_method(D_METHOD("tick", "fill_target", "sun_target", "fog_target", "sky_target", "lightning_color",
								  "sky_speed"),
			&NovaWeatherCore::tick);
	ClassDB::bind_method(D_METHOD("tick_cloud_scroll", "sky_speed"), &NovaWeatherCore::tick_cloud_scroll);
	ClassDB::bind_method(D_METHOD("set_scalar_targets", "fog_distance", "sky_height"), &NovaWeatherCore::set_scalar_targets);
	ClassDB::bind_method(D_METHOD("snap_scalar_currents_to_targets"),
			&NovaWeatherCore::snap_scalar_currents_to_targets);
	ClassDB::bind_method(D_METHOD("apply_network_environment_sample", "fog_dist", "fog_accel", "rain_pct", "overcast"),
			&NovaWeatherCore::apply_network_environment_sample);
	ClassDB::bind_method(D_METHOD("get_fog_distance"), &NovaWeatherCore::get_fog_distance);
	ClassDB::bind_method(D_METHOD("get_sky_height"), &NovaWeatherCore::get_sky_height);
	ClassDB::bind_method(D_METHOD("get_sun_dim_pct"), &NovaWeatherCore::get_sun_dim_pct);
	ClassDB::bind_method(D_METHOD("get_rain_pct"), &NovaWeatherCore::get_rain_pct);
	ClassDB::bind_method(D_METHOD("get_fog_accel_clamp_fixed"), &NovaWeatherCore::get_fog_accel_clamp_fixed);
	ClassDB::bind_method(D_METHOD("get_rain_pct_fixed"), &NovaWeatherCore::get_rain_pct_fixed);
	ClassDB::bind_method(D_METHOD("get_overcast_blend_fixed"), &NovaWeatherCore::get_overcast_blend_fixed);
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
	ClassDB::bind_method(
			D_METHOD("set_exposure_from_iris_samples", "samples", "light_dir", "ceiling", "floor", "iris_percent", "iris_center"),
			&NovaWeatherCore::set_exposure_from_iris_samples);
	ClassDB::bind_method(D_METHOD("get_color_src_gain"), &NovaWeatherCore::get_color_src_gain);
	ClassDB::bind_method(D_METHOD("get_fill"), &NovaWeatherCore::get_fill);
	ClassDB::bind_method(D_METHOD("get_sun"), &NovaWeatherCore::get_sun);
	ClassDB::bind_method(D_METHOD("get_fog"), &NovaWeatherCore::get_fog);
	ClassDB::bind_method(D_METHOD("get_sky"), &NovaWeatherCore::get_sky);
	ClassDB::bind_method(D_METHOD("get_skyfog"), &NovaWeatherCore::get_skyfog);
	ClassDB::bind_method(D_METHOD("get_ceiling"), &NovaWeatherCore::get_ceiling);
	ClassDB::bind_method(D_METHOD("get_cloud"), &NovaWeatherCore::get_cloud);
	ClassDB::bind_method(D_METHOD("get_floor"), &NovaWeatherCore::get_floor);
	ClassDB::bind_method(D_METHOD("get_ceiling_pre_mod"), &NovaWeatherCore::get_ceiling_pre_mod);
	ClassDB::bind_method(D_METHOD("get_floor_pre_mod"), &NovaWeatherCore::get_floor_pre_mod);
	ClassDB::bind_method(D_METHOD("get_skybase"), &NovaWeatherCore::get_skybase);
	ClassDB::bind_method(D_METHOD("get_skybright"), &NovaWeatherCore::get_skybright);
	ClassDB::bind_method(D_METHOD("get_skyhighlight"), &NovaWeatherCore::get_skyhighlight);
	ClassDB::bind_method(D_METHOD("get_cloudbase"), &NovaWeatherCore::get_cloudbase);
	ClassDB::bind_method(D_METHOD("get_cloudhighlight"), &NovaWeatherCore::get_cloudhighlight);
	ClassDB::bind_method(D_METHOD("get_cloudedge"), &NovaWeatherCore::get_cloudedge);
	ClassDB::bind_method(D_METHOD("get_sway_amount"), &NovaWeatherCore::get_sway_amount);
	ClassDB::bind_method(D_METHOD("get_sway_phase"), &NovaWeatherCore::get_sway_phase);
	ClassDB::bind_method(D_METHOD("get_lightning_intensity"), &NovaWeatherCore::get_lightning_intensity);
}

void NovaWeatherCore::snap_colors(const Color &p_fill, const Color &p_sun, const Color &p_fog, const Color &p_sky) {
	core_.fill_block.snap(color_to_packed(p_fill));
	core_.sun_block.snap(color_to_packed(p_sun));
	core_.fog_block.snap(color_to_packed(p_fog));
	core_.sky_block.snap(color_to_packed(p_sky));
}

void NovaWeatherCore::snap_sky_colors(const Color &p_skyfog, const Color &p_ceiling,
		const Color &p_cloud, const Color &p_floor, const Color &p_skybase,
		const Color &p_skybright, const Color &p_skyhighlight,
		const Color &p_cloudbase, const Color &p_cloudhighlight,
		const Color &p_cloudedge) {
	core_.sky_color_blocks.snap({
			color_to_packed(p_skyfog),
			color_to_packed(p_ceiling),
			color_to_packed(p_cloud),
			color_to_packed(p_floor),
			color_to_packed(p_skybase),
			color_to_packed(p_skybright),
			color_to_packed(p_skyhighlight),
			color_to_packed(p_cloudbase),
			color_to_packed(p_cloudhighlight),
			color_to_packed(p_cloudedge),
	});
}

void NovaWeatherCore::set_sky_color_targets(const Color &p_skyfog, const Color &p_ceiling,
		const Color &p_cloud, const Color &p_floor, const Color &p_skybase,
		const Color &p_skybright, const Color &p_skyhighlight,
		const Color &p_cloudbase, const Color &p_cloudhighlight,
		const Color &p_cloudedge) {
	core_.sky_color_blocks.set_targets({
			color_to_packed(p_skyfog),
			color_to_packed(p_ceiling),
			color_to_packed(p_cloud),
			color_to_packed(p_floor),
			color_to_packed(p_skybase),
			color_to_packed(p_skybright),
			color_to_packed(p_skyhighlight),
			color_to_packed(p_cloudbase),
			color_to_packed(p_cloudhighlight),
			color_to_packed(p_cloudedge),
	});
}

void NovaWeatherCore::tick(const Color &p_fill_target, const Color &p_sun_target,
		const Color &p_fog_target, const Color &p_sky_target,
		const Color &p_lightning_color, float p_sky_speed) {
	// The witnessed tick order is env::WeatherCore::tick's
	// [orig: Environment_UpdateWeatherTick @ 0x57e9b0].
	core_.tick(color_to_packed(p_fill_target), color_to_packed(p_sun_target),
			color_to_packed(p_fog_target), color_to_packed(p_sky_target),
			color_to_packed(p_lightning_color), p_sky_speed);
}

void NovaWeatherCore::tick_cloud_scroll(float p_sky_speed) {
	core_.tick_cloud_scroll(p_sky_speed);
}

void NovaWeatherCore::set_wind_intensity(int p_value) {
	core_.set_wind_intensity(p_value);
}

int NovaWeatherCore::get_wind_intensity() const {
	return core_.oscillator.intensity;
}

void NovaWeatherCore::set_wind_duration_ticks(int p_ticks) {
	core_.set_wind_duration_ticks(p_ticks);
}

int NovaWeatherCore::get_wind_duration_ticks() const {
	return core_.wind_duration_ticks;
}

void NovaWeatherCore::trigger_lightning_short() {
	core_.lightning.trigger_short();
}

void NovaWeatherCore::trigger_lightning_long() {
	core_.lightning.trigger_long();
}

void NovaWeatherCore::set_exposure_from_iris(const Vector3 &p_light_dir, float p_iris_percent, float p_iris_center) {
	core_.set_exposure_from_outdoor_iris(p_light_dir.x, p_light_dir.y, p_light_dir.z,
			p_iris_percent, p_iris_center);
}

void NovaWeatherCore::set_exposure_from_iris_samples(const PackedInt32Array &p_samples,
		const Vector3 &p_light_dir, const Color &p_ceiling, const Color &p_floor,
		float p_iris_percent, float p_iris_center) {
	core_.set_exposure_from_iris_samples(p_samples.ptr(),
			static_cast<int>(p_samples.size()), color_to_rgb(p_ceiling),
			color_to_rgb(p_floor), p_light_dir.x, p_light_dir.y, p_light_dir.z,
			p_iris_percent, p_iris_center);
}

Vector3 NovaWeatherCore::get_color_src_gain() const {
	const std::array<float, 3> scale =
			renderer::unpack_modulator_scale(core_.modulator_chain.render_color() & 0xFFFFFFu);
	return Vector3(scale[0], scale[1], scale[2]);
}

Color NovaWeatherCore::get_fill() const {
	return packed_to_color(core_.fill_block.render_color);
}

Color NovaWeatherCore::get_sun() const {
	return packed_to_color(core_.sun_block.render_color);
}

Color NovaWeatherCore::get_fog() const {
	// Fog color blocks operate in authored half-intensity bytes; the derived
	// render color doubles only after smoothing, lightning and modulation.
	return rgb_to_color(opennova::env::double_saturate(
			packed_to_rgb01(core_.fog_block.render_color)));
}

Color NovaWeatherCore::get_sky() const {
	return packed_to_color(core_.sky_block.render_color);
}

Color NovaWeatherCore::get_skyfog() const {
	// Retail overwrites skyfog[0] with the horizon blend in undoubled space,
	// then applies the same saturating x2 as fog. The reimpl's modulate2x clear
	// and dome-fog paths consume this final color verbatim.
	const opennova::env::Rgb blended = opennova::env::horizon_blend_skyfog(
			packed_to_rgb01(core_.fog_block.render_color),
			packed_to_rgb01(core_.sky_color_blocks.skyfog.render_color),
			static_cast<uint32_t>(core_.scalar_channels.fog_dist_fp),
			1024u << 16);
	return rgb_to_color(opennova::env::double_saturate(blended));
}

Color NovaWeatherCore::get_ceiling() const {
	return packed_to_color(core_.sky_color_blocks.ceiling.render_color);
}

Color NovaWeatherCore::get_cloud() const {
	return packed_to_color(core_.sky_color_blocks.cloud.render_color);
}

Color NovaWeatherCore::get_floor() const {
	return packed_to_color(core_.sky_color_blocks.floor.render_color);
}

Color NovaWeatherCore::get_ceiling_pre_mod() const {
	return packed_to_color(core_.sky_color_blocks.ceiling.pre_mod_color);
}

Color NovaWeatherCore::get_floor_pre_mod() const {
	return packed_to_color(core_.sky_color_blocks.floor.pre_mod_color);
}

Color NovaWeatherCore::get_skybase() const {
	return packed_to_color(core_.sky_color_blocks.skybase.render_color);
}

Color NovaWeatherCore::get_skybright() const {
	return packed_to_color(core_.sky_color_blocks.skybright.render_color);
}

Color NovaWeatherCore::get_skyhighlight() const {
	return packed_to_color(core_.sky_color_blocks.skyhighlight.render_color);
}

Color NovaWeatherCore::get_cloudbase() const {
	return packed_to_color(core_.sky_color_blocks.cloudbase.render_color);
}

Color NovaWeatherCore::get_cloudhighlight() const {
	return packed_to_color(core_.sky_color_blocks.cloudhighlight.render_color);
}

Color NovaWeatherCore::get_cloudedge() const {
	return packed_to_color(core_.sky_color_blocks.cloudedge.render_color);
}

float NovaWeatherCore::get_sway_amount() const {
	return static_cast<float>(core_.oscillator.smoothed - 0x8000) / 32768.0f * 2.0f;
}

float NovaWeatherCore::get_sway_phase() const {
	return static_cast<float>(core_.oscillator.ring_index) * (Math_TAU / 256.0f);
}

float NovaWeatherCore::get_lightning_intensity() const {
	return static_cast<float>(core_.lightning.level) / 255.0f;
}

Vector2 NovaWeatherCore::get_cloud_uv_offset1(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			opennova::env::cloud_scroll_uv_offsets(core_.cloud_scroll, p_cam_x, p_cam_z);
	return Vector2(offsets.u1, offsets.v1);
}

Vector2 NovaWeatherCore::get_cloud_uv_offset2(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			opennova::env::cloud_scroll_uv_offsets(core_.cloud_scroll, p_cam_x, p_cam_z);
	return Vector2(offsets.u2, offsets.v2);
}

float NovaWeatherCore::get_cloud_uv_rate_per_second() const {
	return opennova::env::cloud_uv_rate_per_second(core_.cloud_scroll);
}

Vector4 NovaWeatherCore::get_water_uv_state(float p_cam_x, float p_cam_z, float p_fog_distance) const {
	const opennova::env::WaterUvState state =
			opennova::env::water_uv_state(core_.cloud_scroll, p_cam_x, p_cam_z, p_fog_distance);
	return Vector4(state.scale, state.bias, state.offset_u, state.offset_v);
}

void NovaWeatherCore::set_scalar_targets(float p_fog_distance, float p_sky_height) {
	// Target refresh [orig: Environment_SnapStateToTargets @0x57d1e0:
	// Env_FogDistTarget <- Env_FogLevelFixed, sky target <- Env_SkyHeightFixed].
	core_.scalar_channels.fog_dist_target_fp = static_cast<int32_t>(p_fog_distance * 65536.0f);
	core_.scalar_channels.sky_height_target_fp = static_cast<int32_t>(p_sky_height * 65536.0f);
}

void NovaWeatherCore::snap_scalar_currents_to_targets() {
	core_.scalar_channels.snap_currents_to_targets();
}

void NovaWeatherCore::apply_network_environment_sample(int p_fog_dist,
		int p_fog_accel, int p_rain_pct, int p_overcast) {
	core_.scalar_channels.apply_network_sample(
			static_cast<uint16_t>(std::clamp(p_fog_dist, 0, 0xFFFF)),
			static_cast<uint16_t>(std::clamp(p_fog_accel, 0, 0xFFFF)),
			static_cast<uint8_t>(std::clamp(p_rain_pct, 0, 0xFF)),
			static_cast<uint8_t>(std::clamp(p_overcast, 0, 0xFF)));
}

float NovaWeatherCore::get_fog_distance() const {
	return static_cast<float>(core_.scalar_channels.fog_dist_fp) / 65536.0f;
}

float NovaWeatherCore::get_sky_height() const {
	return static_cast<float>(core_.scalar_channels.sky_height_fp) / 65536.0f;
}

float NovaWeatherCore::get_sun_dim_pct() const {
	return static_cast<float>(core_.scalar_channels.sun_dim_fp) / 65536.0f;
}

float NovaWeatherCore::get_rain_pct() const {
	return static_cast<float>(core_.scalar_channels.rain_pct_fp) / 65536.0f;
}

int NovaWeatherCore::get_fog_accel_clamp_fixed() const {
	return core_.scalar_channels.fog_step_fp;
}

int NovaWeatherCore::get_rain_pct_fixed() const {
	return core_.scalar_channels.rain_pct_fp;
}

int NovaWeatherCore::get_overcast_blend_fixed() const {
	return core_.scalar_channels.overcast_fp;
}
