#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <runtime/inmatch/stat_screen_feed.h> // EndRoundSessionState

#include <cstdint>

namespace godot {

// The end-of-round session facts the shell flow keys on (net-re 5.68): the
// S2C 0x1D header edge, the 0x56 board completion, the winner/score words,
// the round clock and the session-open bit. A value wrapper over the engine's
// EndRoundSessionState the role fills (Simulation::get_end_round_state; ADR
// 0043 d10); to_json_value() exists for the MCP boundary only.
class EndRoundState : public RefCounted {
	GDCLASS(EndRoundState, RefCounted)

	opennova::inmatch::EndRoundSessionState value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::EndRoundSessionState &p_value) { value_ = p_value; }

	bool is_header_known() const { return value_.header_known; }
	bool is_board_known() const { return value_.board_known; }
	int64_t get_game_type() const { return static_cast<int64_t>(value_.game_type); }
	bool is_team_mode() const { return value_.team_mode; }
	bool is_session_open() const { return value_.session_open; }

	// The MCP boundary conversion only: the docs/mcp.md key set.
	Dictionary to_json_value() const;
};

} // namespace godot
