#include "nova_mnu_multiline_edit.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuMultilineEdit::_ready() {
	if (behavior_.edit_mode) {
		// Inert while authoring.
		return;
	}
	connect("text_changed", callable_mp(this, &NovaMnuMultilineEdit::on_text_changed));
}

void NovaMnuMultilineEdit::on_text_changed() {
	behavior_.notify_value(String(get_name()), "multiline", -1, get_text());
}

void NovaMnuMultilineEdit::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_readonly", "readonly"), &NovaMnuMultilineEdit::set_readonly);
	ClassDB::bind_method(D_METHOD("get_readonly"), &NovaMnuMultilineEdit::get_readonly);
}
