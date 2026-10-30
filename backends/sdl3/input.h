#pragma once

#include <base/platform/input_device.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>

// The keyboard and mouse over SDL3's events and states (platform/input_device.h): the event pump
// hands each SDL event to on_event, which converts a key, a mouse button, the mouse's motion, a
// wheel turn or a focus change into the engine's raw input sample (platform/input_sample.h) and
// queues it, oldest first, for poll_event. SDL's scancodes (USB HID usages) and key codes become
// the engine's set-1 scancodes and virtual-key codes here (key_event); nothing here decides what a
// key means. Backend internal.
namespace opennova::sdl3 {

class Window;

class InputDevice final : public platform::InputDevice {
public:
	explicit InputDevice(const Window &window) : window_(window) {}

	bool poll_event(platform::InputEvent &out) override;
	platform::InputState state() const override;
	void set_cursor_mode(platform::CursorMode mode) override;
	void warp_cursor(int32_t x, int32_t y) override;

	// One event of the pump.
	void on_event(const SDL_Event &event);

	// An SDL key event as the engine's: the set-1 scancode (an E0 key's with
	// platform::kScancodeExtended), the virtual-key code, the state and the modifiers. The text
	// comes on its own event (SDL_EVENT_TEXT_INPUT), which on_event attaches to its key-down.
	static platform::KeyEvent key_event(const SDL_KeyboardEvent &key);

private:
	const Window &window_;
	std::deque<platform::InputEvent> events_;
	// A warp's target while its own motion event is still to come (SDL reports the move as a
	// motion outside the relative mode); that event is dropped.
	bool warp_pending_ = false;
	float warp_x_ = 0.0f;
	float warp_y_ = 0.0f;
};

} // namespace opennova::sdl3
