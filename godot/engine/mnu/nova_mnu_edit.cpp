#include "nova_mnu_edit.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuEdit::_ready() {
	if (behavior_.edit_mode) {
		// Inert while authoring: stay non-editable, wire nothing.
		return;
	}
	connect("text_changed", callable_mp(this, &NovaMnuEdit::on_text_changed));
	connect("text_submitted", callable_mp(this, &NovaMnuEdit::on_text_submitted));
}

void NovaMnuEdit::on_text_changed(const String &p_text) {
	behavior_.notify_value(String(get_name()), "edit", -1, p_text);
}

void NovaMnuEdit::on_text_submitted(const String &p_text) {
	behavior_.play_click();
	behavior_.notify_value(String(get_name()), "edit", -1, p_text);
}

void NovaMnuEdit::_bind_methods() {
	// text/get_text and the text_changed/text_submitted signals come from LineEdit;
	// we only expose the MNU-specific hotkey accessor.
	ClassDB::bind_method(D_METHOD("get_hotkey"), &NovaMnuEdit::get_hotkey);
}
