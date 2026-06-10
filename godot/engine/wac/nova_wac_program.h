#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <wac/program.h>

#include <utility>

namespace godot {

class NovaResourceRoot;

// A compiled WAC script program: the GDExtension face of libs/wac's compiler,
// so the editor can lint/inspect mission scripts and the runtime can install
// them on the simulation (NovaSimulation.set_wac_program). Compilation here is
// registry-less (symbolic group/area names need a promoted world; numeric ids
// always resolve) — full-fidelity compilation against a live world goes
// through NovaSimulation.compile_and_set_wac instead.
class NovaWacProgram : public RefCounted {
	GDCLASS(NovaWacProgram, RefCounted)

	opennova::wac::Program program_;
	bool compiled_ = false;

protected:
	static void _bind_methods();

public:
	// Compile one source string. Returns OK when the program has no error
	// diagnostics; ERR_COMPILE_FAILED otherwise (diagnostics stay readable).
	Error compile_source(const String &p_source);
	// Compile several sources into ONE program, events numbered across all of
	// them — the original's game.wac -> server.wac -> <mission>.wac layering.
	Error compile_sources(const PackedStringArray &p_sources);
	// Resolve game.wac, server.wac and <mission_basename>.wac through the
	// mounted resource root, skipping absent files in the original order, and
	// compile what exists. [orig: WacScript_InitAndLoad] Returns
	// ERR_DOES_NOT_EXIST when none of the three is present (a BMS-only
	// mission — callers leave the VM unloaded).
	Error compile_from_resource_root(const Ref<NovaResourceRoot> &p_root, const String &p_mission_basename);

	bool is_compiled() const;
	bool is_ok() const;
	int get_error_count() const;
	// Array of { line, col, message, error } from parse + compile.
	Array get_diagnostics() const;
	int get_event_count() const;
	int get_code_size() const;

	// Adopt an already-compiled program (NovaSimulation's registry-aware compile
	// path) so diagnostics surface through one class either way. C++-only.
	void adopt(opennova::wac::Program &&p_program) {
		program_ = std::move(p_program);
		compiled_ = true;
	}

	const opennova::wac::Program &native_program() const { return program_; }
};

} // namespace godot
