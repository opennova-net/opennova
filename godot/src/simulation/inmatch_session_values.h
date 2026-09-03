#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/inmatch/session.h>

namespace godot {

// Typed Godot values for the portable in-match Session. They carry
// data only: GameFramePipeline owns Godot device ordering and Simulation owns the
// conversion to/from the native session records.
class MissionFrameInput : public RefCounted {
	GDCLASS(MissionFrameInput, RefCounted)

public:
	enum HeldAction {
		HELD_FIRE = opennova::inmatch::HELD_FIRE,
	};
	enum PressedAction {
		PRESSED_FIRE = opennova::inmatch::PRESSED_FIRE,
		PRESSED_RELOAD = opennova::inmatch::PRESSED_RELOAD,
		// The dead player's medic call edge (the MedicReq action row; retail
		// Input_HandleActionBinding case 217 @0x49b4b4).
		PRESSED_MEDIC_REQUEST = opennova::inmatch::PRESSED_MEDIC_REQUEST,
	};

private:
	opennova::inmatch::FrameInput value_;

protected:
	static void _bind_methods();

public:
	void set_delta_seconds(double p_delta);
	double get_delta_seconds() const;
	void set_camera_sample(const Vector3 &p_position,
			const Vector3 &p_forward, bool p_listener_valid = true);
	// The camera sample as stamped: the driver pushes it to the present
	// passes' listener seam before it presents.
	Vector3 get_camera_position() const;
	bool is_listener_valid() const;
	void set_movement(bool p_forward, bool p_back, bool p_left, bool p_right,
			bool p_lean_left, bool p_lean_right, bool p_jump);
	void set_look_delta(const Vector2 &p_delta);
	Vector2 get_look_delta() const;
	void set_weapon_input(bool p_fire_held, bool p_fire_pressed,
			bool p_reload_pressed, bool p_medic_pressed = false);
	void set_sequence(int64_t p_sequence);
	int64_t get_sequence() const;

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

	void assign(const opennova::inmatch::FrameOutcome &p_value);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MissionFrameInput::HeldAction)
VARIANT_ENUM_CAST(godot::MissionFrameInput::PressedAction)
VARIANT_ENUM_CAST(godot::MissionFrameOutcome::Status)
VARIANT_ENUM_CAST(godot::MissionFrameOutcome::State)
