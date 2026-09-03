#include "wac/wac_program.h"

#include "resource_index/resource_root.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_layered_load.h>
#include <runtime/wac/wac_system.h>

#include <string>
#include <vector>

using namespace godot;

Error WacProgram::compile_source(const String &p_source) {
	PackedStringArray sources;
	sources.push_back(p_source);
	return compile_sources(sources);
}

Error WacProgram::compile_sources(const PackedStringArray &p_sources) {
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	// Registry-less: symbolic group/area names cannot resolve here (numeric ids
	// can); the live-world compile lives on Simulation.compile_and_set_wac.
	opennova::wac::CompileEnv env;
	program_ = opennova::wac::compile_program(sources, env);
	compiled_ = true;
	return program_.ok() ? OK : ERR_COMPILATION_FAILED;
}

Error WacProgram::compile_from_resource_root(const Ref<ResourceRoot> &p_root, const String &p_mission_basename) {
	ERR_FAIL_COND_V_MSG(p_root.is_null(), ERR_UNCONFIGURED, "WacProgram needs a mounted resource root.");
	// The layered read + compile is the engine's one body (wac_layered_load,
	// lenient mode — the game's policy) [orig: WacScript_InitAndLoad]; this
	// binding adopts the compiled program off a scratch system so the holder
	// keeps its diagnostics surface.
	const opennova::ResourceIndex *index = &p_root->native_index();
	opennova::mission::BootFileSource files;
	files.has_file = [index](const std::string &name) {
		return index->has_file(name);
	};
	files.read_file = [index](const std::string &name, std::vector<uint8_t> &out) {
		return index->read_file(name, out);
	};
	opennova::wac::WacSystem system;
	std::string error;
	const opennova::wac::WacLayeredLoadStatus status =
			opennova::wac::wac_layered_load(system, files,
					std::string(p_mission_basename.utf8().get_data()),
					/*registry=*/nullptr, /*strict_diagnostics=*/false, error);
	switch (status) {
		case opennova::wac::WacLayeredLoadStatus::kAbsent:
			return ERR_DOES_NOT_EXIST; // BMS-only mission: nothing to install
		case opennova::wac::WacLayeredLoadStatus::kBlocked:
			compiled_ = false;
			return ERR_COMPILATION_FAILED;
		case opennova::wac::WacLayeredLoadStatus::kLoaded:
			break;
	}
	adopt(opennova::wac::Program(system.program()));
	return OK;
}

bool WacProgram::is_ok() const {
	return compiled_ && program_.ok();
}

int WacProgram::get_error_count() const {
	return program_.error_count();
}

Array WacProgram::get_diagnostics() const {
	Array out;
	for (const opennova::wac::Diagnostic &d : program_.diagnostics) {
		Dictionary entry;
		entry["line"] = d.line;
		entry["col"] = d.col;
		entry["message"] = String::utf8(d.message.c_str(), static_cast<int>(d.message.length()));
		entry["error"] = d.error;
		out.push_back(entry);
	}
	return out;
}

int WacProgram::get_event_count() const {
	return program_.event_count;
}

int WacProgram::get_code_size() const {
	return static_cast<int>(program_.code.size());
}
