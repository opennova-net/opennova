#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <env/env_weather_core.h>

namespace godot {

// Godot boxing over env::WeatherCore — the [orig: Environment_UpdateWeatherTick
// @ 0x57e9b0] weather state cluster (wind oscillator, lightning sequencers,
// rain fade, the fourteen color blocks, the iris modulator chain, cloud
// scroll, scalar springs). ALL math and the witnessed order-of-operations live
// in engine/formats/env (env/env_weather_core.h); this binding converts Godot
// Colors/vectors at the boundary. NovaWeather (the node) drives one tick() per
// 62 Hz frame and reads the results back. RE record: docs/env/env-tod-re.md.
class NovaWeatherCore : public RefCounted {
	GDCLASS(NovaWeatherCore, RefCounted)

private:
	opennova::env::WeatherCore core_;

protected:
	static void _bind_methods();

public:
	// Snaps the four world-lighting block states (and their packed colors) to
	// the given colors — the discrete-scrub resync path. The ten sky-pass
	// blocks use snap_sky_colors() below.
	void snap_colors(const Color &p_fill, const Color &p_sun, const Color &p_fog, const Color &p_sky);

	void snap_sky_colors(const Color &p_skyfog, const Color &p_ceiling,
			const Color &p_cloud, const Color &p_floor, const Color &p_skybase,
			const Color &p_skybright, const Color &p_skyhighlight,
			const Color &p_cloudbase, const Color &p_cloudhighlight,
			const Color &p_cloudedge);
	void set_sky_color_targets(const Color &p_skyfog, const Color &p_ceiling,
			const Color &p_cloud, const Color &p_floor, const Color &p_skybase,
			const Color &p_skybright, const Color &p_skyhighlight,
			const Color &p_cloudbase, const Color &p_cloudhighlight,
			const Color &p_cloudedge);

	// One 62 Hz weather tick in the witnessed order (env::WeatherCore::tick).
	void tick(const Color &p_fill_target, const Color &p_sun_target,
			const Color &p_fog_target, const Color &p_sky_target,
			const Color &p_lightning_color, float p_sky_speed);

	// The cloud-scroll sub-tick alone — for owners with no weather colors (the
	// standalone-sky fallback path). tick() calls this itself.
	void tick_cloud_scroll(float p_sky_speed);

	// Raw Env_WindScale units. The witnessed retail value is 256; the
	// oscillator's 15*prev feedback is stable only for intensity <= 273.
	// The armed-gust decay semantics are env::WeatherCore's (an OpenNova
	// authoring extension, noted in docs/env/env-tod-re.md).
	void set_wind_intensity(int p_value);
	int get_wind_intensity() const;
	void set_wind_duration_ticks(int p_ticks);
	int get_wind_duration_ticks() const;

	void trigger_lightning_short();
	void trigger_lightning_long();

	// The outdoor iris exposure sample (no-world editor-preview fallback for
	// the marched form below) — env::WeatherCore::set_exposure_from_outdoor_iris.
	void set_exposure_from_iris(const Vector3 &p_light_dir, float p_iris_percent, float p_iris_center);

	// Per-sample classification codes for set_exposure_from_iris_samples.
	// Outdoor samples carry their sun-occlusion level 0..8 directly.
	static constexpr int32_t kIrisSampleIndoor =
			opennova::env::WeatherCore::kIrisSampleIndoor;
	static constexpr int32_t kIrisSampleIndoorNoData =
			opennova::env::WeatherCore::kIrisSampleIndoorNoData;

	// The in-world marched exposure (D-RLIT-2's sampling geometry): the shell
	// supplies one classification per marched sample; the fold (indoor swap,
	// no-interior-data 255, sun level scale, integer average) is
	// env::WeatherCore::set_exposure_from_iris_samples's. An empty array falls
	// back to the outdoor sample above.
	void set_exposure_from_iris_samples(const PackedInt32Array &p_samples,
			const Vector3 &p_light_dir, const Color &p_ceiling, const Color &p_floor,
			float p_iris_percent, float p_iris_center);

	// The modulator's render color / 64 — ColorSrcGlobalGain and the
	// effects/foliage ambient scale [orig: Render_UnpackModulatorToLightScale
	// @ 0x58db30; EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0].
	Vector3 get_color_src_gain() const;

	Color get_fill() const;
	Color get_sun() const;
	Color get_fog() const;
	Color get_sky() const;
	Color get_skyfog() const;
	Color get_ceiling() const;
	Color get_cloud() const;
	Color get_floor() const;
	Color get_ceiling_pre_mod() const;
	Color get_floor_pre_mod() const;
	Color get_skybase() const;
	Color get_skybright() const;
	Color get_skyhighlight() const;
	Color get_cloudbase() const;
	Color get_cloudhighlight() const;
	Color get_cloudedge() const;

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
	// Local mission-start scalar current <- target copy [orig: sub_57F1E0].
	void snap_scalar_currents_to_targets();
	// Apply the scalar subset decoded from one S2C 0x0A phase-2 sample. Inputs
	// are the narrowed wire units; engine/formats/env owns the exact reconstruction.
	void apply_network_environment_sample(int p_fog_dist, int p_fog_accel,
			int p_rain_pct, int p_overcast);
	// The smoothed scalar currents (world units / percent).
	float get_fog_distance() const;
	float get_sky_height() const;
	float get_sun_dim_pct() const;
	float get_rain_pct() const;
	int get_fog_accel_clamp_fixed() const;
	int get_rain_pct_fixed() const;
	int get_overcast_blend_fixed() const;

	// The witnessed water UV transform (scale, bias, offset_u, offset_v):
	// scale/bias from the fog-distance INT part, offsets from the layer-1
	// cloud accumulators + 32x camera [orig: render_water_surface
	// @ 0x5c3348..0x5c33db].
	Vector4 get_water_uv_state(float p_cam_x, float p_cam_z, float p_fog_distance) const;
};

} // namespace godot
