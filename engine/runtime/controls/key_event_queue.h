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
	// Both indices to 0 [orig: Input_ResetKeyQueue @0x760e00..0x760e07]. The embedder runs
	// it at each of the original's resets its shell has: the whole input reset (a menu
	// screen's close, the Escape row's and the pick's cases, every mission start, a
	// joiner's at the end of its game-start wait, whose loop dequeues and drops each key
	// but Escape), the pause row's case behind its flip, and the start-mission splash
	// as it begins, whose exit test then dequeues the key that ended it. The game-mode
	// push, the options and control-remap screens (the remap list's refresh and its
	// display update among them), the session connect waits, the session's random-seed
	// request and the save file's server wait reset it too, each ahead of a reset above
	// (the mission start's, or the menu screen close's).
	// [orig: Input_ResetAllState @0x497ED0 (the call @0x497ED0); Input_HandleActionBinding
	//  @0x49AD40 (case 25: the flip @0x49B52D, the call @0x49B53E); Game_StartMission
	//  @0x524360 (the joiner's wait: the call @0x526288, the dequeue @0x5262A2, Escape
	//  @0x5262AB); Game_ShowStartMissionSplash @0x520820 (the call @0x52085B, the exit
	//  test's dequeue @0x520905); UI_NavHistoryPush @0x52BB30; UI_OptionsScreenInit
	//  @0x554800 (the call @0x554DDF); UI_ControlsRemapArmHandler @0x55D560 (the call
	//  @0x55D5BE); CGameSession_WaitForNWUValidation @0x4D0F20 (the call @0x4D11F7);
	//  CNapiGameSession_ConnectToNovaWorld @0x4D4640 (the call @0x4D4BCA);
	//  CNapiGameSession_ConnectOrHost @0x4D4F10 (the call @0x4D54A1);
	//  CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 (the call @0x51EA05);
	//  SaveFile_SendAndWaitForServerAck @0x5204B0 (the call @0x520543);
	//  UI_RefreshControlMappingList @0x55B320 (the call @0x55B6D2);
	//  UI_UpdateControlMappingDisplay @0x55B700 (the call @0x55B9A7)]
	void reset() { read_ = write_ = 0; }
	// The slot the next push writes and the slot the next pop reads, for an embedder
	// that keeps its device's own record of each queued message beside the slot.
	int write_index() const { return write_; }
	int read_index() const { return read_; }

private:
	std::array<KeyEvent, kCapacity> slots_{};
	uint8_t read_ = 0;
	uint8_t write_ = 0;
};

// One event the keyboard pass queues (Input_QueueEvent's id: the dispatch code, the sign in
// bit 31), in key order: a row's, with the key it came from (the press scan's event key,
// key_event_scan_code, or the latched key of the release pass, BindingSet::release_pass), its
// arguments 0; or the special-key chain's deploy pick (event 12), its pick in the first
// argument (`param`: a spawn zone's letter index + 1, 0 for the default spawn or a RESTART,
// 0xFFFE for the auto team).
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the release pass's queue @0x49d2b1, the
//  press scan's @0x49d42f / @0x49d488; Input_HandleSpecialKeys @0x49c5c0 - Input_QueueEvent(12,
//  ...) @0x49c658 (0), @0x49c8a3 (0), @0x49ca14 (65534), @0x49ca73 (key - 65 + 1)]
struct KeyRowEvent {
	int code = -1;
	bool sign = false;
	int key = 0;
	bool release = false;
	int32_t param = 0;
};

// The keyboard pass's key-press latch (g_KeyPressLatch), one byte per key: the pass sets the
// byte of every event it dequeues, ahead of the capture and special-key branches (an event an
// open text line or the special-key chain takes too), and its release pass clears it once the
// key reads up (BindingSet::release_pass). The queue's own reset leaves it; the whole input
// reset clears it, so a key held through a menu close dispatches no release.
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the set @0x49d2ed on the event's low byte
//  @0x49d2ea, the release pass's test @0x49d240 and clear @0x49d2b9; Input_ResetAllState
//  @0x497ED0 - the memset @0x497F08]
class KeyPressLatch {
public:
	static constexpr int kKeyCount = 256;

	void set(const KeyEvent &event) { set_[event.vk] = true; }
	bool is_set(int key) const { return key >= 0 && key < kKeyCount && set_[key]; }
	void release(int key) {
		if (key >= 0 && key < kKeyCount) set_[key] = false;
	}
	void clear() { set_.fill(false); }

private:
	std::array<bool, kKeyCount> set_{};
};

// The keyboard pass's legs, the embedder's: the release pass, the open text line's take, the
// special-key chain (inmatch::special_key_chain), the binding scan's row for a key, and the
// row's event. dequeued hands over each event the pass takes, with its ring slot, ahead of
// the event's legs, for an embedder that keeps its device's record of each message beside
// the slot (KeyEventQueue::read_index).
class KeyboardPassLegs {
public:
	virtual ~KeyboardPassLegs() = default;
	virtual void release_pass() = 0;
	virtual void dequeued(const KeyEvent &event, int slot) = 0;
	virtual bool chat_key(const KeyEvent &event) = 0;
	virtual bool special_key(const KeyEvent &event) = 0;
	// The dispatch code of the row the scan fires for the key, -1 for none.
	virtual int binding_code(int scan_code) = 0;
	virtual void row_event(const KeyEvent &event, int code, int scan_code) = 0;
};

// One input frame's keyboard pass: the release pass first, then every queued event in order:
// its key-press latch set, then an open text line takes it, else the special-key chain, else
// the binding scan fires the row it matches (keypad Enter scanning as 269).
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the release pass @0x49d22f..0x49d2cc, the
//  dequeue loop @0x49d2d2..0x49d4a0: the latch @0x49d2ed, the capture word's test per event
//  @0x49d2e3 (Chat_HandleInputChar @0x49d498), Input_HandleSpecialKeys @0x49d2fb and a take's
//  skip @0x49d305, Input_IsExtendedEnterKey @0x49d310, the press scan's Input_QueueEvent
//  @0x49d42f / @0x49d488]
inline void run_keyboard_pass(KeyEventQueue &queue, KeyPressLatch &latch, KeyboardPassLegs &legs) {
	legs.release_pass();
	for (;;) {
		const int slot = queue.read_index();
		KeyEvent event;
		if (!queue.pop(event)) return;
		latch.set(event);
		legs.dequeued(event, slot);
		if (legs.chat_key(event) || legs.special_key(event)) continue;
		const int scan_code = key_event_scan_code(event);
		const int code = legs.binding_code(scan_code);
		if (code >= 0) legs.row_event(event, code, scan_code);
	}
}

} // namespace opennova::controls
