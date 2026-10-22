#pragma once

#include <array>
#include <cstdint>

namespace opennova::controls {

// One key-down message as the device's key queue holds it: the VK, the character byte
// (0 at the queue: the WM_CHAR leg fills it on the last queued event), and the flag word:
// Ctrl held, Shift held, the message's extended-key bit (lParam bit 24) and its
// previous-state bit (lParam bit 30), set on every OS auto-repeat.
// [orig: Input_QueueKeyEvent @0x760c10 - the VK @0x760c3c, the character cleared @0x760c3e,
//  Ctrl (key state 17) @0x760c4b / 0x800 @0x760c4d, Shift (key state 16) @0x760c59 / 0x200
//  @0x760c5b, the extended bit @0x760c6b / 0x100 @0x760c6d, the repeat bit @0x760c79 / 0x80
//  @0x760c7b]
namespace key_event_flag {
inline constexpr uint16_t kCtrl = 0x800;
inline constexpr uint16_t kShift = 0x200;
inline constexpr uint16_t kExtended = 0x100;
inline constexpr uint16_t kRepeat = 0x80;
} // namespace key_event_flag

struct KeyEvent {
	uint8_t vk = 0;
	uint8_t character = 0;
	uint16_t flags = 0;
};

// The key the binding scan matches an event on: its VK, but keypad Enter (VK_RETURN with
// the extended bit) is 269, the code its binding records hold.
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the low byte @0x49d2ea,
//  Input_IsExtendedEnterKey @0x760f40 (the 0x100 test @0x760f44, VK 13 @0x760f4c; the call
//  @0x49d310) -> 269 @0x49d31c]
inline int key_event_scan_code(const KeyEvent &event) {
	return (event.flags & key_event_flag::kExtended) != 0 && event.vk == 0x0D ? 269 : event.vk;
}

// The device's key queue: every key-down message (WM_KEYDOWN and WM_SYSKEYDOWN, an OS
// auto-repeat included) joins a 128-slot ring, and the input frame's keyboard pass takes
// them one at a time; a release queues nothing. The ring has no full test: the write
// index wraps over unread slots, so 128 unread events read as an empty queue and each
// further one leaves the newest readable from the read index on. The queue's disable word
// is never set (its one writer, Input_SetKeyQueueDisabled, has no caller), so every press
// queues.
// [orig: Input_HandleKeyPress @0x760c90 (the key state @0x760cbb, the call @0x760cc2);
//  Input_QueueKeyEvent @0x760c10 (the disable word @0x760c12, the slot @0x760c20, the
//  write index's `& 0x7F` @0x760c2d); Input_DequeueKeyEvent @0x760d60 (empty when the read
//  index meets the write index @0x760d6b, the read index's `& 0x7F` @0x760d7a);
//  Input_SetKeyQueueDisabled @0x760f30 (no xref); Input_ResetKeyQueue @0x760e00]
class KeyEventQueue {
public:
	static constexpr int kCapacity = 128;

	void push(int vk, uint16_t flags) {
		KeyEvent &slot = slots_[write_];
		write_ = static_cast<uint8_t>((write_ + 1) & (kCapacity - 1));
		slot.vk = static_cast<uint8_t>(vk);
		slot.character = 0;
		slot.flags = flags;
	}
	// The next unread event; false when the read index meets the write index.
	bool pop(KeyEvent &out) {
		if (read_ == write_) return false;
		out = slots_[read_];
		read_ = static_cast<uint8_t>((read_ + 1) & (kCapacity - 1));
		return true;
	}
	bool empty() const { return read_ == write_; }
	// Both indices to 0 [orig: Input_ResetKeyQueue @0x760e00..0x760e07].
	void reset() { read_ = write_ = 0; }

private:
	std::array<KeyEvent, kCapacity> slots_{};
	uint8_t read_ = 0;
	uint8_t write_ = 0;
};

} // namespace opennova::controls
