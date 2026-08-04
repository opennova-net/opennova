#pragma once

#include <mission/bms.h>
#include <world/world.h>

#include <filesystem>
#include <istream>
#include <string>

namespace opennova::nw_server {

// Resolve the BMS environment reference beside the loose mission by default.
// NW_ENV is passed as explicit_path by the app when resources live elsewhere.
std::filesystem::path resolve_environment_path(
		const std::filesystem::path &mission_path,
		const std::string &environment_name,
		const std::filesystem::path &explicit_path);

// Parse one actual ENV resource and publish the complete retail-native network
// sample. This stays independent of socket/session startup so focused runtime
// harnesses do not need a synthetic environment.
bool publish_initial_environment(
		std::istream &input,
		const bms::Header &header,
		world::EnvNetworkState &state,
		std::string &error);

bool publish_initial_environment_file(
		const std::filesystem::path &path,
		const bms::Header &header,
		world::EnvNetworkState &state,
		std::string &error);

// Retail settles mission-start environment state through 255 complete weather
// ticks before the server can publish phase 2.
void prewarm_initial_environment(world::EnvNetworkState &state) noexcept;

} // namespace opennova::nw_server
