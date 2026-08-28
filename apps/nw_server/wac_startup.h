#pragma once

#include <filesystem>
#include <string>

namespace opennova::bms {
struct File;
}

namespace opennova::mission {
class BmsEventSystem;
}

namespace opennova::wac {
class WacSystem;
}

namespace opennova::world {
class AiSystem;
class World;
}

namespace opennova::nw_server {

// A loose headless mission treats its containing directory as the mounted
// resource root. --resource-root is passed as explicit_path when the mission
// was exported separately from the shared game/server script layers.
std::filesystem::path resolve_resource_root(
		const std::filesystem::path &mission_path,
		const std::filesystem::path &explicit_path);

enum class WacLoadStatus {
	Absent,
	Loaded,
	Error,
};

// Load the retail WacScript_InitAndLoad layers in their observable order:
// game.wac -> server.wac -> <mission-basename>.wac. Missing layers are skipped.
// Compilation is registry-aware because mission promotion has already populated
// the authoritative World by the time this is called.
WacLoadStatus load_wac_program(
		const std::filesystem::path &resource_root,
		const std::string &mission_basename,
		world::World &world,
		wac::WacSystem &wac,
		std::string &error);

// Complete the headless mission-start transaction after the ENV T0 sample and
// mission entities have been published. The systems remain owned by the caller
// and registered on World for the subsequent 62 Hz host loop.
//
// Retail order: load/register script systems, execute WAC once eagerly, perform
// environment mission-start initialization, then run 255 complete weather ticks
// before any client can observe phase 2. A present-but-invalid WAC is fatal;
// absent WAC layers are the valid BMS-only case.
bool initialize_mission_startup(
		const std::filesystem::path &resource_root,
		const std::string &mission_basename,
		const bms::File &mission_file,
		world::World &world,
		wac::WacSystem &wac,
		mission::BmsEventSystem &bms,
		world::AiSystem &ai,
		bool &wac_loaded,
		std::string &error);

} // namespace opennova::nw_server
