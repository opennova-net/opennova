#include "nova_mnu_multiline_edit.h"

#include "nova_mnu_scroll.h"

#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuMultilineEdit::_ready() {
	if (authored_scrollbar_ != nullptr) {
		VScrollBar *native = get_v_scroll_bar();
		if (native != nullptr) {
			authored_scrollbar_->link_range_target(authored_scrollbar_->get_path_to(native));
			native->set_modulate(Color(1, 1, 1, 0));
			native->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		}
	}
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
