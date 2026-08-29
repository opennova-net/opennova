#include "simulation/end_round_state.h"

#include <godot_cpp/variant/array.hpp>

namespace godot {

void EndRoundState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_header_known"), &EndRoundState::is_header_known);
	ClassDB::bind_method(D_METHOD("is_board_known"), &EndRoundState::is_board_known);
	ClassDB::bind_method(D_METHOD("get_game_type"), &EndRoundState::get_game_type);
	ClassDB::bind_method(D_METHOD("is_team_mode"), &EndRoundState::is_team_mode);
	ClassDB::bind_method(D_METHOD("is_session_open"), &EndRoundState::is_session_open);
	ClassDB::bind_method(D_METHOD("to_json_value"), &EndRoundState::to_json_value);
}

Dictionary EndRoundState::to_json_value() const {
	Dictionary out;
	out["header_known"] = value_.header_known;
	out["board_known"] = value_.board_known;
	out["game_type"] = static_cast<int64_t>(value_.game_type);
	out["winner"] = value_.winner;
	Array scores;
	scores.push_back(value_.team_score_0);
	scores.push_back(value_.team_score_1);
	out["team_scores"] = scores;
	out["draw"] = value_.draw;
	out["my_index"] = value_.my_index;
	out["round_ticks"] = value_.round_ticks;
	out["death_screen"] = value_.death_screen;
	out["local_team"] = value_.local_team;
	out["team_mode"] = value_.team_mode;
	out["session_open"] = value_.session_open;
	return out;
}

} // namespace godot
