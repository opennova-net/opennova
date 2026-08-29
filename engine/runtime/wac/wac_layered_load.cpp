#include <runtime/wac/wac_layered_load.h>

#include <formats/wac/program.h>
#include <runtime/wac/compiler.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::wac {

WacLayeredLoadStatus wac_layered_load(WacSystem &system,
		const mission::BootFileSource &files,
		const std::string &mission_basename, world::EntityRegistry *registry,
		bool strict_diagnostics, std::string &error) {
	error.clear();
	if (!files.valid()) return WacLayeredLoadStatus::kAbsent;
	// The original layering, absent files skipped in order
	// [orig: WacScript_InitAndLoad @0x4f91f0].
	std::vector<std::string> sources;
	for (const std::string &name : {std::string("game.wac"),
				 std::string("server.wac"), mission_basename + ".wac"}) {
		if (name == ".wac") continue; // no mission basename authored
		std::vector<uint8_t> bytes;
		if (!files.has_file(name) || !files.read_file(name, bytes)) continue;
		sources.emplace_back(bytes.begin(), bytes.end());
	}
	if (sources.empty()) return WacLayeredLoadStatus::kAbsent;
	CompileEnv env;
	env.registry = registry;
	Program program = compile_program(sources, env);
	// ok() is false only when a diagnostic carries error=true, so the strict
	// arm's "every diagnostic is fatal" test subsumes it.
	const bool blocked =
			strict_diagnostics ? !program.diagnostics.empty() : !program.ok();
	if (blocked) {
		error = "WAC for " + mission_basename + " failed to compile cleanly (" +
				std::to_string(program.error_count()) + " error(s), " +
				std::to_string(program.diagnostics.size()) + " diagnostic(s))";
		for (const Diagnostic &diagnostic : program.diagnostics) {
			error += ": line " + std::to_string(diagnostic.line) + ", column " +
					std::to_string(diagnostic.col) + ": " + diagnostic.message;
			break;
		}
		return WacLayeredLoadStatus::kBlocked;
	}
	system.set_program(std::move(program));
	return WacLayeredLoadStatus::kLoaded;
}

} // namespace opennova::wac
