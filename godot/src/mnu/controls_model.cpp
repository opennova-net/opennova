#include "mnu/controls_model.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input.hpp>

#include <runtime/controls/controls.h>

#include <cmath>

using namespace godot;

namespace {

opennova::controls::Device device_of(int p_device) {
	if (p_device == ControlsModel::DEVICE_MOUSE) {
		return opennova::controls::Device::Mouse;
	}
	if (p_device == ControlsModel::DEVICE_JOYSTICK) {
		return opennova::controls::Device::Joystick;
	}
	return opennova::controls::Device::Keyboard;
}

// The VK <-> Godot Key pairs whose values differ (letters, digits, and space
// share their ASCII values and translate arithmetically).
struct VkKeyPair {
	int vk;
	Key key;
};

constexpr VkKeyPair kVkKeyPairs[] = {
	{ 0x08, KEY_BACKSPACE }, { 0x09, KEY_TAB }, { 0x0D, KEY_ENTER },
	{ 0x10, KEY_SHIFT }, { 0x11, KEY_CTRL }, { 0x12, KEY_ALT },
	{ 0x13, KEY_PAUSE }, { 0x14, KEY_CAPSLOCK }, { 0x1B, KEY_ESCAPE },
	{ 0x21, KEY_PAGEUP }, { 0x22, KEY_PAGEDOWN }, { 0x23, KEY_END },
	{ 0x24, KEY_HOME }, { 0x25, KEY_LEFT }, { 0x26, KEY_UP },
	{ 0x27, KEY_RIGHT }, { 0x28, KEY_DOWN }, { 0x2C, KEY_PRINT },
	{ 0x2D, KEY_INSERT }, { 0x2E, KEY_DELETE },
	{ 0x60, KEY_KP_0 }, { 0x61, KEY_KP_1 }, { 0x62, KEY_KP_2 },
	{ 0x63, KEY_KP_3 }, { 0x64, KEY_KP_4 }, { 0x65, KEY_KP_5 },
	{ 0x66, KEY_KP_6 }, { 0x67, KEY_KP_7 }, { 0x68, KEY_KP_8 },
	{ 0x69, KEY_KP_9 }, { 0x6A, KEY_KP_MULTIPLY }, { 0x6B, KEY_KP_ADD },
	{ 0x6D, KEY_KP_SUBTRACT }, { 0x6E, KEY_KP_PERIOD }, { 0x6F, KEY_KP_DIVIDE },
	{ 0x70, KEY_F1 }, { 0x71, KEY_F2 }, { 0x72, KEY_F3 }, { 0x73, KEY_F4 },
	{ 0x74, KEY_F5 }, { 0x75, KEY_F6 }, { 0x76, KEY_F7 }, { 0x77, KEY_F8 },
	{ 0x78, KEY_F9 }, { 0x79, KEY_F10 }, { 0x7A, KEY_F11 }, { 0x7B, KEY_F12 },
	{ 0x90, KEY_NUMLOCK }, { 0x91, KEY_SCROLLLOCK },
	{ 0xBA, KEY_SEMICOLON }, { 0xBB, KEY_EQUAL }, { 0xBC, KEY_COMMA },
	{ 0xBD, KEY_MINUS }, { 0xBE, KEY_PERIOD }, { 0xBF, KEY_SLASH },
	{ 0xC0, KEY_QUOTELEFT }, { 0xDB, KEY_BRACKETLEFT },
	{ 0xDC, KEY_BACKSLASH }, { 0xDD, KEY_BRACKETRIGHT },
	{ 0xDE, KEY_APOSTROPHE },
	// The keypad Enter's witnessed 269 remap rides the VK side directly.
	{ 269, KEY_KP_ENTER },
};

} // namespace

TypedArray<PackedStringArray> ControlsModel::get_rows(int p_device) const {
	TypedArray<PackedStringArray> out;
	const std::vector<opennova::controls::ControlRow> rows =
			bindings_.build_rows(device_of(p_device));
	for (const opennova::controls::ControlRow &r : rows) {
		PackedStringArray cells;
		cells.push_back(opennova::to_gd(r.cls));
		cells.push_back(opennova::to_gd(r.action));
		cells.push_back(opennova::to_gd(r.control));
		out.push_back(cells);
	}
	return out;
}

int ControlsModel::action_index_for_row(int p_row) const {
	return bindings_.action_index_for_row(p_row);
}

String ControlsModel::control_text(int p_action, int p_device) const {
	return String::utf8(
			bindings_.control_text(p_action, device_of(p_device)).c_str());
}

bool ControlsModel::assign_godot_key(int p_action, int p_godot_key, bool p_ctrl,
		bool p_shift, bool p_repeat) {
	const int vk = vk_from_godot_key(p_godot_key);
	if (vk == 0) {
		return false;
	}
	return bindings_.assign_key(p_action, vk, p_ctrl, p_shift,
			opennova::controls::is_extended_vk(vk), p_repeat);
}

void ControlsModel::assign_mouse_mask(int p_action, int p_mask) {
	bindings_.assign_mouse(p_action, static_cast<uint16_t>(p_mask));
}

void ControlsModel::clear_binding(int p_action, int p_device) {
	bindings_.clear(p_action, device_of(p_device));
}

void ControlsModel::restore_defaults() {
	bindings_.restore_defaults();
}

PackedInt32Array ControlsModel::godot_keys_for_token(const String &p_token) const {
	PackedInt32Array out;
	const std::vector<int> vks =
			bindings_.keys_for_token(p_token.utf8().get_data());
	for (const int vk : vks) {
		const int key = godot_key_from_vk(vk);
		if (key != 0) {
			out.push_back(key);
		}
	}
	return out;
}

String ControlsModel::display_text_for_token(const String &p_token) const {
	const int index = bindings_.index_of_token(p_token.utf8().get_data());
	const opennova::controls::BindingRecord *r = bindings_.record(index);
	if (r == nullptr) {
		return String();
	}
	return opennova::to_gd(opennova::controls::format_display_string(*r));
}

int ControlsModel::pressed_key_for_token(const String &p_token) const {
	Input *input = Input::get_singleton();
	if (input == nullptr) {
		return 0;
	}
	// The device's held state by VK (the engine rule reads it as retail's
	// g_input_key_down_states[vk]).
	const auto physical_vk_down = [input](int vk) {
		const int key = godot_key_from_vk(vk);
		return key != 0 && input->is_physical_key_pressed(static_cast<Key>(key));
	};
	return bindings_.pressed_key(bindings_.index_of_token(p_token.utf8().get_data()),
			physical_vk_down);
}

bool ControlsModel::is_token_pressed(const String &p_token) const {
	const int index = bindings_.index_of_token(p_token.utf8().get_data());
	const opennova::controls::BindingRecord *r = bindings_.record(index);
	if (r == nullptr) {
		return false;
	}
	Input *input = Input::get_singleton();
	if (input == nullptr) {
		return false;
	}
	if (pressed_key_for_token(p_token) != 0) {
		return true;
	}
	uint16_t held = 0;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) held |= opennova::controls::kMouseLeft;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) held |= opennova::controls::kMouseRight;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE)) held |= opennova::controls::kMouseMiddle;
    const auto key_down = [input](int vk) {
        const int key = godot_key_from_vk(vk);
        return key != 0 && input->is_physical_key_pressed(static_cast<Key>(key));
    };
    if (bindings_.pressed_mouse(index, held, key_down)) return true;
    if (r->joy_button != 0) {
        const auto devices = input->get_connected_joypads();
        for (int i = 0; i < devices.size(); ++i) {
            const int device = devices[i];
            const auto button_down = [input, device](int button) {
                return button >= 0 && button < JOY_BUTTON_MAX &&
                    input->is_joy_button_pressed(device, static_cast<JoyButton>(button));
            };
            // Godot presents the first POV hat as four D-pad buttons.
            // Normalize that device shape to the native POV-angle input.
            const int dx = int(button_down(JOY_BUTTON_DPAD_RIGHT)) - int(button_down(JOY_BUTTON_DPAD_LEFT));
            const int dy = int(button_down(JOY_BUTTON_DPAD_DOWN)) - int(button_down(JOY_BUTTON_DPAD_UP));
            std::array<int32_t, 4> hats{-1, -1, -1, -1};
            if (dx != 0 || dy != 0) {
                const int angle = static_cast<int>(std::lround(std::atan2(double(dx), double(-dy)) *
                    (18000.0 / 3.14159265358979323846)));
                hats[0] = angle < 0 ? angle + 36000 : angle;
            }
            if (bindings_.pressed_joystick(index, button_down, hats)) return true;
        }
    }
    return false;
}

String ControlsModel::mouse_event_token(int p_button) const {
    Input *input = Input::get_singleton();
    if (input == nullptr) return String();
    const auto key_down = [input](int vk) {
        const int key = godot_key_from_vk(vk);
        return key != 0 && input->is_physical_key_pressed(static_cast<Key>(key));
    };
    const int index = bindings_.mouse_event_action(mouse_mask_from_godot_button(p_button), key_down);
    std::size_t count = 0;
    const auto *cat = opennova::controls::catalog(&count);
    return index >= 0 ? String::utf8(cat[index].token) : String();
}

int ControlsModel::mouse_mask_from_godot_button(int p_button) {
	// Godot MouseButton -> the engine's witnessed kMouse* masks (the
	// citation lives at controls/binding_set.h).
	switch (p_button) {
		case MOUSE_BUTTON_LEFT:
			return opennova::controls::kMouseLeft;
		case MOUSE_BUTTON_RIGHT:
			return opennova::controls::kMouseRight;
		case MOUSE_BUTTON_MIDDLE:
			return opennova::controls::kMouseMiddle;
		case MOUSE_BUTTON_WHEEL_UP:
			return opennova::controls::kMouseWheelUp;
		case MOUSE_BUTTON_WHEEL_DOWN:
			return opennova::controls::kMouseWheelDown;
		default:
			return 0;
	}
}

PackedStringArray ControlsModel::weapon_category_tokens() {
	PackedStringArray out;
	for (int i = 0; i < opennova::controls::kWeaponCategoryCount; ++i) {
		const char *token = opennova::controls::weapon_category_token(i);
		out.push_back(String(token != nullptr ? token : ""));
	}
	return out;
}

int ControlsModel::vk_from_godot_key(int p_godot_key) {
	// Letters, digits, and space share their values across the two code spaces.
	if ((p_godot_key >= KEY_A && p_godot_key <= KEY_Z) ||
			(p_godot_key >= KEY_0 && p_godot_key <= KEY_9) ||
			p_godot_key == KEY_SPACE) {
		return p_godot_key;
	}
	for (const VkKeyPair &pair : kVkKeyPairs) {
		if (pair.key == static_cast<Key>(p_godot_key)) {
			return pair.vk;
		}
	}
	return 0;
}

int ControlsModel::godot_key_from_vk(int p_vk) {
	if ((p_vk >= 0x41 && p_vk <= 0x5A) || (p_vk >= 0x30 && p_vk <= 0x39) ||
			p_vk == 0x20) {
		return p_vk;
	}
	for (const VkKeyPair &pair : kVkKeyPairs) {
		if (pair.vk == p_vk) {
			return static_cast<int>(pair.key);
		}
	}
	return 0;
}

Dictionary ControlsModel::save_blob() const {
	Dictionary blob;
	std::size_t n = 0;
	const opennova::controls::ActionDef *cat = opennova::controls::catalog(&n);
	for (std::size_t i = 0; i < n; ++i) {
		const opennova::controls::BindingRecord *r =
				bindings_.record(static_cast<int>(i));
		if (r == nullptr || cat[i].token == nullptr) {
			continue;
		}
		PackedInt32Array values;
		values.push_back(r->primary);
		values.push_back(r->secondary);
		values.push_back(r->primary_mod);
		values.push_back(r->secondary_mod);
		values.push_back(r->mouse_mask);
		values.push_back(r->joy_button);
		values.push_back(r->mouse_mod);
		values.push_back(r->joy_mod);
		blob[String::utf8(cat[i].token)] = values;
	}
	return blob;
}

void ControlsModel::load_blob(const Dictionary &p_blob) {
	const Array tokens = p_blob.keys();
	for (int i = 0; i < tokens.size(); ++i) {
		const String token = tokens[i];
		const PackedInt32Array values = p_blob[token];
		if (values.size() != 8) {
			continue;
		}
		const int index = bindings_.index_of_token(token.utf8().get_data());
		if (index < 0) {
			continue;
		}
		opennova::controls::BindingRecord rec;
		rec.primary = static_cast<uint16_t>(values[0]);
		rec.secondary = static_cast<uint16_t>(values[1]);
		rec.primary_mod = static_cast<uint16_t>(values[2]);
		rec.secondary_mod = static_cast<uint16_t>(values[3]);
		rec.mouse_mask = static_cast<uint16_t>(values[4]);
		rec.joy_button = static_cast<uint8_t>(values[5]);
        rec.mouse_mod = static_cast<uint16_t>(values[6]);
        rec.joy_mod = static_cast<uint8_t>(values[7]);
		bindings_.set_record(index, rec);
	}
}

void ControlsModel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_rows", "device"), &ControlsModel::get_rows);
	ClassDB::bind_method(D_METHOD("action_index_for_row", "row"),
			&ControlsModel::action_index_for_row);
	ClassDB::bind_method(D_METHOD("control_text", "action", "device"),
			&ControlsModel::control_text);
	ClassDB::bind_method(D_METHOD("assign_godot_key", "action", "key", "ctrl",
								 "shift", "repeat"),
			&ControlsModel::assign_godot_key, DEFVAL(false), DEFVAL(false),
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("assign_mouse_mask", "action", "mask"),
			&ControlsModel::assign_mouse_mask);
	ClassDB::bind_method(D_METHOD("clear_binding", "action", "device"),
			&ControlsModel::clear_binding);
	ClassDB::bind_method(D_METHOD("restore_defaults"), &ControlsModel::restore_defaults);
	ClassDB::bind_method(D_METHOD("godot_keys_for_token", "token"),
			&ControlsModel::godot_keys_for_token);
    ClassDB::bind_method(D_METHOD("mouse_event_token", "button"), &ControlsModel::mouse_event_token);
	ClassDB::bind_method(D_METHOD("is_token_pressed", "token"),
			&ControlsModel::is_token_pressed);
	ClassDB::bind_method(D_METHOD("pressed_key_for_token", "token"),
			&ControlsModel::pressed_key_for_token);
	ClassDB::bind_method(D_METHOD("display_text_for_token", "token"),
			&ControlsModel::display_text_for_token);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("mouse_mask_from_godot_button", "button"),
			&ControlsModel::mouse_mask_from_godot_button);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("vk_from_godot_key", "key"),
			&ControlsModel::vk_from_godot_key);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("godot_key_from_vk", "vk"),
			&ControlsModel::godot_key_from_vk);
	ClassDB::bind_method(D_METHOD("save_blob"), &ControlsModel::save_blob);
	ClassDB::bind_method(D_METHOD("load_blob", "blob"), &ControlsModel::load_blob);

	BIND_ENUM_CONSTANT(DEVICE_KEYBOARD);
	BIND_ENUM_CONSTANT(DEVICE_MOUSE);
	BIND_ENUM_CONSTANT(DEVICE_JOYSTICK);
}
