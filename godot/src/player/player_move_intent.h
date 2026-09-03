#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// One frame of movement intent for the local player's input router (ADR 0043
// slice G8): the four movement keys, the lean pair and jump -- the same seven
// bits a device produces from the live binding table, so a scripted source
// (probes, automation, the presenter test) hands the sim one contract through
// LocalPlayerPresenter.set_input_override. u/d ride the LEAN keys: retail
// packs lean_left/lean_right as MoveOrder bits 0x40/0x80, and the aircraft
// mover reads those same two bits as descend/ascend -- the collective is the
// lean pair, overloaded. Every field defaults to released (the neutral frame).
#define PLAYER_MOVE_INTENT_FIELDS(X) \
	X(forward)                       \
	X(back)                          \
	X(left)                          \
	X(right)                         \
	X(lean_left)                     \
	X(lean_right)                    \
	X(jump)

class PlayerMoveIntent : public RefCounted {
	GDCLASS(PlayerMoveIntent, RefCounted)

public:
#define PLAYER_MOVE_INTENT_ACCESSORS(m_name)              \
	bool get_##m_name() const { return m_name##_; }       \
	void set_##m_name(bool p_value) { m_name##_ = p_value; }
	PLAYER_MOVE_INTENT_FIELDS(PLAYER_MOVE_INTENT_ACCESSORS)
#undef PLAYER_MOVE_INTENT_ACCESSORS

protected:
	static void _bind_methods();

private:
#define PLAYER_MOVE_INTENT_MEMBER(m_name) bool m_name##_ = false;
	PLAYER_MOVE_INTENT_FIELDS(PLAYER_MOVE_INTENT_MEMBER)
#undef PLAYER_MOVE_INTENT_MEMBER
};

} // namespace godot
