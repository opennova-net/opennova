#pragma once

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <array>
#include <runtime/controls/key_event_queue.h>

namespace godot {

// The device's key queue (engine controls::KeyEventQueue carries the witness: the 128-slot
// ring with no full test, the flag word, the reset) as the object the shell feeds and the
// input frame's keyboard pass drains. The device leg here: a Godot key-down into the slot's
// VK and flag word, and the Godot event itself kept beside its slot, which the drain's legs
// (the chat line, the special-key chain, the deploy keys) read as they read a live key.
// One queue per process, owned by the input router (PlayerInputRouter::key_events); a test
// may make its own.
class KeyEventQueue : public RefCounted {
	GDCLASS(KeyEventQueue, RefCounted)

public:
	// One key-down, an OS repeat (Godot's echo) included: its VK (the physical key, else
	// the label, as the binding model reads keys), Ctrl and Shift as the key state holds
	// them, the extended bit by the VK (keypad Enter queues VK 13 with it), the repeat bit
	// from the echo. A key with no VK queues nothing.
	void push_key(const Ref<InputEventKey> &p_event, bool p_ctrl_down, bool p_shift_down);
	// The next unread key-down, null when the read index meets the write index (a reader
	// outside the input frame's keyboard pass: the start-mission splash's exit test).
	Ref<InputEventKey> pop_key();
	// One input frame's keyboard pass over the queue (engine controls::run_keyboard_pass
	// carries the order and the witness), each leg the legs object's method:
	// ring_release_pass(), ring_chat_key(key) -> bool, ring_special_key(key) -> bool,
	// ring_binding_code(scan_code) -> int (-1 none), ring_row_event(key, code, scan_code);
	// each taken event's key-press latch is set (engine controls::KeyPressLatch).
	void drain(Object *p_legs);
	// The latch the pass sets and the input router's release pass reads; the queue's reset
	// leaves it, the whole input reset clears it.
	opennova::controls::KeyPressLatch &press_latch() { return latch_; }
	void clear_press_latch() { latch_.clear(); }
	// The binding scan's key for the last popped event (controls::key_event_scan_code:
	// its VK, keypad Enter 269); 0 before any pop.
	int last_scan_code() const { return opennova::controls::key_event_scan_code(last_); }
	void reset();
	bool is_empty() const { return queue_.empty(); }

protected:
	static void _bind_methods();

private:
	opennova::controls::KeyEventQueue queue_;
	std::array<Ref<InputEventKey>, opennova::controls::KeyEventQueue::kCapacity> events_;
	opennova::controls::KeyEvent last_;
	opennova::controls::KeyPressLatch latch_;
};

} // namespace godot
