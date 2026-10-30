#pragma once

#include <cstdint>

// The raw input a platform backend's input device produces (ADR 0077 d2: the engine's
// platform API, backend-neutral): what the device saw, in the engine's own codes, with no
// event semantics. Retail's semantics (its event numbers, the wheel's notch walk, the key
// queue's flag word, the input frame's record order) are the engine's: runtime/controls
// (device_messages.h) and inmatch::PlayerInputFrame.
//
// The design follows SDL3's, GLFW's and sokol_app's model: polled events (a key, a mouse
// button, the mouse's motion, the wheel), each delivered in the order the device saw it, plus a
// state snapshot read on demand (the cursor, the buttons held, the lock keys). A key event
// carries both a physical scancode and a layout key code, as SDL3's SDL_KeyboardEvent (scancode
// + key) and GLFW's key callback (key + scancode) do; both are engine enums, never a backend's
// own codes: a backend converts its events and polls into these. Mouse buttons, motion and the
// wheel are events, as in all three.
namespace opennova::platform {

// The layout's key code: the virtual-key numbering a key message's wParam carries, which
// retail's key state, key queue and binding records all key on (the engine's binding records
// store it: controls::BindingSet). The names below are the codes the engine names; every
// other value of the numbering (letters and digits are their ASCII capitals, 0x41..0x5A and
// 0x30..0x39) is a valid KeyCode too.
// [orig: Game_WindowProc @0x7624c0 - wParam @0x7624c8; Input_HandleKeyPress @0x760C90 - the key
//  state @0x760CBB; Input_QueueKeyEvent @0x760c10 - the VK @0x760c3c]
enum class KeyCode : uint16_t {
	None = 0,
	Backspace = 0x08,
	Tab = 0x09,
	Return = 0x0D,
	Shift = 0x10,
	Control = 0x11,
	Alt = 0x12,
	Pause = 0x13,
	CapsLock = 0x14,
	Escape = 0x1B,
	Space = 0x20,
	NumLock = 0x90,
	ScrollLock = 0x91,
};

// The physical key: the PC keyboard's set-1 make code a key message's lParam carries (bits
// 16..23), an extended key's (the E0 prefix, lParam bit 24) with kScancodeExtended added. The
// names below are the codes the engine names; every other set-1 code is a valid Scancode too.
// [orig: Input_QueueKeyEvent @0x760c10 - the extended bit @0x760c6b / 0x100 @0x760c6d]
inline constexpr uint16_t kScancodeExtended = 0x100;
enum class Scancode : uint16_t {
	Unknown = 0,
	Return = 0x1C,
	KeypadEnter = kScancodeExtended | 0x1C,
};

inline constexpr bool scancode_extended(Scancode scancode) {
	return (static_cast<uint16_t>(scancode) & kScancodeExtended) != 0;
}

// The modifiers a key event reports as the device saw them with it.
namespace key_mod {
inline constexpr uint8_t kShift = 0x1;
inline constexpr uint8_t kCtrl = 0x2;
inline constexpr uint8_t kAlt = 0x4;
} // namespace key_mod

// A key going down (an OS auto-repeat included, `repeat` set) or up.
struct KeyEvent {
	Scancode scancode = Scancode::Unknown;
	KeyCode key = KeyCode::None;
	bool down = false;
	bool repeat = false;
	uint8_t mods = 0;
	// The character the key typed (a Unicode code point), 0 for none.
	uint32_t text = 0;
};

enum class MouseButton : uint8_t { Left, Right, Middle, X1, X2 };

// A mouse button going down or up; `clicks` counts the press in a multi-click run (2 = a double
// click's second press, as SDL3's clicks and the window class's double-click message).
struct MouseButtonEvent {
	MouseButton button = MouseButton::Left;
	bool down = false;
	uint8_t clicks = 1;
};

// The mouse's relative motion since the previous motion event, in pixels (sub-pixel motion
// allowed).
struct MouseMotionEvent {
	float dx = 0.0f;
	float dy = 0.0f;
};

// One wheel turn, in notches (fractions from a high-resolution wheel), positive away from the
// user.
struct MouseWheelEvent {
	float y = 0.0f;
};

struct InputEvent {
	enum class Type : uint8_t { None, Key, MouseButton, MouseMotion, MouseWheel };
	Type type = Type::None;
	KeyEvent key;
	MouseButtonEvent button;
	MouseMotionEvent motion;
	MouseWheelEvent wheel;
};

// The state snapshot as the device reads it now.
struct InputState {
	// The cursor in the window's client pixels.
	int32_t cursor_x = 0;
	int32_t cursor_y = 0;
	// The cursor is captured (relative mode: hidden and held to the window).
	bool cursor_captured = false;
	// The buttons held, one bit per MouseButton (1 << button).
	uint8_t buttons = 0;
	// The lock keys' words as the keyboard state holds them: nonzero while the toggle is on or
	// the key is held (retail's GetKeyboardState byte).
	bool caps_lock = false;
	bool num_lock = false;
	bool scroll_lock = false;
};

} // namespace opennova::platform
