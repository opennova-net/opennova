#include <runtime/controls/device_messages.h>

#include <runtime/controls/binding_set.h> // is_extended_vk
#include <runtime/controls/player_actions.h> // kWheelDelta

#include <cmath>

namespace opennova::controls {

uint32_t mouse_button_event(const platform::MouseButtonEvent &button) {
	// The window procedure's button messages [orig: Input_DispatchMouseEvent @0x761470 - the
	//  messages 0x201..0x209 onto the events @0x76151A..0x76156A]: a run's second press is the
	//  double-click message, which takes the down's place.
	const bool twice = button.down && button.clicks >= 2;
	switch (button.button) {
		case platform::MouseButton::Left: return twice ? 0x400u : button.down ? 2u : 4u;
		case platform::MouseButton::Right: return twice ? 0x800u : button.down ? 8u : 0x10u;
		case platform::MouseButton::Middle: return twice ? 0x1000u : button.down ? 0x20u : 0x40u;
		default: return 0u;
	}
}

int32_t wheel_delta_units(const platform::MouseWheelEvent &wheel) {
	// WHEEL_DELTA (120) per notch [orig: Input_DispatchMouseEvent @0x761470 - the +120 walk
	//  @0x761575, the -120 walk @0x7615AC].
	return static_cast<int32_t>(std::lround(wheel.y * static_cast<float>(kWheelDelta)));
}

QueuedKey queued_key(const platform::KeyEvent &key, bool ctrl_down, bool shift_down) {
	// [orig: Input_QueueKeyEvent @0x760c10 - Ctrl @0x760c4d, Shift @0x760c5b, the extended bit
	//  @0x760c6d, the repeat bit @0x760c7b]
	QueuedKey out;
	const int vk = static_cast<int>(key.key);
	if (vk <= 0 || vk > 0xFF) return out;
	out.vk = vk;
	namespace flag = key_event_flag;
	if (ctrl_down) out.flags |= flag::kCtrl;
	if (shift_down) out.flags |= flag::kShift;
	// The message's extended-key bit is the scancode's; the navigation keys a device reports
	// without one carry it by their VK as their own scancode would.
	if (platform::scancode_extended(key.scancode) || is_extended_vk(vk))
		out.flags |= flag::kExtended;
	if (key.repeat) out.flags |= flag::kRepeat;
	return out;
}

} // namespace opennova::controls
