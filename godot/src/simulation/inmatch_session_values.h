#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/inmatch/session.h>

#include "hud/hud_toggles.h"

namespace godot {

// Typed Godot values for the portable in-match Session. They carry
// data only: the GameWorld leg table owns Godot device ordering and Simulation owns the
// conversion to/from the native session records.
class MissionFrameInput : public RefCounted {
	GDCLASS(MissionFrameInput, RefCounted)

public:
	enum HeldAction {
		HELD_FIRE = opennova::inmatch::HELD_FIRE,
		HELD_TO_SPECIAL = opennova::inmatch::HELD_TO_SPECIAL,
		// The menu-open bit the session reads (D-MNU-72).
		HELD_MENU_OPEN = opennova::inmatch::HELD_MENU_OPEN,
		HELD_FIRE_KEYS = opennova::inmatch::HELD_FIRE_KEYS,
	};
	enum PressedAction {
		PRESSED_FIRE = opennova::inmatch::PRESSED_FIRE,
		PRESSED_RELOAD = opennova::inmatch::PRESSED_RELOAD,
	};

private:
	opennova::inmatch::FrameInput value_;
	opennova::inmatch::TickInputSource *tick_source_ = nullptr;
	ObjectID tick_source_owner_;
	Ref<HudToggles> hud_toggles_;

protected:
	static void _bind_methods();

public:
	// The shell's input frame for this frame's updates (engine
	// inmatch::TickInputSource carries the witness), which `p_owner` (the
	// presenter whose input router samples the devices) owns: tick_source()
	// hands it to the session only while that object lives.
	void set_tick_source(opennova::inmatch::TickInputSource *p_source, ObjectID p_owner);
	opennova::inmatch::TickInputSource *tick_source() const;
	void set_delta_seconds(double p_delta);
	double get_delta_seconds() const;
	// The live frame's start stamp, the frame lock's (engine inmatch::FrameInput::
	// frame_stamp_ms); C++ only, the world's session leg sets it.
	void set_frame_stamp_ms(uint32_t p_stamp_ms) { value_.frame_stamp_ms = p_stamp_ms; }
	// Seconds since the previous frame's render finished (negative: unsampled).
	void set_since_render_seconds(double p_seconds);
	double get_since_render_seconds() const;
	// The command map is the open menu screen (engine inmatch::FrameInput::command_map_open).
	void set_command_map_open(bool p_open) { value_.command_map_open = p_open; }
	bool is_command_map_open() const { return value_.command_map_open; }
	void set_camera_sample(const Vector3 &p_position,
			const Vector3 &p_forward, bool p_listener_valid = true);
	// The camera sample as stamped: the driver pushes it to the present
	// passes' listener seam before it presents.
	Vector3 get_camera_position() const;
	bool is_listener_valid() const;
	void set_movement(bool p_forward, bool p_back, bool p_left, bool p_right,
			bool p_lean_left, bool p_lean_right, bool p_jump);
	void set_view_keys(bool p_free_look, bool p_up, bool p_down, bool p_left, bool p_right);
	// The mouse cursor's offset in whole pixels since the previous sample
	// (opennova::inmatch::InputPacket::cursor_dx / cursor_dy).
	void set_cursor_delta(const Vector2i &p_delta);
	// Axis input added at this input frame (the scripted device's look).
	void set_look_delta(const Vector2 &p_delta);
	void set_weapon_input(bool p_fire_held, bool p_fire_pressed, bool p_reload_pressed);
	// The ToSpecial keys' live state after set_weapon_input, which its records' dispatch
	// reads (opennova::inmatch::HELD_TO_SPECIAL).
	void set_to_special_held(bool p_held);
	// The fire row's key slots down after set_weapon_input (opennova::inmatch::HELD_FIRE_KEYS).
	void set_fire_keys_held(bool p_held);
	// A menu screen is up, under which no input frame runs
	// (opennova::inmatch::HELD_MENU_OPEN).
	void set_menu_open(bool p_open);
	void set_sequence(int64_t p_sequence);
	int64_t get_sequence() const;

	// The HUD toggles this frame's input rows drive (the shell's): the session reads their
	// status view after its drain, where retail's render callback reads its word (engine
	// inmatch::Session::draws_scene; FrameInput::hud_toggles). Null when the shell has none.
	void set_hud_toggles(const Ref<HudToggles> &p_toggles) { hud_toggles_ = p_toggles; }
	Ref<HudToggles> get_hud_toggles() const { return hud_toggles_; }

	const opennova::inmatch::FrameInput &native_value() const { return value_; }
};

// The per-tick outcome never crosses to GDScript: the C++ TickSink
// (simulation/tick_sink.h) takes the engine's TickOutcome directly.
class MissionFrameOutcome : public RefCounted {
	GDCLASS(MissionFrameOutcome, RefCounted)

public:
	enum Status {
		STATUS_OK = static_cast<int>(opennova::inmatch::FrameStatus::Ok),
		STATUS_NOT_RUNNING = static_cast<int>(opennova::inmatch::FrameStatus::NotRunning),
		STATUS_SESSION_LOST = static_cast<int>(opennova::inmatch::FrameStatus::SessionLost),
		STATUS_FATAL = static_cast<int>(opennova::inmatch::FrameStatus::Fatal),
	};
	enum State {
		STATE_UNLOADED = static_cast<int>(opennova::inmatch::State::Unloaded),
		STATE_CONNECTING = static_cast<int>(opennova::inmatch::State::Connecting),
		STATE_LOADING = static_cast<int>(opennova::inmatch::State::Loading),
		STATE_RUNNING = static_cast<int>(opennova::inmatch::State::Running),
		STATE_PAUSED = static_cast<int>(opennova::inmatch::State::Paused),
		STATE_STOPPING = static_cast<int>(opennova::inmatch::State::Stopping),
		STATE_FAILED = static_cast<int>(opennova::inmatch::State::Failed),
	};

private:
	int32_t status_ = STATUS_NOT_RUNNING;
	int32_t state_ = STATE_UNLOADED;
	// The catch-up ticks the frame ran; the per-tick outcomes themselves were
	// consumed synchronously by the TickSink inside the frame.
	int32_t ticks_run_ = 0;
	int64_t tick_us_ = 0;
	String error_;
	uint32_t frame_stamp_ms_ = 0;
	bool scene_drawn_ = true;

protected:
	static void _bind_methods();

public:
	int get_status() const { return status_; }
	int get_state() const { return state_; }
	int get_ticks_run() const { return ticks_run_; }
	int64_t get_tick_us() const { return tick_us_; }
	String get_error() const { return error_; }
	bool is_terminal() const;
	bool did_tick() const { return ticks_run_ > 0; }
	// The frame lock's last clock read when it slept, else 0 (engine inmatch::FrameOutcome::
	// frame_stamp_ms); C++ only.
	uint32_t get_frame_stamp_ms() const { return frame_stamp_ms_; }
	// The render callback's choice after the drain: the scene frame, or the status page in its
	// place (engine inmatch::Session::draws_scene); C++ only, GameWorld's scene gate reads it.
	bool is_scene_drawn() const { return scene_drawn_; }

	void assign(const opennova::inmatch::FrameOutcome &p_value);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MissionFrameInput::HeldAction)
VARIANT_ENUM_CAST(godot::MissionFrameInput::PressedAction)
VARIANT_ENUM_CAST(godot::MissionFrameOutcome::Status)
VARIANT_ENUM_CAST(godot::MissionFrameOutcome::State)
