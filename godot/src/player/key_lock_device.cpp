#include "player/key_lock_device.h"

#include <godot_cpp/classes/display_server.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace godot::key_lock_device {

namespace {

// A keyboard is there to read and force: the display is no headless one (a test or
// automation run), fixed for the process.
bool keyboard_present() {
	static const bool present = [] {
		DisplayServer *display = DisplayServer::get_singleton();
		return display != nullptr && display->get_name() != "headless";
	}();
	return present;
}

} // namespace

bool on(int p_vk) {
#ifdef _WIN32
	if (!keyboard_present()) return false;
	BYTE state[256] = {};
	return ::GetKeyboardState(state) != 0 && state[p_vk] != 0;
#else
	(void)p_vk;
	return false;
#endif
}

bool set(int p_vk, bool p_on) {
#ifdef _WIN32
	if (!keyboard_present()) return false;
	BYTE state[256] = {};
	if (::GetKeyboardState(state) == 0 || (state[p_vk] != 0) == p_on) return false;
	INPUT inputs[2] = {};
	for (INPUT &input : inputs) {
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = static_cast<WORD>(p_vk);
		input.ki.wScan = static_cast<WORD>(p_vk);
	}
	inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
	inputs[1].ki.time = 10;
	return ::SendInput(2, inputs, sizeof(INPUT)) != 0;
#else
	(void)p_vk;
	(void)p_on;
	return false;
#endif
}

} // namespace godot::key_lock_device
