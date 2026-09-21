#include "hud/end_round_transition.h"

namespace godot {

int EndRoundTransition::step(bool p_header_known, bool p_board_known, int p_now_ms) {
	const opennova::hud::EndRoundTransitionStep s =
			state_.step(p_header_known, p_board_known, static_cast<uint32_t>(p_now_ms));
	int flags = 0;
	if (s.announced) flags |= STEP_ANNOUNCED;
	if (s.reset) flags |= STEP_RESET;
	if (s.pre_stat) flags |= STEP_PRE_STAT;
	if (s.open_stat) flags |= STEP_OPEN_STAT;
	return flags;
}

void EndRoundTransition::_bind_methods() {
	ClassDB::bind_method(D_METHOD("step", "header_known", "board_known", "now_ms"),
			&EndRoundTransition::step);
	ClassDB::bind_method(D_METHOD("reset"), &EndRoundTransition::reset);
	ClassDB::bind_method(D_METHOD("is_header_seen"), &EndRoundTransition::is_header_seen);
	ClassDB::bind_method(D_METHOD("is_stat_opened"), &EndRoundTransition::is_stat_opened);
	BIND_CONSTANT(STEP_ANNOUNCED);
	BIND_CONSTANT(STEP_RESET);
	BIND_CONSTANT(STEP_PRE_STAT);
	BIND_CONSTANT(STEP_OPEN_STAT);
}

} // namespace godot
