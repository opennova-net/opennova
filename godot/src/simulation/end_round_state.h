#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>

namespace godot {

// The end-of-round session facts the shell flow keys on (net-re 5.68): the
// S2C 0x1D header edge, the 0x56 board completion, the winner/score words,
// the round clock and the session-open bit. A typed record assigned from the
// role's folded ClientEndRoundStats (Simulation::get_end_round_state);
// to_json_value() exists for the MCP boundary only.
class EndRoundState : public RefCounted {
	GDCLASS(EndRoundState, RefCounted)

public:
	struct Value {
		bool header_known = false;
		bool board_known = false;
		uint32_t game_type = 0;
		int winner = 0;
		int team_score_0 = 0;
		int team_score_1 = 0;
		bool draw = false;
		int my_index = 0;
		int round_ticks = 0;
		bool death_screen = false;
		int local_team = 0;
		bool team_mode = false;
		bool session_open = false;
	};

protected:
	static void _bind_methods();

private:
	Value value_;

public:
	void assign(const Value &p_value) { value_ = p_value; }

	bool is_header_known() const { return value_.header_known; }
	bool is_board_known() const { return value_.board_known; }
	int64_t get_game_type() const { return static_cast<int64_t>(value_.game_type); }
	bool is_team_mode() const { return value_.team_mode; }
	bool is_session_open() const { return value_.session_open; }

	// The MCP boundary conversion only: the legacy key set.
	Dictionary to_json_value() const;
};

} // namespace godot
