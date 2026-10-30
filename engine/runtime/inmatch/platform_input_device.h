#pragma once

#include <base/platform/input_device.h>
#include <base/platform/input_sample.h>
#include <runtime/controls/binding_set.h>
#include <runtime/controls/key_event_queue.h>
#include <runtime/controls/key_state_reset.h>
#include <runtime/inmatch/player_input_frame.h>

#include <array>
#include <cstdint>
#include <vector>

namespace opennova::inmatch {

// The local player's device over the platform's raw input (base/platform/input_sample.h: the
// polled events and the state snapshot a platform InputDevice reads) and the live binding table
// (controls::BindingSet): the PlayerInputDevice the input frame (PlayerInputFrame) samples. The
// embedder hands it every polled event in the order the device saw it (event) and the frame head's
// state snapshot (set_state); this device keeps what retail's window procedure keeps from them,
// the key state the binding reads walk (a 512-entry array by VK, an extended key's also at
// +0x100, so keypad Enter reads as 269) and the key queue the keyboard pass drains, and answers the
// binding table's reads over them. D-NET-563's Scroll Lock turn-off is the platform input device's
// lock-key set (platform::InputDevice::set_lock_key), which writes the OS's lock state where the
// platform keeps one. The scripted device and the joystick are not here: a platform that has them
// adds them.
//
// Its holder is the SDL3 app shell (apps/game, ADR 0077 d9) until the engine frontend (R3's
// runtime/frontend) takes the input frame over; the Godot build keeps its own device
// (godot/src/player/player_input_router.h) until R8.
class PlatformInputDevice final : public PlayerInputDevice {
public:
	// `input`: the platform's input device the lock-key set goes to (null: none, the turn-off
	// sends nothing).
	PlatformInputDevice(controls::BindingSet &bindings, platform::InputDevice *input) :
			bindings_(bindings), input_(input) {}

	// One polled event, in the device's order: a key's state, and a key-down's (an OS repeat
	// included) key queue entry with Ctrl and Shift as the key state then holds them; a focus
	// change clears the key state alone; a mouse button's held state.
	void event(const platform::InputEvent &event);
	// The state snapshot the frame's reads take (the cursor, its capture, the lock keys).
	void set_state(const platform::InputState &state) { state_ = state; }

	// The key queue the keyboard pass drains and its press latch.
	controls::KeyEventQueue &key_queue() { return queue_; }
	controls::KeyPressLatch &press_latch() { return latch_; }
	// The dispatch code of the row a dequeued key fires, -1 for none, and the fire's sign
	// (controls::BindingSet::event_row over the key state).
	int event_code(int scan_code, bool &sign) const;

	// Whether `vk` reads down in the key state (a menu close's reset honoured).
	bool key_down(int vk) const;

	// controls::PlayerActionSource
	bool pressed(const char *token) const override;
	int pressed_key(const char *token) const override;
	bool fire_signed(const char *token) const override;
	bool keys_down(const char *token) const override;
	bool binding_active(const char *token) const override;
	bool key_slots_down(const char *token) const override;
	bool held_off_keyboard(const char *token) const override;
	bool scripted_left_button(const char *token) const override;

	// PlayerInputDevice
	platform::InputState state() const override { return state_; }
	void set_scan_gates(bool death_screen, bool scan_word) override;
	bool keyboard_captured() const override;
	ClickRecord mouse_click_record(uint32_t event) const override;
	std::vector<controls::KeyRowEvent> joystick_records(bool death_screen, bool scroll_lock) override;
	std::vector<controls::KeyRowEvent> key_release_pass() override;
	void reset_key_queue() override;
	void reset_key_state() override;
	bool step_scripted(bool advance, int64_t logic_tick, float &look_x, float &look_y) override;
	bool movement_override(world::PlayerInput &movement) const override;
	bool scroll_lock_turn_off() override;

private:
	// The held mouse buttons as the binding table's mouse words (controls::kMouse*).
	uint16_t mouse_held() const;
	// A token's row read under the arms given: the keyboard's held pass, the mouse's held words,
	// or the fire arm's live test (BindingSet::binding_active).
	bool token_pressed(const char *token, bool keyboard, bool mouse, bool active) const;

	controls::BindingSet &bindings_;
	platform::InputDevice *input_ = nullptr;
	platform::InputState state_;
	std::array<bool, controls::KeyStateReset::kKeyCount> keys_{};
	mutable controls::KeyStateReset reset_;
	controls::KeyEventQueue queue_;
	controls::KeyPressLatch latch_;
	// One bit per platform::MouseButton, from the button events.
	uint8_t buttons_ = 0;
};

} // namespace opennova::inmatch
