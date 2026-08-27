#include "environment_startup.h"

#include <formats/env/env.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>

namespace opennova::nw_server {
namespace {

constexpr uint32_t kTodDayFixed24 = 24u << 24;
constexpr uint32_t kTicksPerRealMinute = 60u * 62u;
constexpr uint32_t kMinimumMinutesPerDay = 60u;
constexpr uint32_t kMissionStartPrewarmTicks = 255u;

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

std::filesystem::path resolve_environment_path(
		const std::filesystem::path &mission_path,
		const std::string &environment_name,
		const std::filesystem::path &explicit_path) {
	if (!explicit_path.empty()) return explicit_path;
	return mission_path.parent_path() / (environment_name + ".env");
}

bool publish_initial_environment(
		std::istream &input,
		const bms::Header &header,
		world::EnvNetworkState &state,
		std::string &error) {
	env::Config config;
	if (!env::load_env(input, config, error)) return false;

	// Game_LoadTerrainDuringConnect mutates the parsed ENV with BMS overrides
	// before Game_StartMission snapshots its network-visible targets.
	env::BmsEnvOverrides overrides;
	overrides.has_fog_level = bms::has_flag(
			header.attrib_flags, bms::AttribFlags::FogDistanceOverrideEnable);
	overrides.fog_level = static_cast<float>(header.fog_override);
	env::apply_bms_overrides(config, overrides);

	world::EnvNetworkSample sample;
	sample.fog_target_q16 = fixed_q16(config.fog_level);
	sample.fog_current_q16 = sample.fog_target_q16;
	sample.fog_accel_clamp = 0x00FF0000u;
	sample.tod_fixed24 =
			(static_cast<uint32_t>(header.start_time) << 16) % kTodDayFixed24;
	const uint32_t minutes = std::max<uint32_t>(
			header.minutes_per_day, kMinimumMinutesPerDay);
	sample.tod_advance_per_tick = static_cast<uint32_t>(
			kTodDayFixed24 /
			(static_cast<uint64_t>(kTicksPerRealMinute) * minutes));
	sample.cloud_scroll_rate_target =
			nonnegative_scaled(config.sky_speed, 1024.0);
	sample.precipitation_kind =
			static_cast<uint32_t>(world::PrecipitationKind::Rain);
	state.publish_complete(sample);
	error.clear();
	return true;
}

bool publish_initial_environment_file(
		const std::filesystem::path &path,
		const bms::Header &header,
		world::EnvNetworkState &state,
		std::string &error) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		error = "environment resource '" + path.string() + "' could not be opened";
		return false;
	}
	std::string parse_error;
	if (!publish_initial_environment(input, header, state, parse_error)) {
		error = "failed to parse environment '" + path.string() + "': " + parse_error;
		return false;
	}
	error.clear();
	return true;
}

void prewarm_initial_environment(world::EnvNetworkState &state) noexcept {
	for (uint32_t tick = 0; tick < kMissionStartPrewarmTicks; ++tick)
		state.advance_tick();
}

} // namespace opennova::nw_server
