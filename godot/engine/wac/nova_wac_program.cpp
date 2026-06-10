#include "nova_wac_program.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <wac/compiler.h>

#include <string>
#include <vector>

using namespace godot;

Error NovaWacProgram::compile_source(const String &p_source) {
	PackedStringArray sources;
	sources.push_back(p_source);
	return compile_sources(sources);
}

Error NovaWacProgram::compile_sources(const PackedStringArray &p_sources) {
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	// Registry-less: symbolic group/area names cannot resolve here (numeric ids
	// can); the live-world compile lives on NovaSimulation.compile_and_set_wac.
	opennova::wac::CompileEnv env;
	program_ = opennova::wac::compile_program(sources, env);
	compiled_ = true;
	return program_.ok() ? OK : ERR_COMPILATION_FAILED;
}

Error NovaWacProgram::compile_from_resource_root(const Ref<NovaResourceRoot> &p_root, const String &p_mission_basename) {
	ERR_FAIL_COND_V_MSG(p_root.is_null(), ERR_UNCONFIGURED, "NovaWacProgram needs a mounted resource root.");
	// The original layering, absent files skipped in order. [orig: WacScript_InitAndLoad]
	PackedStringArray names;
	names.push_back("game.wac");
	names.push_back("server.wac");
	if (!p_mission_basename.is_empty()) {
		names.push_back(p_mission_basename + String(".wac"));
	}
	PackedStringArray sources;
	for (int64_t i = 0; i < names.size(); ++i) {
		if (!p_root->has_file(names[i])) {
			continue;
		}
		const PackedByteArray bytes = p_root->read_file(names[i]);
		String text;
		// WAC sources are plain ASCII/latin text; parse permissively.
		text.parse_utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size());
		sources.push_back(text);
	}
	if (sources.is_empty()) {
		return ERR_DOES_NOT_EXIST; // BMS-only mission: nothing to install
	}
	return compile_sources(sources);
}

bool NovaWacProgram::is_compiled() const {
	return compiled_;
}

bool NovaWacProgram::is_ok() const {
	return compiled_ && program_.ok();
}

int NovaWacProgram::get_error_count() const {
	return program_.error_count();
}

Array NovaWacProgram::get_diagnostics() const {
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

int NovaWacProgram::get_event_count() const {
	return program_.event_count;
}

int NovaWacProgram::get_code_size() const {
	return static_cast<int>(program_.code.size());
}

void NovaWacProgram::_bind_methods() {
	ClassDB::bind_method(D_METHOD("compile_source", "source"), &NovaWacProgram::compile_source);
	ClassDB::bind_method(D_METHOD("compile_sources", "sources"), &NovaWacProgram::compile_sources);
	ClassDB::bind_method(D_METHOD("compile_from_resource_root", "root", "mission_basename"), &NovaWacProgram::compile_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_compiled"), &NovaWacProgram::is_compiled);
	ClassDB::bind_method(D_METHOD("is_ok"), &NovaWacProgram::is_ok);
	ClassDB::bind_method(D_METHOD("get_error_count"), &NovaWacProgram::get_error_count);
	ClassDB::bind_method(D_METHOD("get_diagnostics"), &NovaWacProgram::get_diagnostics);
	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaWacProgram::get_event_count);
	ClassDB::bind_method(D_METHOD("get_code_size"), &NovaWacProgram::get_code_size);
}
