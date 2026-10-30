// The local player's device over the platform's raw input (inmatch::PlatformInputDevice): a
// key's event sets and clears its key state, which the default binding table's held rows read
// (move_forward on W and the Up arrow); a key-down queues its key with Ctrl and Shift as the key
// state holds them and the extended bit from its scancode, an OS repeat with the repeat bit; keypad
// Enter's extended state reads as 269; a focus change clears the key state alone (a key held
// through it reads up until its next key-down, the queue keeps its keys); the mouse buttons' held
// words reach the mouse rows; the key scan names a key-down's row by its dispatch code.
#include <runtime/controls/binding_set.h>
#include <runtime/controls/controls.h>
#include <runtime/controls/key_event_queue.h>
#include <runtime/inmatch/platform_input_device.h>

#include <cstdio>
#include <string>

namespace {

using namespace opennova;

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

platform::InputEvent key(int vk, uint16_t scancode, bool down, bool repeat = false, uint8_t mods = 0) {
	platform::InputEvent event;
	event.type = platform::InputEvent::Type::Key;
	event.key.key = static_cast<platform::KeyCode>(vk);
	event.key.scancode = static_cast<platform::Scancode>(scancode);
	event.key.down = down;
	event.key.repeat = repeat;
	event.key.mods = mods;
	return event;
}

platform::InputEvent focus(bool gained) {
	platform::InputEvent event;
	event.type = platform::InputEvent::Type::Focus;
	event.focus.gained = gained;
	return event;
}

platform::InputEvent button(platform::MouseButton which, bool down) {
	platform::InputEvent event;
	event.type = platform::InputEvent::Type::MouseButton;
	event.button.button = which;
	event.button.down = down;
	return event;
}

int code_of(const char *token) {
	std::size_t count = 0;
	const controls::ActionDef *rows = controls::catalog(&count);
	for (std::size_t i = 0; i < count; ++i)
		if (rows[i].token != nullptr && std::string(token) == rows[i].token)
			return controls::action_code(rows[i].id);
	return -1;
}

constexpr int kW = 0x57;
constexpr uint16_t kWScancode = 0x11;
constexpr int kUp = 0x26;
constexpr uint16_t kUpScancode = platform::kScancodeExtended | 0x48;

} // namespace

int main() {
	controls::BindingSet bindings;
	inmatch::PlatformInputDevice device(bindings);

	// The held row reads the key state.
	CHECK(!device.pressed("move_forward"), "nothing held");
	device.event(key(kW, kWScancode, true));
	CHECK(device.key_down(kW), "W's key state");
	CHECK(device.pressed("move_forward"), "W holds move_forward");
	CHECK(device.pressed_key("move_forward") == kW, "the row's firing key is W");
	CHECK(device.key_slots_down("move_forward"), "a key slot of the row is down");
	device.event(key(kW, kWScancode, false));
	CHECK(!device.key_down(kW), "W released");
	CHECK(!device.pressed("move_forward"), "the row lets go with W");
	device.event(key(kUp, kUpScancode, true));
	CHECK(device.pressed("move_forward"), "the Up arrow, the row's second slot");
	CHECK(device.key_down(0x100 | kUp), "an extended key's state at +0x100 too");
	device.event(key(kUp, kUpScancode, false));

	// Each key-down queues its key: the flag word's Ctrl and Shift from the key state, the
	// extended bit from the scancode, the repeat bit on an OS repeat. A release queues nothing.
	controls::KeyEventQueue &queue = device.key_queue();
	controls::KeyEvent queued;
	while (queue.pop(queued)) {}
	device.event(key(0x10, 0x2A, true));                 // Shift down
	device.event(key(kW, kWScancode, true, false, platform::key_mod::kShift));
	device.event(key(kW, kWScancode, true, true, platform::key_mod::kShift)); // its repeat
	device.event(key(kW, kWScancode, false));
	device.event(key(0x10, 0x2A, false));
	device.event(key(kUp, kUpScancode, true));
	device.event(key(kUp, kUpScancode, false));
	CHECK(queue.pop(queued) && queued.vk == 0x10, "Shift's own down");
	CHECK(queue.pop(queued) && queued.vk == kW &&
			queued.flags == controls::key_event_flag::kShift, "W under Shift");
	CHECK(queue.pop(queued) && queued.vk == kW &&
			queued.flags == (controls::key_event_flag::kShift | controls::key_event_flag::kRepeat),
			"W's repeat");
	CHECK(queue.pop(queued) && queued.vk == kUp &&
			(queued.flags & controls::key_event_flag::kExtended) != 0, "the Up arrow, extended");
	CHECK(!queue.pop(queued), "the releases queued nothing");

	// Keypad Enter: VK_RETURN on its E0 scancode, its state at 269, its scan code 269.
	device.event(key(0x0D, platform::kScancodeExtended | 0x1C, true));
	CHECK(device.key_down(269), "keypad Enter's extended state");
	CHECK(queue.pop(queued) && controls::key_event_scan_code(queued) == 269, "keypad Enter scans as 269");
	device.event(key(0x0D, platform::kScancodeExtended | 0x1C, false));

	// A focus change clears the key state alone: W held through it reads up until its next
	// key-down; the queue keeps the key-down it already holds.
	device.event(key(kW, kWScancode, true));
	device.event(focus(false));
	CHECK(!device.key_down(kW), "W cleared by the focus loss");
	CHECK(!device.pressed("move_forward"), "the row reads up");
	CHECK(queue.pop(queued) && queued.vk == kW, "the queue keeps W's down");
	device.event(focus(true));
	device.event(key(kW, kWScancode, true, true)); // the OS's repeat sets it again
	CHECK(device.key_down(kW), "W's next key-down sets it again");
	device.event(key(kW, kWScancode, false));
	while (queue.pop(queued)) {}

	// The whole input reset's device half: a key held through it reads up, the latch is clear.
	device.event(key(kW, kWScancode, true));
	device.reset_key_state();
	CHECK(!device.key_down(kW), "W cleared by the reset");
	device.event(key(kW, kWScancode, false));
	while (queue.pop(queued)) {}

	// The mouse rows read the buttons' held words: the left button fires attack_1's binding.
	CHECK(!device.binding_active("attack_1"), "no button held");
	device.event(button(platform::MouseButton::Left, true));
	CHECK(device.binding_active("attack_1"), "the left button holds the fire row");
	device.event(button(platform::MouseButton::Left, false));
	CHECK(!device.binding_active("attack_1"), "released");

	// The key scan names a key-down's row: B fires binoculars (no modifier row claims it).
	bool sign = true;
	CHECK(device.event_code('B', sign) == code_of("binoculars"), "B's row is binoculars");
	CHECK(!sign, "no Shift, no sign");

	if (g_failures != 0) {
		std::printf("platform_input_device_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::puts("platform_input_device_test: ok");
	return 0;
}
