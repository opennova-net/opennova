#include "wac/wac_program.h"

using namespace godot;

bool WacProgram::is_ok() const {
	return compiled_ && program_.ok();
}
