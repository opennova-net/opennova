#include <runtime/environment/weather_seed.h>

#include <formats/env/tod_clock.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace opennova::env {
namespace {

int32_t fixed_q16(float value) noexcept {
	if (!std::isfinite(value)) return 0;
	const double scaled = static_cast<double>(value) * 65536.0;
	if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
		return std::numeric_limits<int32_t>::min();
	if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
		return std::numeric_limits<int32_t>::max();
	return static_cast<int32_t>(std::llround(scaled));
}

uint32_t nonnegative_scaled(float value, double scale) noexcept {
	if (!std::isfinite(value) || value <= 0.0f) return 0;
	const double scaled = static_cast<double>(value) * scale;
	if (scaled >= static_cast<double>(std::numeric_limits<uint32_t>::max()))
		return std::numeric_limits<uint32_t>::max();
	return static_cast<uint32_t>(std::llround(scaled));
}

} // namespace

world::WeatherSeed weather_seed_from_config(const Config &config, const bms::Header &header) {
	world::WeatherSeed seed;
	seed.fog_level_q16 = fixed_q16(config.fog_level);
	seed.sky_height_q16 = fixed_q16(config.sky_height);
	seed.cloud_scroll_rate_target = nonnegative_scaled(config.sky_speed, 1024.0);
	// [orig: Game_StartMission @ 0x525371 — start_time << 16 into the 8.24
	// clock; Environment_SetTodAdvanceRate @ 0x57d170 with the 60-minute floor]
	seed.tod_fixed24 = static_cast<uint32_t>(tod_start_fixed24(header.start_time)) %
			world::WeatherState::kTodDayFixed24;
	seed.tod_advance_per_tick = static_cast<uint32_t>(
			tod_advance_per_tick(static_cast<int>(header.minutes_per_day)));
	seed.fog_type = config.fog_type;
	// g_EnvLightningColor is the parser's pack of the line [orig: TimeOfDay_ParseProperty
	// @ 0x57c844 <- Color_ScaleRGBAndPack @ 0x57f890].
	seed.lightning_color = scale_rgb_and_pack(config.lightning_rgb, config.envscale);
	seed.wind_scale = 256;
	seed.tod_keyframed = !config.keyframes.empty();
	return seed;
}

} // namespace opennova::env
