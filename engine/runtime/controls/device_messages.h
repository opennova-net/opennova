#pragma once

#include <cstdint>

#include <base/platform/input_sample.h>
#include <runtime/controls/key_event_queue.h>

// The window procedure's messages onto retail's input events: a raw device event (the platform's
// input sample, base/platform/input_sample.h) becomes the event number the mouse walk takes, the
// wheel's WHEEL_DELTA units the notch walk adds up, or the key queue's entry.
namespace opennova::controls {

// The mouse event a button message raises: left down 2 / up 4 / double click 0x400, right 8 /
// 0x10 / 0x800, middle 0x20 / 0x40 / 0x1000; 0 for a button retail's window procedure never sees
// (the X buttons). A double click is the second press of a run (the window class's CS_DBLCLKS
// style sends it as its own message, in place of a down).
// [orig: Input_DispatchMouseEvent @0x761470 - WM_LBUTTONDOWN 2 @0x76151A, WM_LBUTTONUP 4
//  @0x761524, WM_RBUTTONDOWN 8 @0x76152E, WM_RBUTTONUP 0x10 @0x761538, WM_MBUTTONDOWN 0x20
//  @0x761542, WM_MBUTTONUP 0x40 @0x76154C, WM_LBUTTONDBLCLK 0x400 @0x761556, WM_RBUTTONDBLCLK
//  0x800 @0x761560, WM_MBUTTONDBLCLK 0x1000 @0x76156A; WinMain @0x763A80 (the class style 8,
//  CS_DBLCLKS, @0x763AFC)]
uint32_t mouse_button_event(const platform::MouseButtonEvent &button);

// The left button's down: the event the PowerThrow windup test rides (a double click's 0x400
// is not one).
inline constexpr uint32_t kMouseLeftDownEvent = 2;

// The wheel's notch events: each whole +WHEEL_DELTA the remainder reaches raises 0x100 (wheel
// up), each whole -WHEEL_DELTA 0x200 (wheel down) (WheelRemainder walks them).
// [orig: Input_DispatchMouseEvent @0x761470 - 0x100 @0x76158A, 0x200 @0x7615BB]
inline constexpr uint32_t kMouseWheelUpEvent = 0x100;
inline constexpr uint32_t kMouseWheelDownEvent = 0x200;

// A wheel turn's signed delta in WHEEL_DELTA units (120 per notch), the WM_MOUSEWHEEL word the
// notch walk adds to its remainder.
// [orig: Input_DispatchMouseEvent @0x761470 - `sar eax,10h` / `add eax,
//  g_MouseState.wheelRemainder` @0x7614AB..0x7614AE]
int32_t wheel_delta_units(const platform::MouseWheelEvent &wheel);

// A key-down's entry in the key queue: its VK (0 = the key has none and queues nothing) and the
// flag word, Ctrl and Shift as the key state holds them (the caller's reads), the extended-key
// bit from the scancode (lParam bit 24; keypad Enter is VK_RETURN with it, which the binding
// scan reads as 269) or the VK of a navigation key a device reports without it
// (is_extended_vk), and the repeat bit on an OS auto-repeat.
// [orig: Input_QueueKeyEvent @0x760c10 - the VK @0x760c3c, Ctrl (key state 17) @0x760c4b /
//  0x800 @0x760c4d, Shift (key state 16) @0x760c59 / 0x200 @0x760c5b, the extended bit @0x760c6b
//  / 0x100 @0x760c6d, the repeat bit @0x760c79 / 0x80 @0x760c7b]
struct QueuedKey {
	int vk = 0;
	uint16_t flags = 0;
};
QueuedKey queued_key(const platform::KeyEvent &key, bool ctrl_down, bool shift_down);

} // namespace opennova::controls
