#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/hud/loading_screen.h>

namespace godot {

// The SP start-mission splash's input states (hud::StartMissionSplash carries the witness): the
// loading screen raises it, hands it each key and button press and steps it once per display
// frame with the live button mask, and performs the device work its answers name (the input
// callback off on the exit, the dismissal on the release frame).
class StartMissionSplash : public RefCounted {
	GDCLASS(StartMissionSplash, RefCounted)

	opennova::hud::StartMissionSplash state_;

protected:
	static void _bind_methods();

public:
	enum State {
		STATE_NONE = static_cast<int>(opennova::hud::StartMissionSplash::State::kNone),
		STATE_ACTIVE = static_cast<int>(opennova::hud::StartMissionSplash::State::kActive),
		STATE_CLOSING = static_cast<int>(opennova::hud::StartMissionSplash::State::kClosing),
	};

	State get_state() const { return static_cast<State>(state_.state()); }
	bool raise() { return state_.raise(); }
	bool exit(bool p_presents) { return state_.exit(p_presents); }
	bool frame(int64_t p_mouse_mask, bool p_presents) {
		return state_.frame(static_cast<uint32_t>(p_mouse_mask), p_presents);
	}
	bool draws_overlay() const { return state_.draws_overlay(); }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::StartMissionSplash::State);
