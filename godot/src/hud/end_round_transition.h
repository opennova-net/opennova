#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/hud/end_round_overlay.h>

namespace godot {

// The end-of-round STAT transition latch (hud::EndRoundTransition carries the
// witness): the presenter steps it once per HUD frame with the sim's header /
// board knowledge and the wall clock, and performs the device work the
// returned STEP_* bits name. Held across mission rebuilds like retail's
// once-only bytes; reset() is the mission (re)start.
class EndRoundTransition : public RefCounted {
	GDCLASS(EndRoundTransition, RefCounted)

	opennova::hud::EndRoundTransition state_;

protected:
	static void _bind_methods();

public:
	enum {
		STEP_ANNOUNCED = 1,
		STEP_RESET = 2,
		STEP_PRE_STAT = 4,
		STEP_OPEN_STAT = 8,
	};

	int step(bool p_header_known, bool p_board_known, int p_now_ms);
	void reset() { state_.reset(); }
	bool is_header_seen() const { return state_.header_seen; }
	bool is_stat_opened() const { return state_.stat_opened; }
};

} // namespace godot
