#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <base/platform/input_sample.h>

// The Godot backend's input device: Godot's input events and polls converted into the engine's
// raw input sample (engine base/platform/input_sample.h: the polled events and the state
// snapshot, in the engine's own codes). Nothing here decides what an event means; the engine's
// input frame does (inmatch::PlayerInputFrame, controls/device_messages.h).
namespace godot::input_device {

// One Godot event as the engine's raw device event: a key, a mouse button, a wheel turn (a
// pressed wheel button, `factor` notches, one when the platform reports none), the mouse's
// motion. False for any other event (a wheel button's release, the horizontal wheel, a joypad
// event).
bool to_platform(const Ref<InputEvent> &p_event, opennova::platform::InputEvent &r_event);

// A Godot key event as the engine's key event: its layout key code (the virtual-key numbering;
// keypad Enter is VK_RETURN, told apart by its scancode) and its physical scancode (set-1).
opennova::platform::KeyEvent key_event(const InputEventKey &p_key);

// The state snapshot as Godot reads it now: the cursor in the window's client pixels and its
// capture, the buttons held, the lock keys' keyboard-state words.
opennova::platform::InputState state();

} // namespace godot::input_device
