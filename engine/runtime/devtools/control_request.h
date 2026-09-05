// A typed debug-control invocation an F3 window queues for its embedder to
// drain into the ONE debug-control table F3 and MCP share (ADR 0043 d12,
// which folded ADR 0042 d6's per-window request enums into it): the row's
// wire id (debug_control_ids.h) plus the positional arguments the table
// marshals against that row's declared argument schema, exactly as an MCP
// `game_debug op=invoke` does. The window never mutates engine state itself
// and names no engine function: the table resolves the row's live owner,
// validates the arguments before any engine call and applies the row's own
// authority gate, so a joiner's requests are refused there, once, for both
// surfaces.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::devtools {

// One positional argument: the five value kinds the table's argument specs
// accept (DebugArgSpec: int, float, bool, string, vector3).
struct ControlArg {
	enum class Kind { Int, Float, Bool, Text, Vec3 };

	Kind kind = Kind::Int;
	int64_t i = 0;
	double f = 0.0;
	bool b = false;
	std::string text;
	float v[3] = {0.0f, 0.0f, 0.0f};

	static ControlArg integer(int64_t value) {
		ControlArg arg;
		arg.kind = Kind::Int;
		arg.i = value;
		return arg;
	}
	static ControlArg number(double value) {
		ControlArg arg;
		arg.kind = Kind::Float;
		arg.f = value;
		return arg;
	}
	static ControlArg boolean(bool value) {
		ControlArg arg;
		arg.kind = Kind::Bool;
		arg.b = value;
		return arg;
	}
	static ControlArg string(std::string value) {
		ControlArg arg;
		arg.kind = Kind::Text;
		arg.text = std::move(value);
		return arg;
	}
	static ControlArg vector3(float x, float y, float z) {
		ControlArg arg;
		arg.kind = Kind::Vec3;
		arg.v[0] = x;
		arg.v[1] = y;
		arg.v[2] = z;
		return arg;
	}
};

struct ControlRequest {
	// The row's wire id: one of the debug_control_ids.h constants (a string
	// literal with static storage; the table looks the row up by it).
	const char *id = "";
	std::vector<ControlArg> args;
};

}  // namespace opennova::devtools
