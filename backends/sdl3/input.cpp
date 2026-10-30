#include "input.h"

#include "window.h"

#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace opennova::sdl3 {

namespace {

constexpr uint16_t kE0 = platform::kScancodeExtended;

// The PC keyboard's set-1 make codes by SDL scancode (a USB HID usage: the key's position on a US
// layout), the E0-prefixed keys with platform::kScancodeExtended; Pause is 0x45 and Num Lock
// E0 45, as the window procedure's lParam carries them.
struct ScancodeRow {
	SDL_Scancode sdl;
	uint16_t set1;
	// The key's virtual-key code where it names no character (0: the character keys, whose code
	// is the layout's, character_vk).
	uint16_t vk;
};

constexpr ScancodeRow kScancodes[] = {
	{ SDL_SCANCODE_A, 0x1E, 0 }, { SDL_SCANCODE_B, 0x30, 0 }, { SDL_SCANCODE_C, 0x2E, 0 },
	{ SDL_SCANCODE_D, 0x20, 0 }, { SDL_SCANCODE_E, 0x12, 0 }, { SDL_SCANCODE_F, 0x21, 0 },
	{ SDL_SCANCODE_G, 0x22, 0 }, { SDL_SCANCODE_H, 0x23, 0 }, { SDL_SCANCODE_I, 0x17, 0 },
	{ SDL_SCANCODE_J, 0x24, 0 }, { SDL_SCANCODE_K, 0x25, 0 }, { SDL_SCANCODE_L, 0x26, 0 },
	{ SDL_SCANCODE_M, 0x32, 0 }, { SDL_SCANCODE_N, 0x31, 0 }, { SDL_SCANCODE_O, 0x18, 0 },
	{ SDL_SCANCODE_P, 0x19, 0 }, { SDL_SCANCODE_Q, 0x10, 0 }, { SDL_SCANCODE_R, 0x13, 0 },
	{ SDL_SCANCODE_S, 0x1F, 0 }, { SDL_SCANCODE_T, 0x14, 0 }, { SDL_SCANCODE_U, 0x16, 0 },
	{ SDL_SCANCODE_V, 0x2F, 0 }, { SDL_SCANCODE_W, 0x11, 0 }, { SDL_SCANCODE_X, 0x2D, 0 },
	{ SDL_SCANCODE_Y, 0x15, 0 }, { SDL_SCANCODE_Z, 0x2C, 0 },
	{ SDL_SCANCODE_1, 0x02, 0 }, { SDL_SCANCODE_2, 0x03, 0 }, { SDL_SCANCODE_3, 0x04, 0 },
	{ SDL_SCANCODE_4, 0x05, 0 }, { SDL_SCANCODE_5, 0x06, 0 }, { SDL_SCANCODE_6, 0x07, 0 },
	{ SDL_SCANCODE_7, 0x08, 0 }, { SDL_SCANCODE_8, 0x09, 0 }, { SDL_SCANCODE_9, 0x0A, 0 },
	{ SDL_SCANCODE_0, 0x0B, 0 },
	{ SDL_SCANCODE_MINUS, 0x0C, 0 }, { SDL_SCANCODE_EQUALS, 0x0D, 0 },
	{ SDL_SCANCODE_LEFTBRACKET, 0x1A, 0 }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B, 0 },
	{ SDL_SCANCODE_BACKSLASH, 0x2B, 0 }, { SDL_SCANCODE_NONUSHASH, 0x2B, 0 },
	{ SDL_SCANCODE_SEMICOLON, 0x27, 0 }, { SDL_SCANCODE_APOSTROPHE, 0x28, 0 },
	{ SDL_SCANCODE_GRAVE, 0x29, 0 }, { SDL_SCANCODE_COMMA, 0x33, 0 },
	{ SDL_SCANCODE_PERIOD, 0x34, 0 }, { SDL_SCANCODE_SLASH, 0x35, 0 },
	{ SDL_SCANCODE_NONUSBACKSLASH, 0x56, 0 },
	{ SDL_SCANCODE_RETURN, 0x1C, 0x0D }, { SDL_SCANCODE_ESCAPE, 0x01, 0x1B },
	{ SDL_SCANCODE_BACKSPACE, 0x0E, 0x08 }, { SDL_SCANCODE_TAB, 0x0F, 0x09 },
	{ SDL_SCANCODE_SPACE, 0x39, 0x20 }, { SDL_SCANCODE_CAPSLOCK, 0x3A, 0x14 },
	{ SDL_SCANCODE_F1, 0x3B, 0x70 }, { SDL_SCANCODE_F2, 0x3C, 0x71 }, { SDL_SCANCODE_F3, 0x3D, 0x72 },
	{ SDL_SCANCODE_F4, 0x3E, 0x73 }, { SDL_SCANCODE_F5, 0x3F, 0x74 }, { SDL_SCANCODE_F6, 0x40, 0x75 },
	{ SDL_SCANCODE_F7, 0x41, 0x76 }, { SDL_SCANCODE_F8, 0x42, 0x77 }, { SDL_SCANCODE_F9, 0x43, 0x78 },
	{ SDL_SCANCODE_F10, 0x44, 0x79 }, { SDL_SCANCODE_F11, 0x57, 0x7A },
	{ SDL_SCANCODE_F12, 0x58, 0x7B },
	{ SDL_SCANCODE_PRINTSCREEN, kE0 | 0x37, 0x2C }, { SDL_SCANCODE_SCROLLLOCK, 0x46, 0x91 },
	{ SDL_SCANCODE_PAUSE, 0x45, 0x13 }, { SDL_SCANCODE_NUMLOCKCLEAR, kE0 | 0x45, 0x90 },
	{ SDL_SCANCODE_INSERT, kE0 | 0x52, 0x2D }, { SDL_SCANCODE_DELETE, kE0 | 0x53, 0x2E },
	{ SDL_SCANCODE_HOME, kE0 | 0x47, 0x24 }, { SDL_SCANCODE_END, kE0 | 0x4F, 0x23 },
	{ SDL_SCANCODE_PAGEUP, kE0 | 0x49, 0x21 }, { SDL_SCANCODE_PAGEDOWN, kE0 | 0x51, 0x22 },
	{ SDL_SCANCODE_LEFT, kE0 | 0x4B, 0x25 }, { SDL_SCANCODE_UP, kE0 | 0x48, 0x26 },
	{ SDL_SCANCODE_RIGHT, kE0 | 0x4D, 0x27 }, { SDL_SCANCODE_DOWN, kE0 | 0x50, 0x28 },
	// The keypad: Enter is VK_RETURN on its E0 scancode (the binding scan reads it as 269);
	// the digits and the point are keypad_vk's.
	{ SDL_SCANCODE_KP_DIVIDE, kE0 | 0x35, 0x6F }, { SDL_SCANCODE_KP_MULTIPLY, 0x37, 0x6A },
	{ SDL_SCANCODE_KP_MINUS, 0x4A, 0x6D }, { SDL_SCANCODE_KP_PLUS, 0x4E, 0x6B },
	{ SDL_SCANCODE_KP_ENTER, kE0 | 0x1C, 0x0D },
	{ SDL_SCANCODE_KP_7, 0x47, 0 }, { SDL_SCANCODE_KP_8, 0x48, 0 }, { SDL_SCANCODE_KP_9, 0x49, 0 },
	{ SDL_SCANCODE_KP_4, 0x4B, 0 }, { SDL_SCANCODE_KP_5, 0x4C, 0 }, { SDL_SCANCODE_KP_6, 0x4D, 0 },
	{ SDL_SCANCODE_KP_1, 0x4F, 0 }, { SDL_SCANCODE_KP_2, 0x50, 0 }, { SDL_SCANCODE_KP_3, 0x51, 0 },
	{ SDL_SCANCODE_KP_0, 0x52, 0 }, { SDL_SCANCODE_KP_PERIOD, 0x53, 0 },
	// The modifiers: one virtual key a side (VK_SHIFT, VK_CONTROL, VK_MENU, as a key message's
	// wParam carries them), the right Ctrl and Alt E0 keys.
	{ SDL_SCANCODE_LSHIFT, 0x2A, 0x10 }, { SDL_SCANCODE_RSHIFT, 0x36, 0x10 },
	{ SDL_SCANCODE_LCTRL, 0x1D, 0x11 }, { SDL_SCANCODE_RCTRL, kE0 | 0x1D, 0x11 },
	{ SDL_SCANCODE_LALT, 0x38, 0x12 }, { SDL_SCANCODE_RALT, kE0 | 0x38, 0x12 },
	{ SDL_SCANCODE_LGUI, kE0 | 0x5B, 0x5B }, { SDL_SCANCODE_RGUI, kE0 | 0x5C, 0x5C },
	{ SDL_SCANCODE_APPLICATION, kE0 | 0x5D, 0x5D },
};

const ScancodeRow *row_of(SDL_Scancode scancode) {
	for (const ScancodeRow &row : kScancodes)
		if (row.sdl == scancode) return &row;
	return nullptr;
}

// The virtual-key code a character key's layout gives it: a letter's capital, a digit's own, the
// punctuation keys' VK_OEM_* codes; 0 for a character with none (a key of another script, whose
// US position stands in: us_character_vk).
uint16_t character_vk(SDL_Keycode key) {
	if (key >= 'a' && key <= 'z') return static_cast<uint16_t>(key - 'a' + 'A');
	if (key >= '0' && key <= '9') return static_cast<uint16_t>(key);
	switch (key) {
		case ';': return 0xBA;
		case '=': return 0xBB;
		case ',': return 0xBC;
		case '-': return 0xBD;
		case '.': return 0xBE;
		case '/': return 0xBF;
		case '`': return 0xC0;
		case '[': return 0xDB;
		case '\\': return 0xDC;
		case ']': return 0xDD;
		case '\'': return 0xDE;
		default: return 0;
	}
}

// The character key's code at its US position (the layout gave it no code character_vk knows).
uint16_t us_character_vk(SDL_Scancode scancode) {
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		return static_cast<uint16_t>('A' + (scancode - SDL_SCANCODE_A));
	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
		return static_cast<uint16_t>('1' + (scancode - SDL_SCANCODE_1));
	switch (scancode) {
		case SDL_SCANCODE_0: return '0';
		case SDL_SCANCODE_SEMICOLON: return 0xBA;
		case SDL_SCANCODE_EQUALS: return 0xBB;
		case SDL_SCANCODE_COMMA: return 0xBC;
		case SDL_SCANCODE_MINUS: return 0xBD;
		case SDL_SCANCODE_PERIOD: return 0xBE;
		case SDL_SCANCODE_SLASH: return 0xBF;
		case SDL_SCANCODE_GRAVE: return 0xC0;
		case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
		case SDL_SCANCODE_BACKSLASH:
		case SDL_SCANCODE_NONUSHASH: return 0xDC;
		case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
		case SDL_SCANCODE_APOSTROPHE: return 0xDE;
		case SDL_SCANCODE_NONUSBACKSLASH: return 0xE2; // VK_OEM_102
		default: return 0;
	}
}

// A keypad digit's or point's code: VK_NUMPAD0..9 and VK_DECIMAL under Num Lock, else the
// navigation key it doubles as (on its own, non-E0 scancode), keypad 5 VK_CLEAR.
uint16_t keypad_vk(SDL_Scancode scancode, bool num_lock) {
	struct Pad {
		SDL_Scancode sdl;
		uint16_t numeric;
		uint16_t navigation;
	};
	static constexpr Pad kPad[] = {
		{ SDL_SCANCODE_KP_0, 0x60, 0x2D }, { SDL_SCANCODE_KP_1, 0x61, 0x23 },
		{ SDL_SCANCODE_KP_2, 0x62, 0x28 }, { SDL_SCANCODE_KP_3, 0x63, 0x22 },
		{ SDL_SCANCODE_KP_4, 0x64, 0x25 }, { SDL_SCANCODE_KP_5, 0x65, 0x0C },
		{ SDL_SCANCODE_KP_6, 0x66, 0x27 }, { SDL_SCANCODE_KP_7, 0x67, 0x24 },
		{ SDL_SCANCODE_KP_8, 0x68, 0x26 }, { SDL_SCANCODE_KP_9, 0x69, 0x21 },
		{ SDL_SCANCODE_KP_PERIOD, 0x6E, 0x2E },
	};
	for (const Pad &pad : kPad)
		if (pad.sdl == scancode) return num_lock ? pad.numeric : pad.navigation;
	return 0;
}

// The first code point of a UTF-8 string (0 for none or a malformed lead).
uint32_t first_code_point(const char *text) {
	const auto *s = reinterpret_cast<const unsigned char *>(text);
	if (s == nullptr || s[0] == 0) return 0;
	if (s[0] < 0x80) return s[0];
	int extra = 0;
	uint32_t cp = 0;
	if ((s[0] & 0xE0) == 0xC0) {
		extra = 1;
		cp = s[0] & 0x1Fu;
	} else if ((s[0] & 0xF0) == 0xE0) {
		extra = 2;
		cp = s[0] & 0x0Fu;
	} else if ((s[0] & 0xF8) == 0xF0) {
		extra = 3;
		cp = s[0] & 0x07u;
	} else {
		return 0;
	}
	for (int i = 1; i <= extra; ++i) {
		if ((s[i] & 0xC0) != 0x80) return 0;
		cp = (cp << 6) | (s[i] & 0x3Fu);
	}
	return cp;
}

} // namespace

platform::KeyEvent InputDevice::key_event(const SDL_KeyboardEvent &key) {
	platform::KeyEvent out;
	uint16_t vk = 0;
	if (const ScancodeRow *row = row_of(key.scancode)) {
		out.scancode = static_cast<platform::Scancode>(row->set1);
		vk = row->vk;
	}
	if (vk == 0) vk = keypad_vk(key.scancode, (key.mod & SDL_KMOD_NUM) != 0);
	if (vk == 0) {
		vk = character_vk(key.key);
		if (vk == 0) vk = us_character_vk(key.scancode);
	}
	out.key = static_cast<platform::KeyCode>(vk);
	out.down = key.down;
	out.repeat = key.repeat;
	if ((key.mod & SDL_KMOD_SHIFT) != 0) out.mods |= platform::key_mod::kShift;
	if ((key.mod & SDL_KMOD_CTRL) != 0) out.mods |= platform::key_mod::kCtrl;
	if ((key.mod & SDL_KMOD_ALT) != 0) out.mods |= platform::key_mod::kAlt;
	return out;
}

bool InputDevice::poll_event(platform::InputEvent &out) {
	if (events_.empty()) return false;
	out = events_.front();
	events_.pop_front();
	return true;
}

platform::InputState InputDevice::state() const {
	platform::InputState out;
	float x = 0.0f;
	float y = 0.0f;
	const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
	out.cursor_x = static_cast<int32_t>(x);
	out.cursor_y = static_cast<int32_t>(y);
	SDL_Window *window = window_.handle();
	out.cursor_captured = window != nullptr && SDL_GetWindowRelativeMouseMode(window);
	const struct {
		SDL_MouseButtonFlags sdl;
		platform::MouseButton engine;
	} kButtons[] = {
		{ SDL_BUTTON_LMASK, platform::MouseButton::Left },
		{ SDL_BUTTON_RMASK, platform::MouseButton::Right },
		{ SDL_BUTTON_MMASK, platform::MouseButton::Middle },
		{ SDL_BUTTON_X1MASK, platform::MouseButton::X1 },
		{ SDL_BUTTON_X2MASK, platform::MouseButton::X2 },
	};
	for (const auto &button : kButtons)
		if ((buttons & button.sdl) != 0)
			out.buttons |= static_cast<uint8_t>(1u << static_cast<unsigned>(button.engine));
	// A lock key's keyboard-state word: its toggle on, or the key held.
	const SDL_Keymod mods = SDL_GetModState();
	int count = 0;
	const bool *held = SDL_GetKeyboardState(&count);
	const auto down = [held, count](SDL_Scancode scancode) {
		return held != nullptr && static_cast<int>(scancode) < count && held[scancode];
	};
	out.caps_lock = (mods & SDL_KMOD_CAPS) != 0 || down(SDL_SCANCODE_CAPSLOCK);
	out.num_lock = (mods & SDL_KMOD_NUM) != 0 || down(SDL_SCANCODE_NUMLOCKCLEAR);
	out.scroll_lock = (mods & SDL_KMOD_SCROLL) != 0 || down(SDL_SCANCODE_SCROLLLOCK);
	return out;
}

void InputDevice::set_cursor_mode(platform::CursorMode mode) {
	SDL_Window *window = window_.handle();
	if (window == nullptr) return;
	switch (mode) {
		case platform::CursorMode::Visible:
			SDL_SetWindowRelativeMouseMode(window, false);
			SDL_ShowCursor();
			break;
		case platform::CursorMode::Hidden:
			SDL_SetWindowRelativeMouseMode(window, false);
			SDL_HideCursor();
			break;
		case platform::CursorMode::Captured: SDL_SetWindowRelativeMouseMode(window, true); break;
	}
}

void InputDevice::warp_cursor(int32_t x, int32_t y) {
	SDL_Window *window = window_.handle();
	if (window == nullptr) return;
	warp_x_ = static_cast<float>(x);
	warp_y_ = static_cast<float>(y);
	warp_pending_ = !SDL_GetWindowRelativeMouseMode(window);
	SDL_WarpMouseInWindow(window, warp_x_, warp_y_);
}

bool InputDevice::set_lock_key(platform::LockKey key, bool on) {
	struct Lock {
		SDL_Keymod mod;
		SDL_Scancode scancode;
		uint8_t vk;
	};
	Lock lock{ SDL_KMOD_SCROLL, SDL_SCANCODE_SCROLLLOCK, 0x91 };
	if (key == platform::LockKey::CapsLock) lock = { SDL_KMOD_CAPS, SDL_SCANCODE_CAPSLOCK, 0x14 };
	if (key == platform::LockKey::NumLock) lock = { SDL_KMOD_NUM, SDL_SCANCODE_NUMLOCKCLEAR, 0x90 };
#ifdef _WIN32
	if (os_keyboard_) {
		// The OS's toggle: the keyboard state's byte for the key (nonzero while its toggle is on
		// or it is held) against the state wanted, and where they differ the key's press and its
		// release put into the OS's input, the virtual key and the scan code both the key's code,
		// the release stamped 10. Their key events come through the pump.
		BYTE keyboard[256] = {};
		if (::GetKeyboardState(keyboard) == 0 || (keyboard[lock.vk] != 0) == on) return false;
		INPUT inputs[2] = {};
		for (INPUT &input : inputs) {
			input.type = INPUT_KEYBOARD;
			input.ki.wVk = lock.vk;
			input.ki.wScan = lock.vk;
		}
		inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
		inputs[1].ki.time = 10;
		return ::SendInput(2, inputs, sizeof(INPUT)) != 0;
	}
#endif
	// SDL's modifier state alone: its lock bit, the word state() reads (the bit, or the key held).
	const SDL_Keymod mods = SDL_GetModState();
	int count = 0;
	const bool *held = SDL_GetKeyboardState(&count);
	const bool down = held != nullptr && static_cast<int>(lock.scancode) < count && held[lock.scancode];
	if (((mods & lock.mod) != 0 || down) == on) return false;
	SDL_SetModState(on ? static_cast<SDL_Keymod>(mods | lock.mod)
					   : static_cast<SDL_Keymod>(mods & ~lock.mod));
	return true;
}

void InputDevice::on_event(const SDL_Event &event) {
	platform::InputEvent out;
	switch (event.type) {
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			out.type = platform::InputEvent::Type::Key;
			out.key = key_event(event.key);
			break;
		case SDL_EVENT_TEXT_INPUT: {
			// The character a key typed rides its key-down, the newest one still queued.
			const uint32_t text = first_code_point(event.text.text);
			for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
				if (it->type != platform::InputEvent::Type::Key) continue;
				if (it->key.down && it->key.text == 0) it->key.text = text;
				break;
			}
			return;
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			switch (event.button.button) {
				case SDL_BUTTON_LEFT: out.button.button = platform::MouseButton::Left; break;
				case SDL_BUTTON_RIGHT: out.button.button = platform::MouseButton::Right; break;
				case SDL_BUTTON_MIDDLE: out.button.button = platform::MouseButton::Middle; break;
				case SDL_BUTTON_X1: out.button.button = platform::MouseButton::X1; break;
				case SDL_BUTTON_X2: out.button.button = platform::MouseButton::X2; break;
				default: return;
			}
			out.type = platform::InputEvent::Type::MouseButton;
			out.button.down = event.button.down;
			// SDL counts a run's presses on; the window class's double-click message is every
			// second press of a run, a third a plain down again.
			out.button.clicks = event.button.down && event.button.clicks % 2 == 0 ? 2 : 1;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			if (warp_pending_ && event.motion.x == warp_x_ && event.motion.y == warp_y_) {
				warp_pending_ = false;
				return;
			}
			out.type = platform::InputEvent::Type::MouseMotion;
			out.motion.dx = event.motion.xrel;
			out.motion.dy = event.motion.yrel;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			out.type = platform::InputEvent::Type::MouseWheel;
			out.wheel.y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
																		  : event.wheel.y;
			if (out.wheel.y == 0.0f) return; // a horizontal turn alone
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			out.type = platform::InputEvent::Type::Focus;
			out.focus.gained = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
			break;
		default: return;
	}
	events_.push_back(out);
}

} // namespace opennova::sdl3
