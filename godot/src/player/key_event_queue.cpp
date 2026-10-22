#include "player/key_event_queue.h"

#include "mnu/controls_model.h"

#include <godot_cpp/classes/global_constants.hpp>

#include <runtime/controls/binding_set.h> // is_extended_vk

using namespace godot;

void KeyEventQueue::push_key(const Ref<InputEventKey> &p_event, bool p_ctrl_down,
		bool p_shift_down) {
	if (p_event.is_null()) return;
	const Key physical = p_event->get_physical_keycode();
	int vk = ControlsModel::vk_from_godot_key(
			static_cast<int>(physical != KEY_NONE ? physical : p_event->get_keycode()));
	if (vk == 0) return;
	namespace flag = opennova::controls::key_event_flag;
	uint16_t flags = 0;
	if (p_ctrl_down) flags |= flag::kCtrl;
	if (p_shift_down) flags |= flag::kShift;
	// The message's extended-key bit: keypad Enter is VK_RETURN on it (the binding model's
	// 269 is the scan's code for it, controls::key_event_scan_code).
	if (vk == 269) {
		vk = 0x0D;
		flags |= flag::kExtended;
	} else if (opennova::controls::is_extended_vk(vk)) {
		flags |= flag::kExtended;
	}
	if (p_event->is_echo()) flags |= flag::kRepeat;
	events_[static_cast<size_t>(queue_.write_index())] = p_event;
	queue_.push(vk, flags);
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

Ref<InputEventKey> KeyEventQueue::pop_pass_key() {
	Ref<InputEventKey> key = pop_key();
	if (key.is_valid()) latch_.set(last_);
	return key;
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
	ClassDB::bind_method(D_METHOD("pop_pass_key"), &KeyEventQueue::pop_pass_key);
	ClassDB::bind_method(D_METHOD("last_scan_code"), &KeyEventQueue::last_scan_code);
	ClassDB::bind_method(D_METHOD("reset"), &KeyEventQueue::reset);
	ClassDB::bind_method(D_METHOD("is_empty"), &KeyEventQueue::is_empty);
}
