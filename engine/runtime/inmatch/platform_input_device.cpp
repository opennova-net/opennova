#include <runtime/inmatch/platform_input_device.h>

#include <runtime/controls/controls.h> // catalog, action_code
#include <runtime/controls/device_messages.h> // queued_key

#include <cstddef>
#include <string>

namespace opennova::inmatch {

namespace {

// The key state's extended half: an E0 key's VK at +0x100 (keypad Enter reads as 269).
constexpr int kExtendedKeys = 0x100;

// The dispatch code of catalog row `index`, -1 for none.
int row_code(int index) {
	std::size_t count = 0;
	const controls::ActionDef *rows = controls::catalog(&count);
	return index >= 0 && static_cast<std::size_t>(index) < count ? controls::action_code(rows[index].id)
																 : -1;
}

} // namespace

void PlatformInputDevice::event(const platform::InputEvent &event) {
	using Type = platform::InputEvent::Type;
	switch (event.type) {
		case Type::Key: {
			const int vk = static_cast<int>(event.key.key);
			if (vk <= 0 || vk >= kExtendedKeys) return;
			const bool extended = platform::scancode_extended(event.key.scancode);
			if (!event.key.down) {
				keys_[static_cast<std::size_t>(vk)] = false;
				keys_[static_cast<std::size_t>(kExtendedKeys | vk)] = false;
				return;
			}
			// A key-down message (WM_KEYDOWN and WM_SYSKEYDOWN alike, an OS repeat included) sets
			// the key's state, then queues the key with Ctrl and Shift as the key state holds them
			// [orig: Input_HandleKeyPress @0x760C90 - the key state @0x760CBB, the
			//  Input_QueueKeyEvent call @0x760CC2; Input_QueueKeyEvent @0x760c10 - Ctrl @0x760c4b,
			//  Shift @0x760c59].
			keys_[static_cast<std::size_t>(vk)] = true;
			reset_.key_down(vk);
			if (extended) {
				keys_[static_cast<std::size_t>(kExtendedKeys | vk)] = true;
				reset_.key_down(kExtendedKeys | vk);
			}
			const controls::QueuedKey queued =
					controls::queued_key(event.key, key_down(0x11), key_down(0x10));
			if (queued.vk != 0) queue_.push(queued.vk, queued.flags);
			return;
		}
		case Type::Focus:
			// The window procedure clears the key state alone on gaining and on losing the focus: a
			// key held through it reads up until its next key-down (controls::KeyStateReset)
			// [orig: Game_WindowProc @0x7624C0 - WM_KILLFOCUS @0x762664, WM_SETFOCUS @0x76268B ->
			//  Input_ClearKeyState @0x760DE0].
			reset_.reset([this](int vk) { return keys_[static_cast<std::size_t>(vk)]; });
			return;
		case Type::MouseButton: {
			const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(event.button.button));
			if (event.button.down) buttons_ |= bit;
			else buttons_ &= static_cast<uint8_t>(~bit);
			return;
		}
		default: return;
	}
}

bool PlatformInputDevice::key_down(int vk) const {
	if (vk <= 0 || vk >= controls::KeyStateReset::kKeyCount) return false;
	return reset_.held(vk, keys_[static_cast<std::size_t>(vk)]);
}

uint16_t PlatformInputDevice::mouse_held() const {
	const auto held = [this](platform::MouseButton button) {
		return (buttons_ & (1u << static_cast<unsigned>(button))) != 0;
	};
	uint16_t out = 0;
	if (held(platform::MouseButton::Left)) out |= controls::kMouseLeft;
	if (held(platform::MouseButton::Right)) out |= controls::kMouseRight;
	if (held(platform::MouseButton::Middle)) out |= controls::kMouseMiddle;
	return out;
}

int PlatformInputDevice::event_code(int scan_code, bool &sign) const {
	sign = false;
	const auto down = [this](int vk) { return key_down(vk); };
	return row_code(bindings_.event_row(scan_code, down, &sign));
}

bool PlatformInputDevice::token_pressed(const char *token, bool keyboard, bool mouse,
		bool active) const {
	const int index = bindings_.index_of_token(token);
	if (bindings_.record(index) == nullptr) return false;
	const auto down = [this](int vk) { return key_down(vk); };
	const uint16_t held = mouse_held();
	if (active) return bindings_.binding_active(index, down, held);
	if (keyboard && bindings_.pressed_key(index, down) != 0) return true;
	return mouse && bindings_.pressed_mouse(index, held, down);
}

bool PlatformInputDevice::pressed(const char *token) const {
	return token_pressed(token, true, true, false);
}

int PlatformInputDevice::pressed_key(const char *token) const {
	const auto down = [this](int vk) { return key_down(vk); };
	return bindings_.pressed_key(bindings_.index_of_token(token), down);
}

bool PlatformInputDevice::fire_signed(const char *token) const {
	const auto down = [this](int vk) { return key_down(vk); };
	return bindings_.fire_signed(bindings_.index_of_token(token), mouse_held(), down);
}

bool PlatformInputDevice::keys_down(const char *token) const {
	return pressed(token) || key_slots_down(token);
}

bool PlatformInputDevice::binding_active(const char *token) const {
	return token_pressed(token, true, true, true);
}

bool PlatformInputDevice::key_slots_down(const char *token) const {
	const controls::BindingRecord *record = bindings_.record(bindings_.index_of_token(token));
	if (record == nullptr) return false;
	for (const int vk : {static_cast<int>(record->primary), static_cast<int>(record->secondary)})
		if (vk != 0 && key_down(vk)) return true;
	return false;
}

// No scripted device and no joystick: their arms hold nothing.
bool PlatformInputDevice::held_off_keyboard(const char *token) const {
	(void)token;
	return false;
}

bool PlatformInputDevice::scripted_left_button(const char *token) const {
	(void)token;
	return false;
}

void PlatformInputDevice::set_scan_gates(bool death_screen, bool scan_word) {
	bindings_.set_scan_gates(death_screen, scan_word);
}

bool PlatformInputDevice::keyboard_captured() const {
	return bindings_.keyboard_captured();
}

PlayerInputDevice::ClickRecord PlatformInputDevice::mouse_click_record(uint32_t event) const {
	const auto down = [this](int vk) { return key_down(vk); };
	const controls::BindingSet::MouseClickRecord record =
			bindings_.mouse_click_record(event, down, bindings_.scan_death_screen());
	ClickRecord out;
	out.code = row_code(record.index);
	if (out.code >= 0) out.sign = record.sign;
	return out;
}

std::vector<controls::KeyRowEvent> PlatformInputDevice::joystick_records(bool death_screen,
		bool scroll_lock) {
	(void)death_screen;
	(void)scroll_lock;
	return {};
}

std::vector<controls::KeyRowEvent> PlatformInputDevice::key_release_pass() {
	const auto down = [this](int vk) { return key_down(vk); };
	return bindings_.release_pass(latch_, down);
}

void PlatformInputDevice::reset_key_queue() {
	queue_.reset();
}

void PlatformInputDevice::reset_key_state() {
	reset_.reset([this](int vk) { return keys_[static_cast<std::size_t>(vk)]; });
	latch_.clear();
}

bool PlatformInputDevice::step_scripted(bool advance, int64_t logic_tick, float &look_x,
		float &look_y) {
	(void)advance;
	(void)logic_tick;
	(void)look_x;
	(void)look_y;
	return false;
}

bool PlatformInputDevice::movement_override(world::PlayerInput &movement) const {
	(void)movement;
	return false;
}

bool PlatformInputDevice::scroll_lock_turn_off() {
	return false;
}

} // namespace opennova::inmatch
