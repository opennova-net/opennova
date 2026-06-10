#include "nova_mnu_spinlist.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

void NovaMnuSpinList::_ready() {
	value_label_ = Object::cast_to<Label>(get_node_or_null(NodePath("Value")));
	update_label();
	if (behavior_.edit_mode) {
		// Inert while authoring: the value shows but the buttons are not wired.
		return;
	}
	Node *up = get_node_or_null(NodePath("SpinUp"));
	Node *down = get_node_or_null(NodePath("SpinDown"));
	if (up != nullptr) {
		up->connect("pressed", callable_mp(this, &NovaMnuSpinList::on_spin_up));
	}
	if (down != nullptr) {
		down->connect("pressed", callable_mp(this, &NovaMnuSpinList::on_spin_down));
	}
	connect("mouse_entered", callable_mp(this, &NovaMnuSpinList::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuSpinList::on_sound_mouse_exited));
}
void NovaMnuSpinList::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuSpinList::on_sound_mouse_exited() {
	behavior_.play_mouseout();
}

void NovaMnuSpinList::on_spin_up() {
	cycle(1);
}

void NovaMnuSpinList::on_spin_down() {
	cycle(-1);
}

void NovaMnuSpinList::update_label() {
	if (value_label_ != nullptr) {
		value_label_->set_text(get_value());
	}
}

void NovaMnuSpinList::set_values(const PackedStringArray &p_values) {
	values_ = p_values;
	if (index_ >= values_.size()) {
		index_ = values_.is_empty() ? 0 : values_.size() - 1;
	}
	update_label();
}

void NovaMnuSpinList::set_value_index(int p_index) {
	if (values_.is_empty()) {
		index_ = 0;
		update_label();
		return;
	}
	if (p_index < 0) {
		p_index = 0;
	} else if (p_index >= values_.size()) {
		p_index = values_.size() - 1;
	}
	index_ = p_index;
	update_label();
}

String NovaMnuSpinList::get_value() const {
	if (values_.is_empty() || index_ < 0 || index_ >= values_.size()) {
		return String();
	}
	return values_[index_];
}

void NovaMnuSpinList::cycle(int p_delta) {
	if (values_.is_empty()) {
		return;
	}
	const int n = values_.size();
	index_ = ((index_ + p_delta) % n + n) % n; // wrap, handling negative delta
	update_label();
	behavior_.play_click();
	emit_signal("value_changed", index_, get_value());
	behavior_.notify_value(String(get_name()), "spinlist", index_, get_value());
}

void NovaMnuSpinList::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_values", "values"), &NovaMnuSpinList::set_values);
	ClassDB::bind_method(D_METHOD("set_value_index", "index"), &NovaMnuSpinList::set_value_index);
	ClassDB::bind_method(D_METHOD("get_value_index"), &NovaMnuSpinList::get_value_index);
	ClassDB::bind_method(D_METHOD("get_value"), &NovaMnuSpinList::get_value);
	ClassDB::bind_method(D_METHOD("get_value_count"), &NovaMnuSpinList::get_value_count);
	ClassDB::bind_method(D_METHOD("cycle", "delta"), &NovaMnuSpinList::cycle);

	ADD_SIGNAL(MethodInfo("value_changed", PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
}
