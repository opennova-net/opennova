#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/wac/program.h>

#include <utility>

namespace godot {

class ResourceRoot;

// A compiled WAC script program: the C++ face of engine/runtime/wac's compiler
// for the Simulation binding (Simulation::set_wac_program holds one; its
// ClassDB row died with the ADR 0043 d10 sweep — the compile surface is
// pinned by the wac_program_surface ctest). Compilation here is registry-less
// (symbolic group/area names need a promoted world; numeric ids always
// resolve) — full-fidelity compilation against a live world goes through
// Simulation::compile_and_set_wac instead.
class WacProgram {
	opennova::wac::Program program_;
	bool compiled_ = false;

public:
	// Compile one source string. Returns OK when the program has no error
	// diagnostics; ERR_COMPILE_FAILED otherwise (diagnostics stay readable).
	Error compile_source(const String &p_source);
	// Compile several sources into ONE program, events numbered across all of
	// them — the original's game.wac -> server.wac -> <mission>.wac layering.
	Error compile_sources(const PackedStringArray &p_sources);
	// Resolve game.wac, server.wac and <mission_basename>.wac through the
	// mounted resource root, skipping absent files in the original order, and
	// compile what exists. [orig: WacScript_InitAndLoad, see docs/world/world-wac-ai-re.md] Returns
	// ERR_DOES_NOT_EXIST when none of the three is present (a BMS-only
	// mission — callers leave the VM unloaded).
	Error compile_from_resource_root(const Ref<ResourceRoot> &p_root, const String &p_mission_basename);

	bool is_ok() const;
	int get_error_count() const;
	// Array of { line, col, message, error } from parse + compile.
	Array get_diagnostics() const;
	int get_event_count() const;
	int get_code_size() const;

	// Adopt an already-compiled program (Simulation's registry-aware compile
	// path) so diagnostics surface through one class either way.
	void adopt(opennova::wac::Program &&p_program) {
		program_ = std::move(p_program);
		compiled_ = true;
	}

	const opennova::wac::Program &native_program() const { return program_; }
};

} // namespace godot
