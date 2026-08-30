#include <runtime/environment/sky_frame.h>

#include <algorithm>
#include <cmath>

namespace opennova::env {

namespace {

// The retail 2/255 unclamped upload scale for the six packed dome constants
// [orig: Color_UnpackToFloat4 @ 0x578985].
Rgb sky_constant(const Rgb &value) {
	return Rgb{value.r * 2.0f, value.g * 2.0f, value.b * 2.0f};
}

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
		frame.sky_base = sky_constant(env.sky_base());
		frame.sky_bright = sky_constant(env.sky_bright());
		frame.sky_highlight = sky_constant(env.sky_highlight());
		frame.cloud_base = sky_constant(env.cloud_base());
		frame.cloud_highlight = sky_constant(env.cloud_highlight());
		frame.cloud_edge = sky_constant(env.cloud_edge());
	}
	frame.sun_dir = env.sun_direction();
	frame.light_dir = env.light_direction();
	frame.skyfog_color = env.skyfog_color();
	// The dome's fog constant c9.x is the RAW smoothed fog distance
	// (Env_FogDistCurrent x 0.9 / 65536; the shader applies the 0.9), never
	// Environment_GetFogEndDistance's overcast-scaled end that the object and
	// terrain passes fog with: the dome rim keeps converging on the frame
	// clear's skyfog at the authored distance while overcast pulls the world
	// fog in. [orig: render_skybox @ 0x5792c2 (Env_FogDistCurrent *
	// 0.00001373291 into c9.x, uploaded @ 0x579751 / @ 0x579991);
	// Environment_GetFogEndDistance @ 0x57e40e..0x57e435 is the world end]
	frame.fog_end = env.fog_level();
	frame.sky_speed = env.sky_speed();
	frame.sky_height = env.sky_height();
	frame.frame_clear = env.frame_clear_color();
	return frame;
}

void ScrollFallback::advance(double delta, float sky_speed) {
	tick_credit += std::max(delta, 0.0) *
			static_cast<double>(WeatherRuntime::kWeatherTickHz);
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
