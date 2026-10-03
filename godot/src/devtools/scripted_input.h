#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <runtime/devtools/scripted_input.h>

#include <cstdint>

namespace godot {

// The scenario driver's scripted input device (engine
// devtools/scripted_input.h carries the schedule and its timing rules): a
// probe instrument, inert until a script is armed. Attached at the device
// seam (ControlsModel::set_scripted_input), its held tokens read held like
// keyboard slots; the input router steps it once per frame before its device
// sample and feeds its look pixels through the mouse-motion path, under the
// same gates the real mouse passes. Compiled in every flavour, as the
// debug-control automation rows are; only the source-only scenario_play probe
// arms it.
class ScriptedInput : public RefCounted {
	GDCLASS(ScriptedInput, RefCounted)

public:
	enum State {
		STATE_IDLE = static_cast<int>(opennova::devtools::ScriptedInput::State::Idle),
		STATE_ARMED = static_cast<int>(opennova::devtools::ScriptedInput::State::Armed),
		STATE_RUNNING = static_cast<int>(opennova::devtools::ScriptedInput::State::Running),
		STATE_FINISHED = static_cast<int>(opennova::devtools::ScriptedInput::State::Finished),
		STATE_CANCELLED = static_cast<int>(opennova::devtools::ScriptedInput::State::Cancelled),
	};

	// The shared script's "steps" array as JSON parses it: each step an
	// object with an integral "tick" and exactly one of "down" / "up" /
	// "press" (an integral action code), "look_px" ([dx, dy]) or "end"
	// (true); other keys are ignored. ERR_INVALID_PARAMETER with get_error()
	// naming the step when the shape or the engine's validation refuses it.
	Error load_steps(const Array &p_steps);
	String get_error() const { return error_; }
	bool start() { return driver_.start(); }
	void cancel() { driver_.cancel(); }
	// The frame's device sample at the session's logic tick.
	void advance(int64_t p_logic_tick) { driver_.advance(p_logic_tick); }
	bool is_token_held(const String &p_token) const;
	// The look pixels the applied steps fed since the last take.
	Vector2 take_look();

	int get_state() const { return static_cast<int>(driver_.state()); }
	String get_state_name() const;
	bool is_done() const;
	int64_t get_start_logic_tick() const { return driver_.start_logic_tick(); }
	int64_t get_end_logic_tick() const { return driver_.end_logic_tick(); }
	String get_cancel_reason() const;
	// Per source step (the script's own index): the logic tick it applied at
	// (-1 while pending), and a press's release tick (-1 otherwise).
	PackedInt64Array get_applied_logic_ticks() const;
	PackedInt64Array get_release_logic_ticks() const;
	PackedInt32Array get_held_codes() const;
	// The config token the catalog row dispatching `code` carries ("" when
	// no row does): forward 152 -> "move_forward", fire 149 -> "attack_1".
	static String token_for_action_code(int p_code);

	opennova::devtools::ScriptedInput &native() { return driver_; }
	const opennova::devtools::ScriptedInput &native() const { return driver_; }

protected:
	static void _bind_methods();

private:
	opennova::devtools::ScriptedInput driver_;
	String error_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::ScriptedInput::State);
