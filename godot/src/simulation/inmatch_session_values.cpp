#include "simulation/inmatch_session_values.h"

#include <algorithm>

namespace godot {

void MissionFrameInput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_delta_seconds", "delta"),
			&MissionFrameInput::set_delta_seconds);
	ClassDB::bind_method(D_METHOD("get_delta_seconds"),
			&MissionFrameInput::get_delta_seconds);
	ClassDB::bind_method(D_METHOD("set_camera_sample", "position", "forward",
			"listener_valid"), &MissionFrameInput::set_camera_sample,
			DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_camera_position"),
			&MissionFrameInput::get_camera_position);
	ClassDB::bind_method(D_METHOD("is_listener_valid"),
			&MissionFrameInput::is_listener_valid);
	ClassDB::bind_method(D_METHOD("set_movement", "forward", "back", "left",
			"right", "lean_left", "lean_right", "jump"),
			&MissionFrameInput::set_movement);
	ClassDB::bind_method(D_METHOD("set_look_delta", "delta"),
			&MissionFrameInput::set_look_delta);
	ClassDB::bind_method(D_METHOD("get_look_delta"),
			&MissionFrameInput::get_look_delta);
	ClassDB::bind_method(D_METHOD("set_weapon_input", "fire_held", "fire_pressed",
			"reload_pressed", "medic_pressed"), &MissionFrameInput::set_weapon_input,
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_sequence", "sequence"),
			&MissionFrameInput::set_sequence);
	ClassDB::bind_method(D_METHOD("get_sequence"),
			&MissionFrameInput::get_sequence);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "delta_seconds"),
			"set_delta_seconds", "get_delta_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "look_delta"),
			"set_look_delta", "get_look_delta");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sequence"),
			"set_sequence", "get_sequence");
}

void MissionFrameInput::set_delta_seconds(double p_delta) {
	value_.delta_seconds = std::max(0.0, p_delta);
}

double MissionFrameInput::get_delta_seconds() const {
	return value_.delta_seconds;
}

void MissionFrameInput::set_camera_sample(const Vector3 &p_position,
		const Vector3 &p_forward, bool p_listener_valid) {
	value_.camera.position[0] = p_position.x;
	value_.camera.position[1] = p_position.y;
	value_.camera.position[2] = p_position.z;
	value_.camera.forward[0] = p_forward.x;
	value_.camera.forward[1] = p_forward.y;
	value_.camera.forward[2] = p_forward.z;
	value_.camera.listener_valid = p_listener_valid && p_position.is_finite();
}

Vector3 MissionFrameInput::get_camera_position() const {
	return Vector3(value_.camera.position[0], value_.camera.position[1],
			value_.camera.position[2]);
}

bool MissionFrameInput::is_listener_valid() const {
	return value_.camera.listener_valid;
}

void MissionFrameInput::set_movement(bool p_forward, bool p_back, bool p_left,
		bool p_right, bool p_lean_left, bool p_lean_right, bool p_jump) {
	auto &movement = value_.player.movement;
	movement.forward = p_forward;
	movement.back = p_back;
	movement.left = p_left;
	movement.right = p_right;
	movement.lean_left = p_lean_left;
	movement.lean_right = p_lean_right;
	movement.jump = p_jump;
}

void MissionFrameInput::set_look_delta(const Vector2 &p_delta) {
	value_.player.look_delta_x = p_delta.x;
	value_.player.look_delta_y = p_delta.y;
}

Vector2 MissionFrameInput::get_look_delta() const {
	return Vector2(value_.player.look_delta_x, value_.player.look_delta_y);
}

void MissionFrameInput::set_weapon_input(bool p_fire_held,
		bool p_fire_pressed, bool p_reload_pressed, bool p_medic_pressed) {
	value_.player.held_action_bits = p_fire_held ? HELD_FIRE : 0u;
	value_.player.pressed_action_bits =
			(p_fire_pressed ? PRESSED_FIRE : 0u) |
			(p_reload_pressed ? PRESSED_RELOAD : 0u) |
			(p_medic_pressed ? PRESSED_MEDIC_REQUEST : 0u);
}

void MissionFrameInput::set_sequence(int64_t p_sequence) {
	value_.player.sequence = static_cast<uint64_t>(std::max<int64_t>(0, p_sequence));
}

int64_t MissionFrameInput::get_sequence() const {
	return static_cast<int64_t>(std::min<uint64_t>(value_.player.sequence,
			static_cast<uint64_t>(INT64_MAX)));
}

void MissionFrameOutcome::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &MissionFrameOutcome::get_status);
	ClassDB::bind_method(D_METHOD("get_state"), &MissionFrameOutcome::get_state);
	ClassDB::bind_method(D_METHOD("get_ticks_run"), &MissionFrameOutcome::get_ticks_run);
	ClassDB::bind_method(D_METHOD("get_tick_us"), &MissionFrameOutcome::get_tick_us);
	ClassDB::bind_method(D_METHOD("get_error"), &MissionFrameOutcome::get_error);
	ClassDB::bind_method(D_METHOD("is_terminal"), &MissionFrameOutcome::is_terminal);
	ClassDB::bind_method(D_METHOD("did_tick"), &MissionFrameOutcome::did_tick);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "status"), "", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "state"), "", "get_state");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ticks_run"), "", "get_ticks_run");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tick_us"), "", "get_tick_us");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error"), "", "get_error");
	BIND_ENUM_CONSTANT(STATE_UNLOADED);
	BIND_ENUM_CONSTANT(STATE_CONNECTING);
	BIND_ENUM_CONSTANT(STATE_LOADING);
	BIND_ENUM_CONSTANT(STATE_RUNNING);
	BIND_ENUM_CONSTANT(STATE_PAUSED);
	BIND_ENUM_CONSTANT(STATE_STOPPING);
	BIND_ENUM_CONSTANT(STATE_FAILED);
}

bool MissionFrameOutcome::is_terminal() const {
	return status_ == STATUS_SESSION_LOST || status_ == STATUS_FATAL;
}

void MissionFrameOutcome::assign(const opennova::inmatch::FrameOutcome &p_value) {
	status_ = static_cast<int32_t>(p_value.status);
	state_ = static_cast<int32_t>(p_value.state);
	ticks_run_ = static_cast<int32_t>(p_value.ticks.size());
	tick_us_ = p_value.perf.tick_us;
	error_ = String::utf8(p_value.error.message.c_str());
}

} // namespace godot
