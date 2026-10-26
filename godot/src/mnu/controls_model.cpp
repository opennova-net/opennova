#include "mnu/controls_model.h"
#include "player/player_profiles.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>

#include <runtime/controls/controls.h>
#include <runtime/profile/profile_controls.h>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

// The device id the scripted device's replayed key events carry, so the key queue's
// scan leaves their rows to the hold (ControlsModel::is_scripted_key_event).
constexpr int32_t kScriptedKeyDevice = 0x7E5C;

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
	return opennova::to_gd(bindings_.control_text(p_action, device_of(p_device)));
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

String ControlsModel::display_text_for_action_code(int p_code) const {
	const opennova::controls::ActionDef *def = opennova::controls::action_for_code(p_code);
	if (def == nullptr) {
		return opennova::to_gd(
				opennova::controls::format_display_string(opennova::controls::BindingRecord{}));
	}
	const opennova::controls::BindingRecord *r = bindings_.record(bindings_.index_of_token(def->token));
	if (r == nullptr) {
		return String();
	}
	return opennova::to_gd(
			opennova::controls::format_display_string(*r, (def->flags & 0x200u) != 0u));
}

void ControlsModel::build_help_screen() { help_screen_.build(bindings_); }
void ControlsModel::cycle_help_page(bool p_forward) { help_screen_.cycle_page(p_forward); }
String ControlsModel::get_help_title() const { return opennova::to_gd(help_screen_.title()); }
String ControlsModel::get_help_page_line() const {
	return opennova::to_gd(help_screen_.page_line());
}
PackedStringArray ControlsModel::get_help_keys() const {
	PackedStringArray out;
	for (const opennova::controls::HelpScreenRow &row : help_screen_.rows())
		out.push_back(opennova::to_gd(row.key));
	return out;
}
PackedStringArray ControlsModel::get_help_texts() const {
	PackedStringArray out;
	for (const opennova::controls::HelpScreenRow &row : help_screen_.rows())
		out.push_back(opennova::to_gd(row.text));
	return out;
}
String ControlsModel::help_footer() { return opennova::to_gd(opennova::controls::help_screen_footer()); }

int ControlsModel::pressed_key_for_token(const String &p_token) const {
	Input *input = Input::get_singleton();
	if (input == nullptr) {
		return 0;
	}
	// The device's held state by VK (the engine rule reads it as retail's
	// g_input_key_down_states[vk]).
	const auto physical_vk_down = [this](int vk) { return vk_down_(vk); };
	return bindings_.pressed_key(bindings_.index_of_token(p_token.utf8().get_data()),
			physical_vk_down);
}

bool ControlsModel::is_token_fire_signed(const String &p_token) const {
	Input *input = Input::get_singleton();
	if (input == nullptr) {
		return false;
	}
	uint16_t held = 0;
	if (input->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) held |= opennova::controls::kMouseLeft;
	if (input->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) held |= opennova::controls::kMouseRight;
	if (input->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE)) held |= opennova::controls::kMouseMiddle;
	const auto key_down = [this](int vk) { return vk_down_(vk); };
	return bindings_.fire_signed(bindings_.index_of_token(p_token.utf8().get_data()), held,
			key_down);
}

bool ControlsModel::vk_down_(int p_vk) const {
	Input *input = Input::get_singleton();
	const int key = godot_key_from_vk(p_vk);
	// The key state by the layout's VK, as WM_KEYDOWN stores it (engine controls::KeyEvent
	// carries the witness). A key held through a menu close's reset reads up (KeyStateReset).
	const bool down = key_reset_.held(p_vk, input != nullptr && key != 0 &&
			input->is_key_pressed(static_cast<Key>(key)));
	// A press the special-key handler took reads up until the key is
	// released (consume_key_press).
	if (p_vk > 0 && p_vk < static_cast<int>(consumed_vks_.size()) && consumed_vks_[p_vk]) {
		if (down) return false;
		consumed_vks_[p_vk] = false;
	}
	return down;
}

bool ControlsModel::is_token_key_down(const char *p_token) const {
	const opennova::controls::BindingRecord *r =
			bindings_.record(bindings_.index_of_token(p_token));
	if (r == nullptr) return false;
	for (const int vk : {static_cast<int>(r->primary), static_cast<int>(r->secondary)})
		if (vk != 0 && is_key_state_down(vk)) return true;
	return false;
}

bool ControlsModel::is_key_state_down(int p_vk) const {
	Input *input = Input::get_singleton();
	const int key = godot_key_from_vk(p_vk);
	return key_reset_.held(p_vk, input != nullptr && key != 0 &&
			input->is_key_pressed(static_cast<Key>(key)));
}

void ControlsModel::reset_key_state() {
	Input *input = Input::get_singleton();
	key_reset_.reset([input](int p_vk) {
		const int key = godot_key_from_vk(p_vk);
		return input != nullptr && key != 0 &&
				input->is_key_pressed(static_cast<Key>(key));
	});
}

void ControlsModel::key_down_event(int p_godot_key) {
	key_reset_.key_down(vk_from_godot_key(p_godot_key));
}

int ControlsModel::event_code(int p_scan_code) const {
	const auto key_down = [this](int vk) { return vk_down_(vk); };
	// The scan names a catalog index; its dispatch code is the row id's (the port's
	// catalog leaves row 66 out, so an index past it is not its id).
	const int index = bindings_.event_row(p_scan_code, key_down);
	std::size_t count = 0;
	const opennova::controls::ActionDef *rows = opennova::controls::catalog(&count);
	return index >= 0 && static_cast<std::size_t>(index) < count
			? opennova::controls::action_code(rows[index].id)
			: -1;
}

bool ControlsModel::event_signed(int p_scan_code) const {
	const auto key_down = [this](int vk) { return vk_down_(vk); };
	bool fallback_signed = false;
	bindings_.event_row(p_scan_code, key_down, &fallback_signed);
	return fallback_signed;
}

bool ControlsModel::is_scripted_key_event(const Ref<InputEventKey> &p_event) {
	return p_event.is_valid() && p_event->get_device() == kScriptedKeyDevice;
}

void ControlsModel::consume_key_press(int p_vk) {
	if (p_vk > 0 && p_vk < static_cast<int>(consumed_vks_.size())) consumed_vks_[p_vk] = true;
}

void ControlsModel::advance_scripted_input(int64_t p_logic_tick) {
	if (!scripted_input_.is_valid()) return;
	opennova::devtools::ScriptedInput &script = scripted_input_->native();
	const std::vector<int> before = script.held_codes();
	script.advance(p_logic_tick);
	const std::vector<int> after = script.held_codes();
	Input *input = Input::get_singleton();
	if (input == nullptr) return;
	// The row's bound key (slot 1, else slot 2) with that slot's modifier, as
	// Godot keys: a keyboard press of a modified binding (Ctrl+1's seat1,
	// Ctrl+F6's hudcolor) holds the modifier, without which the bare key would
	// reach the row the key alone binds (the 1's Knife, the F6's huddetail).
	struct Slot {
		int key = 0;
		int mod = 0;
	};
	const auto slot_of = [&](int p_code) {
		Slot slot;
		const opennova::controls::ActionDef *def = opennova::controls::action_for_code(p_code);
		if (def == nullptr) return slot;
		const opennova::controls::BindingRecord *r =
				bindings_.record(bindings_.index_of_token(def->token));
		if (r == nullptr) return slot;
		const bool first = r->primary != 0;
		slot.key = godot_key_from_vk(first ? r->primary : r->secondary);
		slot.mod = godot_key_from_vk(first ? r->primary_mod : r->secondary_mod);
		return slot;
	};
	const auto key_event = [input](int p_key, bool p_pressed) {
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_device(kScriptedKeyDevice);
		event->set_keycode(static_cast<Key>(p_key));
		event->set_physical_keycode(static_cast<Key>(p_key));
		event->set_pressed(p_pressed);
		input->parse_input_event(event);
	};
	const auto holds = [](const std::vector<int> &p_codes, int p_code) {
		return std::find(p_codes.begin(), p_codes.end(), p_code) != p_codes.end();
	};
	const auto replay = [&](int p_code, bool p_pressed) {
		const Slot slot = slot_of(p_code);
		if (slot.key == 0) return false;
		if (p_pressed && slot.mod != 0) key_event(slot.mod, true);
		key_event(slot.key, p_pressed);
		// The modifier comes up with its last scripted row.
		if (!p_pressed && slot.mod != 0 &&
				std::none_of(after.begin(), after.end(),
						[&](int p_held) { return slot_of(p_held).mod == slot.mod; }))
			key_event(slot.mod, false);
		return true;
	};
	bool replayed = false;
	for (const int code : after) {
		if (!holds(before, code)) replayed = replay(code, true) || replayed;
	}
	for (const int code : before) {
		if (!holds(after, code)) replayed = replay(code, false) || replayed;
	}
	// Each typed character arrives as the keystroke onHook's driver hands
	// retail within the step's tick: the key's press carrying the character
	// (WM_KEYDOWN, then the WM_CHAR stored into its ring entry), then its
	// release; an open line's editor takes it through the shell's chat route
	// (HudChatEntry::key_event -> hud::ChatEntry::key) as the tick's ring
	// drain hands the editor a capture-mode entry (engine
	// devtools/scripted_input.h carries the witness). A letter is its own key
	// (shifted when upper case), backspace and return theirs, any other
	// printable character the key Godot gives its code.
	for (const uint8_t character : script.take_chars()) {
		Key key = static_cast<Key>(character);
		if (character == 0x08) key = KEY_BACKSPACE;
		else if (character == 0x0D) key = KEY_ENTER;
		else if (character >= 'a' && character <= 'z') key = static_cast<Key>(character - 'a' + 'A');
		for (const bool pressed : {true, false}) {
			Ref<InputEventKey> event;
			event.instantiate();
			event->set_device(kScriptedKeyDevice);
			event->set_keycode(key);
			event->set_physical_keycode(key);
			event->set_shift_pressed(character >= 'A' && character <= 'Z');
			if (pressed && character >= 0x20) event->set_unicode(character);
			event->set_pressed(pressed);
			input->parse_input_event(event);
		}
		replayed = true;
	}
	// The key state lands now, in step with the held tokens the sample reads
	// next, not at the next frame's event flush.
	if (replayed) input->flush_buffered_events();
}

bool ControlsModel::is_token_pressed(const String &p_token) const {
	return token_pressed_(p_token, true);
}

bool ControlsModel::is_token_held_off_keyboard(const String &p_token) const {
	return token_pressed_(p_token, false, false);
}

bool ControlsModel::is_token_binding_active(const char *p_token) const {
	return token_pressed_(String(p_token), true, true, true);
}

bool ControlsModel::is_token_scripted_left_button(const char *p_token) const {
	const opennova::controls::BindingRecord *r = bindings_.record(bindings_.index_of_token(p_token));
	return r != nullptr && r->mouse_mask == opennova::controls::kMouseLeft &&
			scripted_input_.is_valid() && !bindings_.keyboard_captured() &&
			scripted_input_->native().token_held(p_token);
}

ControlsModel::MouseClickRecord ControlsModel::mouse_click_record(int p_event) const {
	const auto key_down = [this](int vk) { return vk_down_(vk); };
	const opennova::controls::BindingSet::MouseClickRecord record = bindings_.mouse_click_record(
			static_cast<uint32_t>(p_event), key_down, bindings_.scan_death_screen());
	std::size_t count = 0;
	const auto *cat = opennova::controls::catalog(&count);
	MouseClickRecord out;
	if (record.index >= 0 && static_cast<std::size_t>(record.index) < count) {
		out.code = opennova::controls::action_code(cat[record.index].id);
		out.sign = record.sign;
	}
	return out;
}

bool ControlsModel::token_pressed_(const String &p_token, bool p_keyboard, bool p_mouse,
		bool p_binding_active) const {
	const CharString token = p_token.utf8();
	const int index = bindings_.index_of_token(token.get_data());
	const opennova::controls::BindingRecord *r = bindings_.record(index);
	if (r == nullptr) {
		return false;
	}
	// The scripted device holds the row itself, as a keyboard would.
	if (scripted_input_.is_valid() && !bindings_.keyboard_captured() &&
			scripted_input_->native().token_held(token.get_data())) {
		return true;
	}
	Input *input = Input::get_singleton();
	if (input == nullptr) {
		return false;
	}
	uint16_t held = 0;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) held |= opennova::controls::kMouseLeft;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) held |= opennova::controls::kMouseRight;
    if (input->is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE)) held |= opennova::controls::kMouseMiddle;
	if (p_binding_active) {
		const auto key_state = [this](int vk) { return is_key_state_down(vk); };
		if (bindings_.binding_active(index, key_state, held)) return true;
	} else {
		if (p_keyboard && pressed_key_for_token(p_token) != 0) {
			return true;
		}
		const auto key_down = [this](int vk) { return vk_down_(vk); };
		if (p_mouse && bindings_.pressed_mouse(index, held, key_down)) return true;
	}
    // A disabled joystick is never polled (engine BindingSet::set_joystick_enabled).
    if (r->joy_button != 0 && bindings_.joystick_enabled()) {
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

void ControlsModel::apply_profile(const Ref<PlayerProfiles> &p_profiles) {
	if (p_profiles.is_null()) return;
	opennova::profile::apply_controls(p_profiles->native().current(), bindings_);
}

Dictionary ControlsModel::save_blob() const {
	Dictionary blob;
	std::size_t n = 0;
	const opennova::controls::ActionDef *cat = opennova::controls::catalog(&n);
	for (std::size_t i = 0; i < n; ++i) {
		const opennova::controls::BindingRecord *r = bindings_.record(static_cast<int>(i));
		// The rows a player's binding table carries: those whose flags hold 0x4000000 (engine
		// profile/player_profiles.cpp default_binding_table carries the witness); the cheat
		// row's token field is its command CRC, no text (engine controls/controls.cpp).
		if (r == nullptr || cat[i].token == nullptr || (cat[i].flags & 0x4000000u) == 0) continue;
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
		if (values.size() != 8) continue;
		const int index = bindings_.index_of_token(token.utf8().get_data());
		if (index < 0) continue;
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
	ClassDB::bind_method(D_METHOD("is_token_pressed", "token"),
			&ControlsModel::is_token_pressed);
	ClassDB::bind_method(D_METHOD("set_scripted_input", "input"),
			&ControlsModel::set_scripted_input);
	ClassDB::bind_method(D_METHOD("get_scripted_input"), &ControlsModel::get_scripted_input);
	ClassDB::bind_method(D_METHOD("advance_scripted_input", "logic_tick"),
			&ControlsModel::advance_scripted_input);
	ClassDB::bind_method(D_METHOD("pressed_key_for_token", "token"),
			&ControlsModel::pressed_key_for_token);
	ClassDB::bind_method(D_METHOD("is_token_fire_signed", "token"),
			&ControlsModel::is_token_fire_signed);
	ClassDB::bind_method(D_METHOD("set_keyboard_captured", "captured"),
			&ControlsModel::set_keyboard_captured);
	ClassDB::bind_method(D_METHOD("is_keyboard_captured"), &ControlsModel::is_keyboard_captured);
	ClassDB::bind_method(D_METHOD("set_scan_gates", "death_screen", "scan_word"),
			&ControlsModel::set_scan_gates);
	ClassDB::bind_method(D_METHOD("build_help_screen"), &ControlsModel::build_help_screen);
	ClassDB::bind_method(D_METHOD("cycle_help_page", "forward"), &ControlsModel::cycle_help_page);
	ClassDB::bind_method(D_METHOD("get_help_title"), &ControlsModel::get_help_title);
	ClassDB::bind_method(D_METHOD("get_help_page_line"), &ControlsModel::get_help_page_line);
	ClassDB::bind_method(D_METHOD("get_help_keys"), &ControlsModel::get_help_keys);
	ClassDB::bind_method(D_METHOD("get_help_texts"), &ControlsModel::get_help_texts);
	ClassDB::bind_static_method("ControlsModel", D_METHOD("help_footer"), &ControlsModel::help_footer);
	ClassDB::bind_method(D_METHOD("display_text_for_token", "token"),
			&ControlsModel::display_text_for_token);
	ClassDB::bind_method(D_METHOD("display_text_for_action_code", "code"),
			&ControlsModel::display_text_for_action_code);
	ClassDB::bind_method(D_METHOD("get_binding_revision"),
			&ControlsModel::get_binding_revision);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("mouse_mask_from_godot_button", "button"),
			&ControlsModel::mouse_mask_from_godot_button);
	ClassDB::bind_method(D_METHOD("consume_key_press", "vk"), &ControlsModel::consume_key_press);
	ClassDB::bind_method(D_METHOD("is_vk_down", "vk"), &ControlsModel::is_vk_down);
	ClassDB::bind_method(D_METHOD("event_code", "scan_code"), &ControlsModel::event_code);
	ClassDB::bind_method(D_METHOD("event_signed", "scan_code"), &ControlsModel::event_signed);
	ClassDB::bind_static_method("ControlsModel", D_METHOD("is_scripted_key_event", "event"),
			&ControlsModel::is_scripted_key_event);
	ClassDB::bind_method(D_METHOD("reset_key_state"), &ControlsModel::reset_key_state);
	ClassDB::bind_method(D_METHOD("key_down_event", "key"), &ControlsModel::key_down_event);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("vk_from_godot_key", "key"),
			&ControlsModel::vk_from_godot_key);
	ClassDB::bind_static_method("ControlsModel",
			D_METHOD("godot_key_from_vk", "vk"),
			&ControlsModel::godot_key_from_vk);
	ClassDB::bind_method(D_METHOD("apply_profile", "profiles"), &ControlsModel::apply_profile);
	ClassDB::bind_method(D_METHOD("is_joystick_enabled"), &ControlsModel::is_joystick_enabled);
	ClassDB::bind_method(D_METHOD("save_blob"), &ControlsModel::save_blob);
	ClassDB::bind_method(D_METHOD("load_blob", "blob"), &ControlsModel::load_blob);

	BIND_ENUM_CONSTANT(DEVICE_KEYBOARD);
	BIND_ENUM_CONSTANT(DEVICE_MOUSE);
	BIND_ENUM_CONSTANT(DEVICE_JOYSTICK);
	BIND_CONSTANT(SLOT_BAR_KEY_RECORD_BASE);
}

static_assert(ControlsModel::SLOT_BAR_KEY_RECORD_BASE == opennova::controls::kSlotBarKeyRecordBase,
		"SLOT_BAR_KEY_RECORD_BASE drifted from engine controls.h");
