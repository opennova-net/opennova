#pragma once

#include <formats/wac/program.h>

#include <utility>

namespace godot {

// A compiled WAC script program: the holder the Simulation binding retains
// (Simulation::compile_and_set_wac adopts the registry-compiled program into
// one; its ClassDB row died with the ADR 0043 d10 sweep). Compilation itself
// is the engine's (runtime/wac compile_program, pinned by the
// wac_program_surface ctest); this class only carries the compiled program
// and its ok state.
class WacProgram {
	opennova::wac::Program program_;
	bool compiled_ = false;

public:
	bool is_ok() const;

	// Adopt an already-compiled program (Simulation's registry-aware compile
	// path); its diagnostics ride native_program().
	void adopt(opennova::wac::Program &&p_program) {
		program_ = std::move(p_program);
		compiled_ = true;
	}

	const opennova::wac::Program &native_program() const { return program_; }
};

} // namespace godot
