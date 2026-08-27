#include "wac_startup.h"

#include "environment_startup.h"

#include <runtime/mission/event_runtime.h>
#include <runtime/mission/mission_systems.h>
#include <runtime/wac/compiler.h>
#include <formats/wac/program.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <array>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace opennova::nw_server {
namespace {

bool read_source_file(const std::filesystem::path &path,
		std::string &source, std::string &error) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		error = "WAC resource '" + path.string() + "' could not be opened";
		return false;
	}
	std::ostringstream bytes;
	bytes << input.rdbuf();
	if (!input.eof() && input.fail()) {
		error = "WAC resource '" + path.string() + "' could not be read";
		return false;
	}
	source = bytes.str();
	return true;
}

std::string compile_error(const wac::Program &program) {
	std::string message = "WAC layers failed to compile cleanly (" +
			std::to_string(program.diagnostics.size()) + " diagnostic(s))";
	for (const wac::Diagnostic &diagnostic : program.diagnostics) {
		message += ": line " + std::to_string(diagnostic.line) +
				", column " + std::to_string(diagnostic.col) + ": " +
				diagnostic.message;
		break;
	}
	return message;
}

} // namespace

std::filesystem::path resolve_resource_root(
		const std::filesystem::path &mission_path,
		const std::filesystem::path &explicit_path) {
	if (!explicit_path.empty()) return explicit_path;
	return mission_path.parent_path();
}

WacLoadStatus load_wac_program(
		const std::filesystem::path &resource_root,
		const std::string &mission_basename,
		world::World &world,
		wac::WacSystem &wac_system,
		std::string &error) {
	const std::array<std::string, 3> layer_names = {
			"game.wac", "server.wac", mission_basename + ".wac"};
	std::vector<std::string> sources;
	sources.reserve(layer_names.size());
	for (const std::string &name : layer_names) {
		if (name == ".wac") continue;
		const std::filesystem::path path = resource_root / name;
		std::error_code exists_error;
		const bool exists = std::filesystem::exists(path, exists_error);
		if (exists_error) {
			error = "WAC resource '" + path.string() +
					"' could not be inspected: " + exists_error.message();
			return WacLoadStatus::Error;
		}
		if (!exists) continue;
		std::string source;
		if (!read_source_file(path, source, error))
			return WacLoadStatus::Error;
		sources.push_back(std::move(source));
	}
	if (sources.empty()) {
		error.clear();
		return WacLoadStatus::Absent;
	}

	wac::CompileEnv compile_env;
	compile_env.registry = &world.registry;
	wac::Program program = wac::compile_program(sources, compile_env);
	// The shared compiler intentionally labels recoverable/legacy syntax issues
	// as warnings so editor tooling can decompile a partial program. A golden
	// host cannot safely do that: running a partial script is a known wire-parity
	// failure, so every diagnostic is fatal at this boundary.
	if (!program.ok() || !program.diagnostics.empty()) {
		error = compile_error(program);
		return WacLoadStatus::Error;
	}
	wac_system.set_program(std::move(program));
	error.clear();
	return WacLoadStatus::Loaded;
}

bool initialize_mission_startup(
		const std::filesystem::path &resource_root,
		const std::string &mission_basename,
		const bms::File &mission_file,
		world::World &world,
		wac::WacSystem &wac_system,
		mission::BmsEventSystem &bms_system,
		world::AiSystem &ai,
		bool &wac_loaded,
		std::string &error) {
	wac_loaded = false;
	if (!world.network_env.valid) {
		error = "mission startup requires a complete T0 environment sample";
		return false;
	}
	const WacLoadStatus status = load_wac_program(
			resource_root, mission_basename, world, wac_system, error);
	if (status == WacLoadStatus::Error) return false;
	wac_loaded = status == WacLoadStatus::Loaded;

	bms_system.load(
			mission_file.events, mission_file.triggers, mission_file.actions);
	mission::register_mission_systems(
			world, wac_system, bms_system, ai);
	// PreMission BMS events settle before play; WAC explicitly skips this pass.
	world.run_logic_tick(
			/*is_authority=*/true,
			opennova::world::TickPhase::PreMission);
	if (wac_loaded && wac_system.vm().loaded() &&
			!wac_system.execute_initial(world)) {
		error = "loaded WAC program refused its mission-start execution";
		return false;
	}

	world.network_env.initialize_mission_start();
	prewarm_initial_environment(world.network_env);
	error.clear();
	return true;
}

} // namespace opennova::nw_server
