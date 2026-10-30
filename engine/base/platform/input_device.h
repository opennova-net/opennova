#pragma once

#include <base/platform/input_sample.h>

#include <cstdint>

// The keyboard and mouse a platform backend reads (ADR 0077 d2: the engine's platform API,
// backend-neutral): the raw input sample (input_sample.h) as polled events and a state snapshot,
// and the cursor's mode and position. Nothing here decides what an event means: retail's event
// numbers, the wheel's notch walk, the key queue and the input frame are the engine's
// (runtime/controls, inmatch::PlayerInputFrame).
//
// The design follows SDL3's, GLFW's and sokol_app's: the backend's pump (Clock::pump_events)
// collects the OS's events, and the engine takes them oldest first, as SDL_PollEvent does (GLFW's
// callbacks and sokol_app's event callback deliver the same order during glfwPollEvents and the
// frame); the state is read on demand, as SDL_GetMouseState / glfwGetCursorPos are. The cursor
// mode is GLFW's (GLFW_CURSOR_NORMAL, GLFW_CURSOR_HIDDEN, GLFW_CURSOR_DISABLED) and SDL3's
// (SDL_ShowCursor / SDL_HideCursor, SDL_SetWindowRelativeMouseMode); the warp is
// SDL_WarpMouseInWindow / glfwSetCursorPos. The lock keys' set is SDL3's SDL_SetModState (the
// lock bits of the keyboard's modifier state) where the OS keeps no toggle a program may set, and
// the Win32 toggle (keybd_event / SendInput: the key's press and release put into the OS's input,
// as the keyboard's own would be) where it does; GLFW and sokol_app read the locks and set none.
namespace opennova::platform {

// The cursor over the window: shown; hidden over the window but free to leave it; or captured,
// the relative mode (hidden, held to the window, the motion events reporting the mouse's travel
// with no edge stopping it: InputState::cursor_captured).
enum class CursorMode : uint8_t { Visible, Hidden, Captured };

// The keyboard's three lock keys (InputState::caps_lock, num_lock, scroll_lock).
enum class LockKey : uint8_t { CapsLock, NumLock, ScrollLock };

class InputDevice {
public:
	virtual ~InputDevice() = default;

	// The oldest event the pump collected and the engine has not taken; false when none waits.
	// Events come in the order the device saw them, the focus changes among the keys.
	virtual bool poll_event(InputEvent &out) = 0;

	// The device's state as it stands now (the cursor in the window's client pixels, its capture,
	// the buttons held, the lock keys).
	virtual InputState state() const = 0;

	virtual void set_cursor_mode(CursorMode mode) = 0;

	// Moves the cursor to (`x`, `y`) in the window's client pixels; no motion event is reported
	// for the move itself.
	virtual void warp_cursor(int32_t x, int32_t y) = 0;

	// The lock key's toggle set to `on`, as a press and release of the key sets it, where its word
	// in the state (InputState's: the toggle on, or the key held) reads otherwise: the OS's own
	// toggle where the platform keeps one (the press and its release put into the OS's input, whose
	// key events then come through the pump as any key's do; the word reads the new toggle once the
	// pump has read them), else the device's own state. True when the device toggled it; false when
	// the word already read `on`, or the platform has no keyboard whose locks it may set (a run
	// with no display).
	virtual bool set_lock_key(LockKey key, bool on) = 0;
};

} // namespace opennova::platform
