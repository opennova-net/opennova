#include "nova_mnu_spinlist.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

void NovaMnuSpinList::_ready() {
	value_mount_ = Object::cast_to<Control>(get_node_or_null(NodePath("Value")));
	update_value_cell();
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

void NovaMnuSpinList::update_value_cell() {
	if (value_mount_ == nullptr) {
		return;
	}
	if (index_ >= 0 && index_ < static_cast<int>(visuals_.size())) {
		mnu_show_item_cell(value_mount_, visuals_[index_]);
	} else {
		mnu_show_item_cell(value_mount_, MnuItemVisual());
	}
}

void NovaMnuSpinList::set_item_visuals(const std::vector<MnuItemVisual> &p_visuals) {
	visuals_ = p_visuals;
	if (index_ >= static_cast<int>(visuals_.size())) {
		index_ = visuals_.empty() ? 0 : static_cast<int>(visuals_.size()) - 1;
	}
	update_value_cell();
}

void NovaMnuSpinList::set_values(const PackedStringArray &p_values) {
	// Shell-supplied text values: build text-kind visuals so the cell shows them.
	visuals_.clear();
	for (int i = 0; i < p_values.size(); ++i) {
		MnuItemVisual v;
		v.kind = MnuItemVisual::TEXT;
		v.text = p_values[i];
		visuals_.push_back(v);
	}
	if (index_ >= static_cast<int>(visuals_.size())) {
		index_ = visuals_.empty() ? 0 : static_cast<int>(visuals_.size()) - 1;
	}
	update_value_cell();
}

void NovaMnuSpinList::set_value_index(int p_index) {
	if (visuals_.empty()) {
		index_ = 0;
		update_value_cell();
		return;
	}
	if (p_index < 0) {
		p_index = 0;
	} else if (p_index >= static_cast<int>(visuals_.size())) {
		p_index = static_cast<int>(visuals_.size()) - 1;
	}
	index_ = p_index;
	update_value_cell();
}

String NovaMnuSpinList::get_value() const {
	if (index_ < 0 || index_ >= static_cast<int>(visuals_.size())) {
		return String();
	}
	return visuals_[index_].text;
}

String NovaMnuSpinList::get_value_attr() const {
	if (index_ < 0 || index_ >= static_cast<int>(visuals_.size())) {
		return String();
	}
	return visuals_[index_].value;
}

void NovaMnuSpinList::cycle(int p_delta) {
	if (visuals_.empty()) {
		return;
	}
	const int n = static_cast<int>(visuals_.size());
	index_ = ((index_ + p_delta) % n + n) % n; // wrap, handling negative delta
	update_value_cell();
	behavior_.play_click();
	emit_signal("value_changed", index_, get_value());
	behavior_.notify_value(String(get_name()), "spinlist", index_, get_value());
}

void NovaMnuSpinList::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_values", "values"), &NovaMnuSpinList::set_values);
	ClassDB::bind_method(D_METHOD("set_value_index", "index"), &NovaMnuSpinList::set_value_index);
	ClassDB::bind_method(D_METHOD("get_value_index"), &NovaMnuSpinList::get_value_index);
	ClassDB::bind_method(D_METHOD("get_value_attr"), &NovaMnuSpinList::get_value_attr);
	ClassDB::bind_method(D_METHOD("get_value"), &NovaMnuSpinList::get_value);
	ClassDB::bind_method(D_METHOD("get_value_count"), &NovaMnuSpinList::get_value_count);
	ClassDB::bind_method(D_METHOD("cycle", "delta"), &NovaMnuSpinList::cycle);

	ADD_SIGNAL(MethodInfo("value_changed", PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
}
