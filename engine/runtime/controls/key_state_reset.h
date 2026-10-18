#pragma once

// A menu close's reset of the keyboard key state the binding samplers read.

#include <array>
#include <cstddef>
#include <functional>

namespace opennova::controls {

// Every menu close clears the key-state array, the keys and the extended keys
// (the 512-byte memset; the binding scan reads keypad Enter as VK 269, the
// extended Enter): a key held through it reads up until its next key-down
// message, a fresh press or an OS repeat, or until it is released and pressed
// again. The mouse buttons and the joystick are not in that array and hold
// through the reset; the modifiers are in it, Shift's sign bit included.
// [orig: UI_CloseMenuScreen @0x54E63F and WeaponLoadout_ApplyFromBuffer
//  @0x565D01 -> Input_ResetAllState @0x497ED0 -> Input_ClearKeyState
//  @0x497ED5 (memset(keyState, 0, 0x200) @0x760DE7, extendedKeyState at its
//  +0x100); only Input_HandleKeyPress sets a key again @0x760CAA / @0x760CBB,
//  from WM_KEYDOWN and WM_SYSKEYDOWN alike (Game_WindowProc @0x76276A /
//  @0x7627E5), OS repeats included; the keypad Enter remap
//  Input_ProcessKeyboardEvents @0x49D31C, Shift's sign bit @0x49D46A;
//  Input_PumpAndCenterCursor writes only the mouse deltas and the cursor
//  @0x761724..0x76173A]
class KeyStateReset {
public:
	static constexpr int kKeyCount = 512;

	// The reset: every key `down` reports held reads up from here.
	void reset(const std::function<bool(int vk)> &down) {
		for (int vk = 1; vk < kKeyCount; ++vk) cleared_[static_cast<std::size_t>(vk)] = down(vk);
	}
	// A key-down message for `vk` (a press or an OS repeat) sets the key again.
	void key_down(int vk) {
		if (vk > 0 && vk < kKeyCount) cleared_[static_cast<std::size_t>(vk)] = false;
	}
	// The held state the samplers read for `vk`, whose device state is `down`:
	// a cleared key reads up until a key-down message or its release.
	bool held(int vk, bool down) {
		if (vk <= 0 || vk >= kKeyCount || !cleared_[static_cast<std::size_t>(vk)]) return down;
		if (!down) cleared_[static_cast<std::size_t>(vk)] = false;
		return false;
	}

private:
	std::array<bool, kKeyCount> cleared_{};
};

} // namespace opennova::controls
