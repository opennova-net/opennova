#include "player/input_device.h"

#include "mnu/controls_model.h"
#include "player/key_lock_device.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

#include <runtime/controls/player_actions.h> // kVkCapital, kVkNumLock, kVkScroll

namespace godot {

namespace {

namespace platform = opennova::platform;

// Godot's physical keys (the US layout's positions) onto the set-1 make codes, the E0-prefixed
// keys with platform::kScancodeExtended.
struct ScancodePair {
	Key key;
	uint16_t scancode;
};

constexpr uint16_t kE0 = platform::kScancodeExtended;

constexpr ScancodePair kScancodePairs[] = {
	{ KEY_ESCAPE, 0x01 }, { KEY_1, 0x02 }, { KEY_2, 0x03 }, { KEY_3, 0x04 }, { KEY_4, 0x05 },
	{ KEY_5, 0x06 }, { KEY_6, 0x07 }, { KEY_7, 0x08 }, { KEY_8, 0x09 }, { KEY_9, 0x0A },
	{ KEY_0, 0x0B }, { KEY_MINUS, 0x0C }, { KEY_EQUAL, 0x0D }, { KEY_BACKSPACE, 0x0E },
	{ KEY_TAB, 0x0F }, { KEY_Q, 0x10 }, { KEY_W, 0x11 }, { KEY_E, 0x12 }, { KEY_R, 0x13 },
	{ KEY_T, 0x14 }, { KEY_Y, 0x15 }, { KEY_U, 0x16 }, { KEY_I, 0x17 }, { KEY_O, 0x18 },
	{ KEY_P, 0x19 }, { KEY_BRACKETLEFT, 0x1A }, { KEY_BRACKETRIGHT, 0x1B }, { KEY_ENTER, 0x1C },
	{ KEY_A, 0x1E }, { KEY_S, 0x1F }, { KEY_D, 0x20 }, { KEY_F, 0x21 }, { KEY_G, 0x22 },
	{ KEY_H, 0x23 }, { KEY_J, 0x24 }, { KEY_K, 0x25 }, { KEY_L, 0x26 }, { KEY_SEMICOLON, 0x27 },
	{ KEY_APOSTROPHE, 0x28 }, { KEY_QUOTELEFT, 0x29 }, { KEY_BACKSLASH, 0x2B }, { KEY_Z, 0x2C },
	{ KEY_X, 0x2D }, { KEY_C, 0x2E }, { KEY_V, 0x2F }, { KEY_B, 0x30 }, { KEY_N, 0x31 },
	{ KEY_M, 0x32 }, { KEY_COMMA, 0x33 }, { KEY_PERIOD, 0x34 }, { KEY_SLASH, 0x35 },
	{ KEY_KP_MULTIPLY, 0x37 }, { KEY_SPACE, 0x39 }, { KEY_CAPSLOCK, 0x3A }, { KEY_F1, 0x3B },
	{ KEY_F2, 0x3C }, { KEY_F3, 0x3D }, { KEY_F4, 0x3E }, { KEY_F5, 0x3F }, { KEY_F6, 0x40 },
	{ KEY_F7, 0x41 }, { KEY_F8, 0x42 }, { KEY_F9, 0x43 }, { KEY_F10, 0x44 },
	{ KEY_NUMLOCK, kE0 | 0x45 }, { KEY_PAUSE, 0x45 }, { KEY_SCROLLLOCK, 0x46 },
	{ KEY_KP_7, 0x47 }, { KEY_KP_8, 0x48 }, { KEY_KP_9, 0x49 }, { KEY_KP_SUBTRACT, 0x4A },
	{ KEY_KP_4, 0x4B }, { KEY_KP_5, 0x4C }, { KEY_KP_6, 0x4D }, { KEY_KP_ADD, 0x4E },
	{ KEY_KP_1, 0x4F }, { KEY_KP_2, 0x50 }, { KEY_KP_3, 0x51 }, { KEY_KP_0, 0x52 },
	{ KEY_KP_PERIOD, 0x53 }, { KEY_F11, 0x57 }, { KEY_F12, 0x58 },
	{ KEY_KP_ENTER, kE0 | 0x1C }, { KEY_KP_DIVIDE, kE0 | 0x35 }, { KEY_PRINT, kE0 | 0x37 },
	{ KEY_HOME, kE0 | 0x47 }, { KEY_UP, kE0 | 0x48 }, { KEY_PAGEUP, kE0 | 0x49 },
	{ KEY_LEFT, kE0 | 0x4B }, { KEY_RIGHT, kE0 | 0x4D }, { KEY_END, kE0 | 0x4F },
	{ KEY_DOWN, kE0 | 0x50 }, { KEY_PAGEDOWN, kE0 | 0x51 }, { KEY_INSERT, kE0 | 0x52 },
	{ KEY_DELETE, kE0 | 0x53 }, { KEY_META, kE0 | 0x5B }, { KEY_MENU, kE0 | 0x5D },
};

platform::Scancode scancode_of(Key p_key, KeyLocation p_location) {
	// The modifiers' sides are their location: the right Ctrl and Alt are E0 keys.
	switch (p_key) {
		case KEY_SHIFT:
			return static_cast<platform::Scancode>(p_location == KEY_LOCATION_RIGHT ? 0x36 : 0x2A);
		case KEY_CTRL:
			return static_cast<platform::Scancode>(
					p_location == KEY_LOCATION_RIGHT ? (kE0 | 0x1D) : 0x1D);
		case KEY_ALT:
			return static_cast<platform::Scancode>(
					p_location == KEY_LOCATION_RIGHT ? (kE0 | 0x38) : 0x38);
		default: break;
	}
	for (const ScancodePair &pair : kScancodePairs)
		if (pair.key == p_key) return static_cast<platform::Scancode>(pair.scancode);
	return platform::Scancode::Unknown;
}

} // namespace

platform::KeyEvent InputDevice::key_event(const InputEventKey &p_key) {
	platform::KeyEvent out;
	// The layout's key, else the physical one (a synthesized event may carry either).
	const Key keycode = p_key.get_keycode() != KEY_NONE ? p_key.get_keycode()
														: p_key.get_physical_keycode();
	const Key physical = p_key.get_physical_keycode() != KEY_NONE ? p_key.get_physical_keycode()
																  : keycode;
	// Keypad Enter is VK_RETURN on its own scancode.
	out.key = static_cast<platform::KeyCode>(keycode == KEY_KP_ENTER
					? static_cast<int>(platform::KeyCode::Return)
					: ControlsModel::vk_from_godot_key(static_cast<int>(keycode)));
	out.scancode = scancode_of(physical, p_key.get_location());
	out.down = p_key.is_pressed();
	out.repeat = p_key.is_echo();
	if (p_key.is_shift_pressed()) out.mods |= platform::key_mod::kShift;
	if (p_key.is_ctrl_pressed()) out.mods |= platform::key_mod::kCtrl;
	if (p_key.is_alt_pressed()) out.mods |= platform::key_mod::kAlt;
	out.text = static_cast<uint32_t>(p_key.get_unicode());
	return out;
}

namespace {

// One Godot event as the engine's raw device event (InputDevice::push).
bool to_platform(const Ref<InputEvent> &p_event, platform::InputEvent &r_event) {
	r_event = platform::InputEvent{};
	if (const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr())) {
		r_event.type = platform::InputEvent::Type::Key;
		r_event.key = key_event(*key);
		return true;
	}
	if (const InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		const MouseButton index = button->get_button_index();
		if (index == MOUSE_BUTTON_WHEEL_UP || index == MOUSE_BUTTON_WHEEL_DOWN) {
			// A turn is the wheel button's press; its factor is |delta| / WHEEL_DELTA (0 where
			// the platform reports none: one notch).
			if (!button->is_pressed()) return false;
			const float factor = button->get_factor();
			const float notches = factor > 0.0f ? factor : 1.0f;
			r_event.type = platform::InputEvent::Type::MouseWheel;
			r_event.wheel.y = index == MOUSE_BUTTON_WHEEL_UP ? notches : -notches;
			return true;
		}
		platform::MouseButton engine_button;
		switch (index) {
			case MOUSE_BUTTON_LEFT: engine_button = platform::MouseButton::Left; break;
			case MOUSE_BUTTON_RIGHT: engine_button = platform::MouseButton::Right; break;
			case MOUSE_BUTTON_MIDDLE: engine_button = platform::MouseButton::Middle; break;
			case MOUSE_BUTTON_XBUTTON1: engine_button = platform::MouseButton::X1; break;
			case MOUSE_BUTTON_XBUTTON2: engine_button = platform::MouseButton::X2; break;
			default: return false;
		}
		r_event.type = platform::InputEvent::Type::MouseButton;
		r_event.button.button = engine_button;
		r_event.button.down = button->is_pressed();
		r_event.button.clicks = button->is_pressed() && button->is_double_click() ? 2 : 1;
		return true;
	}
	if (const InputEventMouseMotion *motion = Object::cast_to<InputEventMouseMotion>(p_event.ptr())) {
		const Vector2 relative = motion->get_relative();
		r_event.type = platform::InputEvent::Type::MouseMotion;
		r_event.motion.dx = static_cast<float>(relative.x);
		r_event.motion.dy = static_cast<float>(relative.y);
		return true;
	}
	return false;
}

} // namespace

bool InputDevice::push(const Ref<InputEvent> &p_event) {
	platform::InputEvent event;
	if (!to_platform(p_event, event))
		return false;
	events_.push_back(event);
	return true;
}

bool InputDevice::poll_event(platform::InputEvent &r_event) {
	if (events_.empty())
		return false;
	r_event = events_.front();
	events_.pop_front();
	return true;
}

void InputDevice::set_cursor_mode(platform::CursorMode p_mode) {
	Input *input = Input::get_singleton();
	const Input::MouseMode mode = p_mode == platform::CursorMode::Captured
			? Input::MOUSE_MODE_CAPTURED
			: (p_mode == platform::CursorMode::Hidden ? Input::MOUSE_MODE_HIDDEN
													  : Input::MOUSE_MODE_VISIBLE);
	if (input->get_mouse_mode() != mode)
		input->set_mouse_mode(mode);
}

void InputDevice::warp_cursor(int32_t p_x, int32_t p_y) {
	DisplayServer::get_singleton()->warp_mouse(Vector2i(p_x, p_y));
}

platform::InputState InputDevice::state() const {
	platform::InputState out;
	DisplayServer *display = DisplayServer::get_singleton();
	if (display != nullptr) {
		const Vector2i client = display->mouse_get_position() - display->window_get_position();
		out.cursor_x = client.x;
		out.cursor_y = client.y;
	}
	Input *input = Input::get_singleton();
	if (input != nullptr) {
		out.cursor_captured = input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
		const struct {
			MouseButton godot;
			platform::MouseButton engine;
		} buttons[] = {
			{ MOUSE_BUTTON_LEFT, platform::MouseButton::Left },
			{ MOUSE_BUTTON_RIGHT, platform::MouseButton::Right },
			{ MOUSE_BUTTON_MIDDLE, platform::MouseButton::Middle },
			{ MOUSE_BUTTON_XBUTTON1, platform::MouseButton::X1 },
			{ MOUSE_BUTTON_XBUTTON2, platform::MouseButton::X2 },
		};
		for (const auto &button : buttons)
			if (input->is_mouse_button_pressed(button.godot))
				out.buttons |= static_cast<uint8_t>(1u << static_cast<unsigned>(button.engine));
	}
	out.caps_lock = key_lock_device::on(opennova::controls::kVkCapital);
	out.num_lock = key_lock_device::on(opennova::controls::kVkNumLock);
	out.scroll_lock = key_lock_device::on(opennova::controls::kVkScroll);
	return out;
}

} // namespace godot
