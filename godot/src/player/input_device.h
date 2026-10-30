#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <base/platform/input_device.h>

#include <deque>

// The Godot backend's input device (engine base/platform/input_device.h): Godot's input events and
// polls converted into the engine's raw input sample (the polled events and the state snapshot, in
// the engine's own codes), and the cursor's mode and warp. Godot hands its events over as it pumps
// them (push), and the engine takes them in that order (poll_event). Nothing here decides what an
// event means; the engine's input frame does (inmatch::PlayerInputFrame,
// controls/device_messages.h).
namespace godot {

class InputDevice final : public opennova::platform::InputDevice {
public:
	// One Godot event queued as the engine's raw device event: a key, a mouse button, a wheel turn
	// (a pressed wheel button, `factor` notches, one when the platform reports none), the mouse's
	// motion. False for any other event (a wheel button's release, the horizontal wheel, a joypad
	// event), which queues nothing.
	bool push(const Ref<InputEvent> &p_event);
	bool poll_event(opennova::platform::InputEvent &r_event) override;
	// The state snapshot as Godot reads it now: the cursor in the window's client pixels and its
	// capture, the buttons held, the lock keys' keyboard-state words.
	opennova::platform::InputState state() const override;
	void set_cursor_mode(opennova::platform::CursorMode p_mode) override;
	void warp_cursor(int32_t p_x, int32_t p_y) override;

	// A Godot key event as the engine's key event: its layout key code (the virtual-key numbering;
	// keypad Enter is VK_RETURN, told apart by its scancode) and its physical scancode (set-1).
	static opennova::platform::KeyEvent key_event(const InputEventKey &p_key);

private:
	std::deque<opennova::platform::InputEvent> events_;
};

} // namespace godot
