#include <runtime/environment/sky_frame.h>
#include <base/io/tick_rate.h>

#include <algorithm>
#include <cmath>

namespace opennova::env {

namespace {

// The six packed dome constants unpack through one helper
// [orig: Color_UnpackToFloat4 @ 0x578900]. The plain leg is byte * 2/255,
// unclamped [orig: @ 0x578985..0x5789bb]. With NVG on in first person
// (g_NVGActive && g_CameraMode == 0 [orig: @ 0x578901..0x578911]) every
// channel dims to byte * a * 2/255 + b with f = (level + 1) * 0.2, a = 0.25 * f,
// b = f * 0.0015625 [orig: @ 0x578913..0x57897b]; alpha stays 1 either way.
Rgb sky_constant(const Rgb &value, bool nvg_view, int nvg_level) {
	if (!nvg_view) {
		return Rgb{value.r * 2.0f, value.g * 2.0f, value.b * 2.0f};
	}
	const double f = static_cast<double>(nvg_level + 1) * static_cast<double>(0.2f);
	const double a = static_cast<double>(0.25f) * f;
	const double b = f * static_cast<double>(0.0015625f);
	const auto dim = [a, b](float channel) {
		const double byte_value = static_cast<double>(channel) * 255.0;
		return static_cast<float>(byte_value * a * static_cast<double>(2.0f / 255.0f) + b);
	};
	return Rgb{dim(value.r), dim(value.g), dim(value.b)};
}

// The thermal dome: after the unpack the shader path overwrites sky base,
// bright and highlight with 1.0 and the three cloud blocks with 0.9
// [orig: Render_Skybox @ 0x579377..0x579447 (flt_7C459C = 0.9)], and the sky
// wrapper fogs toward 0x808080 instead of the skyfog block
// [orig: SkyDome_RenderWithSkyfog @ 0x579cbc].
constexpr float kThermalSkyWhite = 1.0f;
constexpr float kThermalCloudWhite = 0.9f;
constexpr Rgb kThermalSkyFog{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};

} // namespace

SkyFrameState build_sky_frame(const EnvironmentState &env) {
	SkyFrameState frame;
	frame.loaded = env.is_loaded();
	if (!frame.loaded) {
		return frame;
	}
	const Config *config = env.config();
	frame.flat_pass = config != nullptr && config->advanced_clouds == 0;
	if (frame.flat_pass) {
		frame.flat_color = env.cloud_tint();
	} else {
		const bool nvg = env.nvg_view_active();
		const int level = env.nvg_gain();
		frame.sky_base = sky_constant(env.sky_base(), nvg, level);
		frame.sky_bright = sky_constant(env.sky_bright(), nvg, level);
		frame.sky_highlight = sky_constant(env.sky_highlight(), nvg, level);
		frame.cloud_base = sky_constant(env.cloud_base(), nvg, level);
		frame.cloud_highlight = sky_constant(env.cloud_highlight(), nvg, level);
		frame.cloud_edge = sky_constant(env.cloud_edge(), nvg, level);
		if (env.thermal_view()) {
			frame.sky_base = frame.sky_bright = frame.sky_highlight =
					Rgb{kThermalSkyWhite, kThermalSkyWhite, kThermalSkyWhite};
			frame.cloud_base = frame.cloud_highlight = frame.cloud_edge =
					Rgb{kThermalCloudWhite, kThermalCloudWhite, kThermalCloudWhite};
		}
	}
	frame.sun_dir = env.sun_direction();
	frame.light_dir = env.light_direction();
	// The main frame hands the thermal latch to the sky wrapper as its first
	// argument [orig: Render_ProcessMainSceneFrame @ 0x5ca363 (edi = the
	// thermal byte) -> SkyDome_RenderWithSkyfog @ 0x5ca81a].
	frame.skyfog_color = env.thermal_view() ? kThermalSkyFog : env.skyfog_color();
	// The dome's fog constant c9.x is the RAW smoothed fog distance
	// (g_EnvFogDistCurrent x 0.9 / 65536; the shader applies the 0.9), never
	// Environment_GetFogEndDistance's overcast-scaled end that the object and
	// terrain passes fog with: the dome rim keeps converging on the frame
	// clear's skyfog at the authored distance while overcast pulls the world
	// fog in. [orig: Render_Skybox @ 0x5792c2 (g_EnvFogDistCurrent *
	// 0.00001373291 into c9.x, uploaded @ 0x579751 / @ 0x579991);
	// Environment_GetFogEndDistance @ 0x57e40e..0x57e435 is the world end]
	frame.fog_end = env.fog_level();
	frame.sky_speed = env.sky_speed();
	frame.sky_height = env.sky_height();
	return frame;
}

void ScrollFallback::advance(double delta, float sky_speed) {
	tick_credit += std::max(delta, 0.0) *
			io::kTickHz;
	int tick_count = static_cast<int>(std::floor(tick_credit + 1.0e-9));
	if (tick_count > 0) {
		tick_credit = std::max(0.0, tick_credit - static_cast<double>(tick_count));
		if (tick_count > WeatherRuntime::kMaxCatchupTicks) {
			tick_count = WeatherRuntime::kMaxCatchupTicks;
			tick_credit = 0.0;
		}
		for (int i = 0; i < tick_count; ++i) {
			core.tick_cloud_scroll(sky_speed);
		}
	}
}

} // namespace opennova::env
