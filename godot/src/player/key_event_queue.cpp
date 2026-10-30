#include "player/key_event_queue.h"

#include "player/input_device.h"

#include <runtime/controls/device_messages.h>

using namespace godot;

void KeyEventQueue::push_key(const Ref<InputEventKey> &p_event, bool p_ctrl_down,
		bool p_shift_down) {
	if (p_event.is_null()) return;
	// The key's entry is the engine's (controls::queued_key carries the witness): its VK and
	// flag word from the device's key event.
	const opennova::controls::QueuedKey key = opennova::controls::queued_key(
			InputDevice::key_event(**p_event), p_ctrl_down, p_shift_down);
	if (key.vk == 0) return;
	events_[static_cast<size_t>(queue_.write_index())] = p_event;
	queue_.push(key.vk, key.flags);
}

Ref<InputEventKey> KeyEventQueue::pop_key() {
	const size_t slot = static_cast<size_t>(queue_.read_index());
	opennova::controls::KeyEvent event;
	if (!queue_.pop(event)) return Ref<InputEventKey>();
	last_ = event;
	Ref<InputEventKey> key = events_[slot];
	events_[slot].unref();
	return key;
}

void KeyEventQueue::drain(Object *p_legs) {
	if (p_legs == nullptr) return;
	// The engine's pass over the shell's legs: each dequeued event hands its slot's key over
	// (and frees the slot) ahead of the event's legs.
	struct Legs final : opennova::controls::KeyboardPassLegs {
		KeyEventQueue &queue;
		Object &legs;
		Ref<InputEventKey> key;
		Legs(KeyEventQueue &p_queue, Object &p_legs) : queue(p_queue), legs(p_legs) {}
		void release_pass() override { legs.call("ring_release_pass"); }
		void dequeued(const opennova::controls::KeyEvent &p_event, int p_slot) override {
			queue.last_ = p_event;
			key = queue.events_[static_cast<size_t>(p_slot)];
			queue.events_[static_cast<size_t>(p_slot)].unref();
		}
		bool chat_key(const opennova::controls::KeyEvent &) override {
			return static_cast<bool>(legs.call("ring_chat_key", key));
		}
		bool special_key(const opennova::controls::KeyEvent &) override {
			return static_cast<bool>(legs.call("ring_special_key", key));
		}
		int binding_code(int p_scan_code) override {
			return static_cast<int>(static_cast<int64_t>(legs.call("ring_binding_code", p_scan_code)));
		}
		void row_event(const opennova::controls::KeyEvent &, int p_code, int p_scan_code) override {
			legs.call("ring_row_event", key, p_code, p_scan_code);
		}
	};
	Legs legs(*this, *p_legs);
	opennova::controls::run_keyboard_pass(queue_, latch_, legs);
}

void KeyEventQueue::reset() {
	queue_.reset();
	for (Ref<InputEventKey> &event : events_) event.unref();
	last_ = opennova::controls::KeyEvent{};
}

void KeyEventQueue::_bind_methods() {
	ClassDB::bind_method(D_METHOD("push_key", "event", "ctrl_down", "shift_down"),
			&KeyEventQueue::push_key);
	ClassDB::bind_method(D_METHOD("pop_key"), &KeyEventQueue::pop_key);
	ClassDB::bind_method(D_METHOD("drain", "legs"), &KeyEventQueue::drain);
	ClassDB::bind_method(D_METHOD("last_scan_code"), &KeyEventQueue::last_scan_code);
	ClassDB::bind_method(D_METHOD("reset"), &KeyEventQueue::reset);
	ClassDB::bind_method(D_METHOD("is_empty"), &KeyEventQueue::is_empty);
}
